/* Kontrol dari iPad di Windows.
 *   Sentuh, mouse, keyboard -> SendInput (keyboard lewat scancode, jadi tidak
 *                              bergantung pada layout; teks lewat Unicode).
 *   Apple Pencil            -> pena sintetis Windows Ink (tekanan 0-1024 dan
 *                              kemiringan), sama seperti pena tablet sungguhan,
 *                              atau jadi mouse biasa bila tidak tersedia.
 * Bila dijalankan layanan sebagai SYSTEM, masukan mengikuti desktop yang
 * sedang aktif, termasuk layar login dan layar UAC.
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <math.h>
#include <string.h>

#include "win.h"

/* ---------- pena sintetis (Windows 10 1809+) ---------- */

typedef HANDLE TpPenDevice;

typedef struct {
    POINTER_INPUT_TYPE type;
    union {
        POINTER_TOUCH_INFO touchInfo;
        POINTER_PEN_INFO penInfo;
    } u;
} TpPointerTypeInfo;

typedef TpPenDevice (WINAPI *CreateSyntheticFn)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
typedef BOOL (WINAPI *InjectSyntheticFn)(TpPenDevice, const TpPointerTypeInfo *, UINT32);
typedef void (WINAPI *DestroySyntheticFn)(TpPenDevice);

static CreateSyntheticFn create_synthetic;
static InjectSyntheticFn inject_synthetic;
static DestroySyntheticFn destroy_synthetic;

static gboolean load_synthetic(void)
{
    static gboolean tried, ok;
    if (tried)
        return ok;
    tried = TRUE;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    create_synthetic = (CreateSyntheticFn) (void *) GetProcAddress(user32, "CreateSyntheticPointerDevice");
    inject_synthetic = (InjectSyntheticFn) (void *) GetProcAddress(user32, "InjectSyntheticPointerInput");
    destroy_synthetic = (DestroySyntheticFn) (void *) GetProcAddress(user32, "DestroySyntheticPointerDevice");
    ok = create_synthetic && inject_synthetic && destroy_synthetic;
    return ok;
}

/* Dipakai `tsunagupad capabilities`: "tablet" bila Pencil bisa jadi pena. */
gboolean caps_has_uinput(void)
{
    return load_synthetic();
}

#define PEN_PRESSURE_MAX 1024
/* Windows menganggap pena hilang bila lama tidak ada kabar; posisi terakhir
 * diulang saat Pencil diam di atas atau di layar. */
#define PEN_REPEAT_MS 60

struct _Input {
    App *app;
    TpPenDevice pen;            /* NULL: Pencil dipakai sebagai mouse */
    TpPointerTypeInfo last;     /* bingkai pena terakhir, untuk diulang */
    gint64 last_pen;
    guint repeat;
    gboolean pen_near, pen_touch, pen_mouse_down;
    double scroll_x, scroll_y;  /* satuan 1/120 klik roda */
    guint buttons;              /* bitmask tombol mouse yang sedang ditekan */
    GHashTable *keys;           /* scancode tombol yang sedang ditekan */
    gint64 rect_checked;
};

/* ---------- desktop yang sedang aktif ---------- */

static HDESK current_desktop;

/* Layar login, layar kunci, dan UAC berada di desktop "Winlogon". Masukan
 * hanya sampai ke desktop yang sedang tampil, jadi thread ini ikut pindah.
 * Pengguna biasa tidak bisa membuka desktop itu; hanya proses SYSTEM yang
 * dijalankan layanan Tsunagu-Pad yang bisa. */
void win_follow_input_desktop(gboolean force)
{
    static gint64 last;
    gint64 now = g_get_monotonic_time();
    if (!force && now - last < 400 * 1000)
        return;
    last = now;
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input)
        return;
    wchar_t want[128] = { 0 }, have[128] = { 0 };
    DWORD n = 0;
    GetUserObjectInformationW(input, UOI_NAME, want, sizeof want - sizeof(wchar_t), &n);
    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    if (cur)
        GetUserObjectInformationW(cur, UOI_NAME, have, sizeof have - sizeof(wchar_t), &n);
    if (wcscmp(want, have) != 0 && SetThreadDesktop(input)) {
        if (current_desktop)
            CloseDesktop(current_desktop);
        current_desktop = input;
        return;
    }
    CloseDesktop(input);
}

