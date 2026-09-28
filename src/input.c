/* Kontrol dari iPad.
 *   Sentuh, mouse, keyboard -> XTest (langsung ke server X).
 *   Apple Pencil            -> tablet virtual uinput (tekanan + kemiringan),
 *                              atau jadi mouse biasa bila /dev/uinput tidak bisa dibuka.
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "tsunagupad.h"

#define ABS_POS_MAX  32767
#define PRESSURE_MAX 4095

struct _Input {
    App *app;
    Display *dpy;
    int tablet;             /* fd uinput; -1 berarti Pencil dipakai sebagai mouse */
    gboolean pen_near, pen_touch, pen_mouse_down;
    double scroll_x, scroll_y;
    KeyCode spare;          /* keycode kosong untuk karakter di luar layout keyboard */
    KeyCode shift;
    guint buttons;          /* bitmask tombol mouse yang sedang ditekan */
    GHashTable *keys;       /* keycode yang sedang ditekan */
};

/* KeyboardEvent.code (atau .key untuk tombol khusus) -> kode evdev.
 * Cmd di keyboard iPad dipetakan ke Ctrl supaya Cmd+C/V/Z tetap terasa sama. */
static const struct { const char *name; int key; } keymap[] = {
    { "KeyA", KEY_A }, { "KeyB", KEY_B }, { "KeyC", KEY_C }, { "KeyD", KEY_D },
    { "KeyE", KEY_E }, { "KeyF", KEY_F }, { "KeyG", KEY_G }, { "KeyH", KEY_H },
    { "KeyI", KEY_I }, { "KeyJ", KEY_J }, { "KeyK", KEY_K }, { "KeyL", KEY_L },
    { "KeyM", KEY_M }, { "KeyN", KEY_N }, { "KeyO", KEY_O }, { "KeyP", KEY_P },
    { "KeyQ", KEY_Q }, { "KeyR", KEY_R }, { "KeyS", KEY_S }, { "KeyT", KEY_T },
    { "KeyU", KEY_U }, { "KeyV", KEY_V }, { "KeyW", KEY_W }, { "KeyX", KEY_X },
    { "KeyY", KEY_Y }, { "KeyZ", KEY_Z },
    { "Digit1", KEY_1 }, { "Digit2", KEY_2 }, { "Digit3", KEY_3 }, { "Digit4", KEY_4 },
    { "Digit5", KEY_5 }, { "Digit6", KEY_6 }, { "Digit7", KEY_7 }, { "Digit8", KEY_8 },
    { "Digit9", KEY_9 }, { "Digit0", KEY_0 },
    { "Enter", KEY_ENTER }, { "Escape", KEY_ESC }, { "Backspace", KEY_BACKSPACE },
    { "Tab", KEY_TAB }, { "Space", KEY_SPACE }, { "Minus", KEY_MINUS }, { "Equal", KEY_EQUAL },
    { "BracketLeft", KEY_LEFTBRACE }, { "BracketRight", KEY_RIGHTBRACE },
    { "Backslash", KEY_BACKSLASH }, { "Semicolon", KEY_SEMICOLON }, { "Quote", KEY_APOSTROPHE },
    { "Backquote", KEY_GRAVE }, { "Comma", KEY_COMMA }, { "Period", KEY_DOT },
    { "Slash", KEY_SLASH }, { "CapsLock", KEY_CAPSLOCK }, { "IntlBackslash", KEY_102ND },
    { "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 },
    { "F5", KEY_F5 }, { "F6", KEY_F6 }, { "F7", KEY_F7 }, { "F8", KEY_F8 },
    { "F9", KEY_F9 }, { "F10", KEY_F10 }, { "F11", KEY_F11 }, { "F12", KEY_F12 },
    { "ArrowUp", KEY_UP }, { "ArrowDown", KEY_DOWN }, { "ArrowLeft", KEY_LEFT },
    { "ArrowRight", KEY_RIGHT }, { "Home", KEY_HOME }, { "End", KEY_END },
    { "PageUp", KEY_PAGEUP }, { "PageDown", KEY_PAGEDOWN }, { "Delete", KEY_DELETE },
    { "Insert", KEY_INSERT },
    { "ShiftLeft", KEY_LEFTSHIFT }, { "ShiftRight", KEY_RIGHTSHIFT }, { "Shift", KEY_LEFTSHIFT },
    { "ControlLeft", KEY_LEFTCTRL }, { "ControlRight", KEY_RIGHTCTRL }, { "Control", KEY_LEFTCTRL },
    { "AltLeft", KEY_LEFTALT }, { "AltRight", KEY_RIGHTALT }, { "Alt", KEY_LEFTALT },
    { "MetaLeft", KEY_LEFTCTRL }, { "MetaRight", KEY_RIGHTCTRL }, { "Meta", KEY_LEFTCTRL },
};

static int lookup_key(const char *name)
{
    if (!name || !*name)
        return -1;
    for (guint i = 0; i < G_N_ELEMENTS(keymap); i++)
        if (strcmp(keymap[i].name, name) == 0)
            return keymap[i].key;
    return -1;
}

static const char *str_member(JsonObject *o, const char *name)
{
    const char *s = json_object_get_string_member_with_default(o, name, "");
    return s ? s : "";
}

/* Koordinat 0..1 dari iPad -> piksel root X11 di dalam area yang dibagikan. */
static void region_point(Input *in, double nx, double ny, int *x, int *y)
{
    App *a = in->app;
    *x = a->rx + (int) lround(CLAMP(nx, 0.0, 1.0) * (a->rw - 1));
    *y = a->ry + (int) lround(CLAMP(ny, 0.0, 1.0) * (a->rh - 1));
}

/* ---------- tablet virtual (uinput) ---------- */

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != sizeof ev) {
        /* antrean kernel penuh: event ini dibuang, event berikutnya tetap jalan */
    }
}

static int tablet_create(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    static const int keys[] = { BTN_TOOL_PEN, BTN_TOUCH, BTN_STYLUS };
    static const struct { int code, min, max, res; } axes[] = {
        { ABS_X, 0, ABS_POS_MAX, 100 },
        { ABS_Y, 0, ABS_POS_MAX, 100 },
        { ABS_PRESSURE, 0, PRESSURE_MAX, 0 },
        { ABS_TILT_X, -90, 90, 57 },
        { ABS_TILT_Y, -90, 90, 57 },
    };

    gboolean ok = ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 &&
                  ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 &&
                  ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0;
    for (guint i = 0; ok && i < G_N_ELEMENTS(keys); i++)
        ok = ioctl(fd, UI_SET_KEYBIT, keys[i]) == 0;
    for (guint i = 0; ok && i < G_N_ELEMENTS(axes); i++) {
        struct uinput_abs_setup a;
        memset(&a, 0, sizeof a);
        a.code = axes[i].code;
        a.absinfo.minimum = axes[i].min;
        a.absinfo.maximum = axes[i].max;
        a.absinfo.resolution = axes[i].res;
        ok = ioctl(fd, UI_SET_ABSBIT, axes[i].code) == 0 && ioctl(fd, UI_ABS_SETUP, &a) == 0;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209;
    setup.id.product = 0x1c41;
    setup.id.version = 1;
    g_strlcpy(setup.name, "Tsunagu-Pad Pencil", sizeof setup.name);
    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;

    if (!ok) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pen_tablet(Input *in, const char *e, JsonArray *pts, guint n)
{
    gboolean down = g_str_equal(e, "down"), up = g_str_equal(e, "up");
    gboolean hover = g_str_equal(e, "hover"), leave = g_str_equal(e, "leave");
    int fd = in->tablet;
    int sw = MAX(DisplayWidth(in->dpy, DefaultScreen(in->dpy)) - 1, 1);
    int sh = MAX(DisplayHeight(in->dpy, DefaultScreen(in->dpy)) - 1, 1);

    for (guint i = 0; i < n; i++) {
        JsonArray *pt = json_array_get_array_element(pts, i);
        guint len = pt ? json_array_get_length(pt) : 0;
        if (len < 3)
            continue;
        int x, y;
        region_point(in, json_array_get_double_element(pt, 0),
                     json_array_get_double_element(pt, 1), &x, &y);
        double pressure = json_array_get_double_element(pt, 2);

        emit(fd, EV_ABS, ABS_X, (int) ((gint64) x * ABS_POS_MAX / sw));
        emit(fd, EV_ABS, ABS_Y, (int) ((gint64) y * ABS_POS_MAX / sh));
        if (len >= 5) {
            emit(fd, EV_ABS, ABS_TILT_X, CLAMP((int) json_array_get_double_element(pt, 3), -90, 90));
            emit(fd, EV_ABS, ABS_TILT_Y, CLAMP((int) json_array_get_double_element(pt, 4), -90, 90));
        }
        if (!in->pen_near) {
            emit(fd, EV_KEY, BTN_TOOL_PEN, 1);
            in->pen_near = TRUE;
        }
        if (down && !in->pen_touch) {
            emit(fd, EV_KEY, BTN_TOUCH, 1);
            in->pen_touch = TRUE;
        }
        if (in->pen_touch)
            emit(fd, EV_ABS, ABS_PRESSURE, (int) (CLAMP(pressure, 0.02, 1.0) * PRESSURE_MAX));
        emit(fd, EV_SYN, SYN_REPORT, 0);
    }

    if ((up || hover || leave) && in->pen_touch) {
        emit(fd, EV_ABS, ABS_PRESSURE, 0);
        emit(fd, EV_KEY, BTN_TOUCH, 0);
        in->pen_touch = FALSE;
    }
    if (leave && in->pen_near) {
        emit(fd, EV_KEY, BTN_TOOL_PEN, 0);
        in->pen_near = FALSE;
    }
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void pen_mouse(Input *in, const char *e, JsonArray *pts, guint n)
{
    gboolean down = g_str_equal(e, "down");
    gboolean release = g_str_equal(e, "up") || g_str_equal(e, "leave") || g_str_equal(e, "hover");

    for (guint i = 0; i < n; i++) {
        JsonArray *pt = json_array_get_array_element(pts, i);
        if (!pt || json_array_get_length(pt) < 2)
            continue;
        int x, y;
        region_point(in, json_array_get_double_element(pt, 0),
                     json_array_get_double_element(pt, 1), &x, &y);
        XTestFakeMotionEvent(in->dpy, -1, x, y, CurrentTime);
        if (down && !in->pen_mouse_down) {
            XTestFakeButtonEvent(in->dpy, 1, True, CurrentTime);
            in->pen_mouse_down = TRUE;
        }
    }
    if (release && in->pen_mouse_down) {
        XTestFakeButtonEvent(in->dpy, 1, False, CurrentTime);
        in->pen_mouse_down = FALSE;
    }
    XFlush(in->dpy);
}

/* ---------- mouse, scroll, keyboard ---------- */

static void pointer_to(Input *in, JsonObject *o)
{
    if (!json_object_has_member(o, "x"))
        return;
    int x, y;
    region_point(in, json_object_get_double_member_with_default(o, "x", 0),
                 json_object_get_double_member_with_default(o, "y", 0), &x, &y);
    XTestFakeMotionEvent(in->dpy, -1, x, y, CurrentTime);
}

static void mouse_event(Input *in, JsonObject *o)
{
    const char *e = str_member(o, "e");
    guint b = CLAMP(json_object_get_int_member_with_default(o, "b", 1), 1, 3);

    pointer_to(in, o);
    if (g_str_equal(e, "down")) {
        XTestFakeButtonEvent(in->dpy, b, True, CurrentTime);
        in->buttons |= 1u << b;
    } else if (g_str_equal(e, "up")) {
        XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
        in->buttons &= ~(1u << b);
    } else if (g_str_equal(e, "click")) {
        XTestFakeButtonEvent(in->dpy, b, True, CurrentTime);
        XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
    }
    XFlush(in->dpy);
}

static void wheel(Input *in, double *acc, guint neg_button, guint pos_button)
{
    while (*acc >= 1.0 || *acc <= -1.0) {
        guint b = *acc > 0 ? pos_button : neg_button;
        XTestFakeButtonEvent(in->dpy, b, True, CurrentTime);
        XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
        *acc += *acc > 0 ? -1.0 : 1.0;
    }
}

/* dy > 0 = gulir ke bawah, dx > 0 = gulir ke kanan (satuan: klik roda). */
static void scroll_event(Input *in, JsonObject *o)
{
    pointer_to(in, o);
    in->scroll_y += json_object_get_double_member_with_default(o, "dy", 0);
    in->scroll_x += json_object_get_double_member_with_default(o, "dx", 0);
    wheel(in, &in->scroll_y, 4, 5);
    wheel(in, &in->scroll_x, 6, 7);
    XFlush(in->dpy);
}

static void key_event(Input *in, JsonObject *o)
{
    int key = lookup_key(str_member(o, "code"));
    if (key < 0)
        key = lookup_key(str_member(o, "key"));
    if (key < 0)
        return;

    KeyCode kc = key + 8;   /* keycode X = keycode evdev + 8 */
    gboolean down = g_str_equal(str_member(o, "e"), "down");
    XTestFakeKeyEvent(in->dpy, kc, down, CurrentTime);
    if (down)
        g_hash_table_add(in->keys, GUINT_TO_POINTER(kc));
    else
        g_hash_table_remove(in->keys, GUINT_TO_POINTER(kc));
    XFlush(in->dpy);
}

static KeyCode find_spare_keycode(Display *dpy)
{
    int min, max, per;
    XDisplayKeycodes(dpy, &min, &max);
    KeySym *map = XGetKeyboardMapping(dpy, min, max - min + 1, &per);
    KeyCode spare = 0;
    for (int kc = max; kc >= min && !spare; kc--) {
        gboolean empty = TRUE;
        for (int i = 0; i < per; i++)
            empty = empty && map[(kc - min) * per + i] == NoSymbol;
        if (empty)
            spare = kc;
    }
    XFree(map);
    return spare;
}

static void type_char(Input *in, gunichar ch)
{
    KeySym ks;
    if (ch == '\n' || ch == '\r')
        ks = XK_Return;
    else if (ch == '\t')
        ks = XK_Tab;
    else if ((ch >= 0x20 && ch <= 0x7e) || (ch >= 0xa0 && ch <= 0xff))
        ks = ch;
    else
        ks = 0x01000000 | ch;   /* keysym Unicode */

    KeyCode kc = XKeysymToKeycode(in->dpy, ks);
    gboolean shift = FALSE;
    if (kc && XkbKeycodeToKeysym(in->dpy, kc, 0, 0) == ks) {
        shift = FALSE;
    } else if (kc && XkbKeycodeToKeysym(in->dpy, kc, 0, 1) == ks) {
        shift = TRUE;
    } else if (in->spare) {
        /* karakter tidak ada di layout: pinjam keycode kosong sebentar */
        KeySym syms[1] = { ks };
        XChangeKeyboardMapping(in->dpy, in->spare, 1, syms, 1);
        XSync(in->dpy, False);
        kc = in->spare;
    } else {
        return;
    }

    if (shift)
        XTestFakeKeyEvent(in->dpy, in->shift, True, CurrentTime);
    XTestFakeKeyEvent(in->dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(in->dpy, kc, False, CurrentTime);
    if (shift)
        XTestFakeKeyEvent(in->dpy, in->shift, False, CurrentTime);
    XSync(in->dpy, False);
}

static void text_event(Input *in, JsonObject *o)
{
    const char *s = str_member(o, "s");
    if (!g_utf8_validate(s, -1, NULL))
        return;
    for (const char *p = s; *p; p = g_utf8_next_char(p))
        type_char(in, g_utf8_get_char(p));
}

/* ---------- API ---------- */

Input *input_new(App *app)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        tsunagu_event("warning", "message", "Display X11 tidak ditemukan, kontrol dari iPad dimatikan", NULL);
        return NULL;
    }
    int ev, err, major, minor;
    if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
        tsunagu_event("warning", "message", "Ekstensi XTest tidak tersedia, kontrol dari iPad dimatikan", NULL);
        XCloseDisplay(dpy);
        return NULL;
    }

    Input *in = g_new0(Input, 1);
    in->app = app;
    in->dpy = dpy;
    in->tablet = -1;
    in->keys = g_hash_table_new(NULL, NULL);
    in->shift = XKeysymToKeycode(dpy, XK_Shift_L);
    in->spare = find_spare_keycode(dpy);

    if (g_strcmp0(app->pen_mode, "mouse") != 0) {
        in->tablet = tablet_create();
        if (in->tablet < 0 && g_strcmp0(app->pen_mode, "tablet") == 0)
            tsunagu_event("warning", "message",
                       "/dev/uinput tidak bisa dibuka, tekanan Pencil tidak aktif (jalankan install.sh)", NULL);
    }
    tsunagu_event("input", "pen", in->tablet >= 0 ? "tablet" : "mouse", NULL);
    return in;
}

void input_handle(Input *in, JsonObject *o)
{
    const char *type = str_member(o, "type");

    if (g_str_equal(type, "pen")) {
        JsonArray *pts = NULL;
        if (json_object_has_member(o, "pts") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "pts")))
            pts = json_object_get_array_member(o, "pts");
        guint n = pts ? json_array_get_length(pts) : 0;
        if (in->tablet >= 0)
            pen_tablet(in, str_member(o, "e"), pts, n);
        else
            pen_mouse(in, str_member(o, "e"), pts, n);
    } else if (g_str_equal(type, "mouse")) {
        mouse_event(in, o);
    } else if (g_str_equal(type, "scroll")) {
        scroll_event(in, o);
    } else if (g_str_equal(type, "key")) {
        key_event(in, o);
    } else if (g_str_equal(type, "text")) {
        text_event(in, o);
    }
}