static void send_inputs(INPUT *in, UINT n)
{
    if (SendInput(n, in, sizeof *in) == n)
        return;
    /* Gagal biasanya karena desktop baru saja berganti (mis. layar kunci). */
    win_follow_input_desktop(TRUE);
    SendInput(n, in, sizeof *in);
}

/* ---------- koordinat ---------- */

static void refresh_region(Input *in)
{
    App *a = in->app;
    gint64 now = g_get_monotonic_time();
    if (a->crop || !a->monitor || now - in->rect_checked < G_USEC_PER_SEC)
        return;
    in->rect_checked = now;
    /* Monitor bisa digeser di Pengaturan Tampilan selama sesi berjalan,
     * terutama layar kedua; letaknya dibaca ulang sekali sedetik. */
    WinMonitor m;
    if (win_monitor_by_device(a->monitor, &m)) {
        a->rx = m.x;
        a->ry = m.y;
        a->rw = m.width;
        a->rh = m.height;
    }
}

/* Koordinat 0..1 dari iPad -> piksel desktop virtual di dalam area yang dibagikan. */
static void region_point(Input *in, double nx, double ny, int *x, int *y)
{
    App *a = in->app;
    refresh_region(in);
    *x = a->rx + (int) lround(CLAMP(nx, 0.0, 1.0) * (a->rw - 1));
    *y = a->ry + (int) lround(CLAMP(ny, 0.0, 1.0) * (a->rh - 1));
}

static const char *str_member(JsonObject *o, const char *name)
{
    const char *s = json_object_get_string_member_with_default(o, name, "");
    return s ? s : "";
}

/* ---------- mouse ---------- */