/* Dipanggil saat iPad putus, supaya tidak ada tombol yang "nyangkut". */
void input_release_all(Input *in)
{
    if (!in)
        return;
    if (in->tablet >= 0 && (in->pen_touch || in->pen_near)) {
        emit(in->tablet, EV_ABS, ABS_PRESSURE, 0);
        emit(in->tablet, EV_KEY, BTN_TOUCH, 0);
        emit(in->tablet, EV_KEY, BTN_TOOL_PEN, 0);
        emit(in->tablet, EV_SYN, SYN_REPORT, 0);
        in->pen_touch = in->pen_near = FALSE;
    }
    if (in->pen_mouse_down) {
        XTestFakeButtonEvent(in->dpy, 1, False, CurrentTime);
        in->pen_mouse_down = FALSE;
    }
    for (guint b = 1; b <= 3; b++)
        if (in->buttons & (1u << b))
            XTestFakeButtonEvent(in->dpy, b, False, CurrentTime);
    in->buttons = 0;

    GHashTableIter it;
    gpointer kc;
    g_hash_table_iter_init(&it, in->keys);
    while (g_hash_table_iter_next(&it, &kc, NULL))
        XTestFakeKeyEvent(in->dpy, GPOINTER_TO_UINT(kc), False, CurrentTime);
    g_hash_table_remove_all(in->keys);
    in->scroll_x = in->scroll_y = 0;
    XFlush(in->dpy);
}

void input_free(Input *in)
{
    if (!in)
        return;
    input_release_all(in);
    if (in->spare) {
        KeySym none[1] = { NoSymbol };
        XChangeKeyboardMapping(in->dpy, in->spare, 1, none, 1);
    }
    if (in->tablet >= 0) {
        ioctl(in->tablet, UI_DEV_DESTROY);
        close(in->tablet);
    }
    XCloseDisplay(in->dpy);
    g_hash_table_destroy(in->keys);
    g_free(in);
}