static void mouse_move(int x, int y)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = MAX(GetSystemMetrics(SM_CXVIRTUALSCREEN), 2), vh = MAX(GetSystemMetrics(SM_CYVIRTUALSCREEN), 2);
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG) (((gint64) (x - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = (LONG) (((gint64) (y - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send_inputs(&in, 1);
}

/* Nomor tombol gaya X seperti versi Linux: 1 kiri, 2 tengah, 3 kanan. */
static void mouse_button(guint b, gboolean down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    switch (b) {
    case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 3: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    default: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    }
    send_inputs(&in, 1);
}

static void mouse_wheel(gboolean horizontal, int delta)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD) delta;
    send_inputs(&in, 1);
}

static void pointer_to(Input *in, JsonObject *o)
{
    if (!json_object_has_member(o, "x"))
        return;
    int x, y;
    region_point(in, json_object_get_double_member_with_default(o, "x", 0),
                 json_object_get_double_member_with_default(o, "y", 0), &x, &y);
    mouse_move(x, y);
}

static void handle_mouse(Input *in, JsonObject *o)
{
    const char *e = str_member(o, "e");
    guint b = CLAMP(json_object_get_int_member_with_default(o, "b", 1), 1, 3);

    pointer_to(in, o);
    if (g_str_equal(e, "down")) {
        mouse_button(b, TRUE);
        in->buttons |= 1u << b;
    } else if (g_str_equal(e, "up")) {
        mouse_button(b, FALSE);
        in->buttons &= ~(1u << b);
    } else if (g_str_equal(e, "click")) {
        mouse_button(b, TRUE);
        mouse_button(b, FALSE);
    }
}

/* Windows menerima gulir halus dalam satuan 1/120 klik, jadi gerakan dua jari
 * diteruskan dengan resolusi penuh, bukan dibulatkan per klik. Ctrl+roda
 * untuk zoom tetap tepat satu klik (120). */
#define WHEEL_STEP 24

static void wheel(Input *in, double *acc, gboolean horizontal)
{
    if (*acc >= WHEEL_STEP || *acc <= -WHEEL_STEP) {
        int delta = (int) *acc;
        *acc -= delta;
        mouse_wheel(horizontal, horizontal ? delta : -delta);
    }
}

/* dy > 0 = gulir ke bawah, dx > 0 = gulir ke kanan (satuan: klik roda). */
static void scroll_event(Input *in, JsonObject *o)
{
    pointer_to(in, o);
    in->scroll_y += json_object_get_double_member_with_default(o, "dy", 0) * WHEEL_DELTA;
    in->scroll_x += json_object_get_double_member_with_default(o, "dx", 0) * WHEEL_DELTA;
    wheel(in, &in->scroll_y, FALSE);
    wheel(in, &in->scroll_x, TRUE);
}

/* ---------- keyboard ---------- */

#define EXT 0x100

/* KeyboardEvent.code (atau .key untuk tombol khusus) -> scancode set 1.
 * Cmd di keyboard iPad dipetakan ke Ctrl supaya Cmd+C/V/Z terasa sama,
 * persis seperti versi Linux. */
static const struct { const char *name; int sc; } keymap[] = {
    { "KeyA", 0x1E }, { "KeyB", 0x30 }, { "KeyC", 0x2E }, { "KeyD", 0x20 },
    { "KeyE", 0x12 }, { "KeyF", 0x21 }, { "KeyG", 0x22 }, { "KeyH", 0x23 },
    { "KeyI", 0x17 }, { "KeyJ", 0x24 }, { "KeyK", 0x25 }, { "KeyL", 0x26 },
    { "KeyM", 0x32 }, { "KeyN", 0x31 }, { "KeyO", 0x18 }, { "KeyP", 0x19 },
    { "KeyQ", 0x10 }, { "KeyR", 0x13 }, { "KeyS", 0x1F }, { "KeyT", 0x14 },
    { "KeyU", 0x16 }, { "KeyV", 0x2F }, { "KeyW", 0x11 }, { "KeyX", 0x2D },
    { "KeyY", 0x15 }, { "KeyZ", 0x2C },
    { "Digit1", 0x02 }, { "Digit2", 0x03 }, { "Digit3", 0x04 }, { "Digit4", 0x05 },
    { "Digit5", 0x06 }, { "Digit6", 0x07 }, { "Digit7", 0x08 }, { "Digit8", 0x09 },
    { "Digit9", 0x0A }, { "Digit0", 0x0B },
    { "Enter", 0x1C }, { "Escape", 0x01 }, { "Backspace", 0x0E },
    { "Tab", 0x0F }, { "Space", 0x39 }, { "Minus", 0x0C }, { "Equal", 0x0D },
    { "BracketLeft", 0x1A }, { "BracketRight", 0x1B },
    { "Backslash", 0x2B }, { "Semicolon", 0x27 }, { "Quote", 0x28 },
    { "Backquote", 0x29 }, { "Comma", 0x33 }, { "Period", 0x34 },
    { "Slash", 0x35 }, { "CapsLock", 0x3A }, { "IntlBackslash", 0x56 },
    { "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E },
    { "F5", 0x3F }, { "F6", 0x40 }, { "F7", 0x41 }, { "F8", 0x42 },
    { "F9", 0x43 }, { "F10", 0x44 }, { "F11", 0x57 }, { "F12", 0x58 },
    { "ArrowUp", EXT | 0x48 }, { "ArrowDown", EXT | 0x50 }, { "ArrowLeft", EXT | 0x4B },
    { "ArrowRight", EXT | 0x4D }, { "Home", EXT | 0x47 }, { "End", EXT | 0x4F },
    { "PageUp", EXT | 0x49 }, { "PageDown", EXT | 0x51 }, { "Delete", EXT | 0x53 },
    { "Insert", EXT | 0x52 },
    { "ShiftLeft", 0x2A }, { "ShiftRight", 0x36 }, { "Shift", 0x2A },
    { "ControlLeft", 0x1D }, { "ControlRight", EXT | 0x1D }, { "Control", 0x1D },
    { "AltLeft", 0x38 }, { "AltRight", EXT | 0x38 }, { "Alt", 0x38 },
    { "MetaLeft", 0x1D }, { "MetaRight", EXT | 0x1D }, { "Meta", 0x1D },
};

static int lookup_key(const char *name)
{
    if (!name || !*name)
        return -1;
    for (guint i = 0; i < G_N_ELEMENTS(keymap); i++)
        if (strcmp(keymap[i].name, name) == 0)
            return keymap[i].sc;
    return -1;
}

static void key_scancode(int sc, gboolean down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD) (sc & 0xff);
    in.ki.wVk = (WORD) MapVirtualKeyW((UINT) ((sc & EXT ? 0xE000 : 0) | (sc & 0xff)), MAPVK_VSC_TO_VK_EX);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (sc & EXT ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    send_inputs(&in, 1);
}

static void key_event(Input *in, JsonObject *o)
{
    int sc = lookup_key(str_member(o, "code"));
    if (sc < 0)
        sc = lookup_key(str_member(o, "key"));
    if (sc < 0)
        return;
    gboolean down = g_str_equal(str_member(o, "e"), "down");
    key_scancode(sc, down);
    if (down)
        g_hash_table_add(in->keys, GINT_TO_POINTER(sc));
    else
        g_hash_table_remove(in->keys, GINT_TO_POINTER(sc));
}

static void vk_tap(WORD vk)
{
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    send_inputs(in, 2);
}

/* Teks dari keyboard layar iPad (termasuk huruf beraksen, emoji, dan hasil
 * komposisi) dikirim sebagai karakter Unicode, tidak bergantung layout. */
static void text_event(Input *in, JsonObject *o)
{
    const char *s = str_member(o, "s");
    if (!g_utf8_validate(s, -1, NULL))
        return;
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (ch == '\n' || ch == '\r') {
            vk_tap(VK_RETURN);
            continue;
        }
        if (ch == '\t') {
            vk_tap(VK_TAB);
            continue;
        }
        gunichar2 units[2];
        int n = 1;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            units[0] = (gunichar2) (0xD800 + (ch >> 10));
            units[1] = (gunichar2) (0xDC00 + (ch & 0x3ff));
            n = 2;
        } else {
            units[0] = (gunichar2) ch;
        }
        for (int i = 0; i < n; i++) {
            INPUT k[2];
            memset(k, 0, sizeof k);
            k[0].type = k[1].type = INPUT_KEYBOARD;
            k[0].ki.wScan = k[1].ki.wScan = units[i];
            k[0].ki.dwFlags = KEYEVENTF_UNICODE;
            k[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            send_inputs(k, 2);
        }
    }
}

/* ---------- Apple Pencil ---------- */

static void pen_inject(Input *in, POINTER_FLAGS flags)
{
    in->last.u.penInfo.pointerInfo.pointerFlags = flags;
    in->last.u.penInfo.pointerInfo.dwTime = 0;
    in->last.u.penInfo.pointerInfo.PerformanceCount = 0;
    if (!inject_synthetic(in->pen, &in->last, 1)) {
        win_follow_input_desktop(TRUE);
        inject_synthetic(in->pen, &in->last, 1);
    }
    in->last_pen = g_get_monotonic_time();
}

static void pen_point(Input *in, JsonArray *pt, guint len)
{
    int x, y;
    region_point(in, json_array_get_double_element(pt, 0), json_array_get_double_element(pt, 1), &x, &y);
    POINTER_PEN_INFO *p = &in->last.u.penInfo;
    p->pointerInfo.ptPixelLocation.x = x;
    p->pointerInfo.ptPixelLocation.y = y;
    double pressure = json_array_get_double_element(pt, 2);
    p->pressure = in->pen_touch ? (UINT32) (CLAMP(pressure, 0.02, 1.0) * PEN_PRESSURE_MAX) : 0;
    if (len >= 5) {
        p->tiltX = CLAMP((INT32) json_array_get_double_element(pt, 3), -90, 90);
        p->tiltY = CLAMP((INT32) json_array_get_double_element(pt, 4), -90, 90);
    }
}

static void pen_tablet(Input *in, const char *e, JsonArray *pts, guint n)
{
    gboolean down = g_str_equal(e, "down"), up = g_str_equal(e, "up");
    gboolean hover = g_str_equal(e, "hover"), leave = g_str_equal(e, "leave");

    for (guint i = 0; i < n; i++) {
        JsonArray *pt = json_array_get_array_element(pts, i);
        guint len = pt ? json_array_get_length(pt) : 0;
        if (len < 3)
            continue;
        if (down && !in->pen_touch) {
            /* Ujung pena menyentuh: kabar "masuk jangkauan" dulu, lalu tekan. */
            in->pen_touch = TRUE;
            pen_point(in, pt, len);
            if (!in->pen_near)
                pen_inject(in, POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE);
            in->pen_near = TRUE;
            pen_inject(in, POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN);
            continue;
        }
        pen_point(in, pt, len);
        in->pen_near = TRUE;
        pen_inject(in, in->pen_touch ? POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE
                                     : POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE);
    }

    if ((up || hover || leave) && in->pen_touch) {
        in->pen_touch = FALSE;
        in->last.u.penInfo.pressure = 0;
        pen_inject(in, POINTER_FLAG_INRANGE | POINTER_FLAG_UP);
    }
    if (leave && in->pen_near) {
        in->pen_near = FALSE;
        pen_inject(in, POINTER_FLAG_UPDATE);   /* tanpa INRANGE: pena menjauh */
    }
}

/* Pencil yang diam tidak mengirim apa-apa; tanpa pengulangan Windows mengira
 * pena hilang dan memutus goresan atau kursornya. */
static gboolean pen_repeat(gpointer data)
{
    Input *in = data;
    if (!in->pen_near || g_get_monotonic_time() - in->last_pen < PEN_REPEAT_MS * 1000)
        return G_SOURCE_CONTINUE;
    pen_inject(in, in->pen_touch ? POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE
                                 : POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE);
    return G_SOURCE_CONTINUE;
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
        region_point(in, json_array_get_double_element(pt, 0), json_array_get_double_element(pt, 1), &x, &y);
        mouse_move(x, y);
        if (down && !in->pen_mouse_down) {
            mouse_button(1, TRUE);
            in->pen_mouse_down = TRUE;
        }
    }
    if (release && in->pen_mouse_down) {
        mouse_button(1, FALSE);
        in->pen_mouse_down = FALSE;
    }
}

/* ---------- API ---------- */

Input *input_new(App *app)
{
    Input *in = g_new0(Input, 1);
    in->app = app;
    in->keys = g_hash_table_new(NULL, NULL);
    win_follow_input_desktop(TRUE);

    if (g_strcmp0(app->pen_mode, "mouse") != 0 && load_synthetic()) {
        in->pen = create_synthetic(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        if (in->pen) {
            in->last.type = PT_PEN;
            in->last.u.penInfo.pointerInfo.pointerType = PT_PEN;
            in->last.u.penInfo.pointerInfo.pointerId = 0;
            in->last.u.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
            in->repeat = g_timeout_add(PEN_REPEAT_MS / 2, pen_repeat, in);
        }
    }
    if (!in->pen && g_strcmp0(app->pen_mode, "tablet") == 0)
        tsunagu_event("warning", "message",
                      "Pena Windows Ink tidak tersedia (butuh Windows 10 versi 1809 atau lebih baru); "
                      "Apple Pencil dipakai sebagai mouse", NULL);
    tsunagu_event("input", "pen", in->pen ? "tablet" : "mouse", NULL);
    return in;
}

void input_handle(Input *in, JsonObject *o)
{
    const char *type = str_member(o, "type");
    win_follow_input_desktop(FALSE);

    if (g_str_equal(type, "pen")) {
        JsonArray *pts = NULL;
        if (json_object_has_member(o, "pts") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "pts")))
            pts = json_object_get_array_member(o, "pts");
        guint n = pts ? json_array_get_length(pts) : 0;
        if (in->pen)
            pen_tablet(in, str_member(o, "e"), pts, n);
        else
            pen_mouse(in, str_member(o, "e"), pts, n);
    } else if (g_str_equal(type, "mouse")) {
        handle_mouse(in, o);
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
    if (in->pen && in->pen_touch) {
        in->pen_touch = FALSE;
        in->last.u.penInfo.pressure = 0;
        pen_inject(in, POINTER_FLAG_INRANGE | POINTER_FLAG_UP);
    }
    if (in->pen && in->pen_near) {
        in->pen_near = FALSE;
        pen_inject(in, POINTER_FLAG_UPDATE);
    }
    if (in->pen_mouse_down) {
        mouse_button(1, FALSE);
        in->pen_mouse_down = FALSE;
    }
    for (guint b = 1; b <= 3; b++)
        if (in->buttons & (1u << b))
            mouse_button(b, FALSE);
    in->buttons = 0;

    GHashTableIter it;
    gpointer sc;
    g_hash_table_iter_init(&it, in->keys);
    while (g_hash_table_iter_next(&it, &sc, NULL))
        key_scancode(GPOINTER_TO_INT(sc), FALSE);
    g_hash_table_remove_all(in->keys);
    in->scroll_x = in->scroll_y = 0;
}

void input_free(Input *in)
{
    if (!in)
        return;
    input_release_all(in);
    if (in->repeat)
        g_source_remove(in->repeat);
    if (in->pen)
        destroy_synthetic(in->pen);
    g_hash_table_destroy(in->keys);
    g_free(in);
}
