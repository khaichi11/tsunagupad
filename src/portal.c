/* Sesi Wayland: tangkap layar (ScreenCast) dan kendali masukan (RemoteDesktop)
 * lewat xdg-desktop-portal. GNOME menampilkan satu jendela izin; pilihan
 * disimpan dengan restore token sehingga sambungan berikutnya langsung jalan.
 * SPDX-License-Identifier: MIT
 */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "tsunagupad.h"

#define PORTAL_NAME   "org.freedesktop.portal.Desktop"
#define PORTAL_PATH   "/org/freedesktop/portal/desktop"
#define IFACE_RD      "org.freedesktop.portal.RemoteDesktop"
#define IFACE_SC      "org.freedesktop.portal.ScreenCast"
#define IFACE_REQUEST "org.freedesktop.portal.Request"
#define IFACE_SESSION "org.freedesktop.portal.Session"

#define DEVICE_KEYBOARD 1
#define DEVICE_POINTER  2
#define SOURCE_MONITOR  1
#define CURSOR_EMBEDDED 2
#define PERSIST_UNTIL_REVOKED 2

/* Waktu tunggu: jendela izin menunggu pengguna, langkah lain harus cepat. */
#define TIMEOUT_QUICK 15
#define TIMEOUT_USER  180

struct _Portal {
    App *app;
    GDBusConnection *bus;
    char *sender;           /* nama unik bus tanpa ':', '.' diganti '_' */
    char *session;          /* object path sesi */
    guint closed_sub;
    guint counter;
    int pw_fd;
    guint node;
    int x, y, w, h;         /* posisi dan ukuran logis layar yang dipilih */
    guint devices;          /* perangkat masukan yang diizinkan */
};

typedef struct {
    GMainLoop *loop;
    gboolean got;
    guint response;         /* 0 berhasil, 1 dibatalkan pengguna, 2 lainnya */
    GVariant *results;
} Wait;

static char *token_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "tsunagupad", "portal-token", NULL);
}

static char *token_load(void)
{
    char *path = token_path(), *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        text = NULL;
    g_free(path);
    if (text)
        g_strstrip(text);
    if (text && !*text)
        g_clear_pointer(&text, g_free);
    return text;
}

static void token_clear(void)
{
    char *path = token_path();
    g_unlink(path);
    g_free(path);
}

static void token_save(const char *token)
{
    char *path = token_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, token, -1, NULL);
    g_chmod(path, 0600);
    g_free(dir);
    g_free(path);
}

static void on_response(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *signal, GVariant *params, gpointer data)
{
    Wait *w = data;
    g_variant_get(params, "(u@a{sv})", &w->response, &w->results);
    w->got = TRUE;
    g_main_loop_quit(w->loop);
}

static gboolean on_wait_timeout(gpointer data)
{
    Wait *w = data;
    g_main_loop_quit(w->loop);
    return G_SOURCE_REMOVE;
}

/* Satu permintaan portal: kirim, lalu tunggu sinyal Response dengan loop sendiri.
 * Hasilnya hanya berlaku bila response == 0. */
static gboolean request(Portal *p, const char *iface, const char *method, const char *session,
                        gboolean with_parent, GVariantBuilder *options, guint timeout_s,
                        GVariant **results, GError **error)
{
    char *token = g_strdup_printf("tsunagu%u", ++p->counter);
    char *req_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", p->sender, token);
    g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token));

    Wait w = { .loop = g_main_loop_new(NULL, FALSE) };
    guint sub = g_dbus_connection_signal_subscribe(p->bus, PORTAL_NAME, IFACE_REQUEST, "Response",
                                                   req_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                   on_response, &w, NULL);
    GVariant *params;
    if (session && with_parent)
        params = g_variant_new("(osa{sv})", session, "", options);
    else if (session)
        params = g_variant_new("(oa{sv})", session, options);
    else
        params = g_variant_new("(a{sv})", options);

    GVariant *reply = g_dbus_connection_call_sync(p->bus, PORTAL_NAME, PORTAL_PATH, iface, method, params,
                                                  G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                                                  NULL, error);
    gboolean ok = FALSE;
    if (reply) {
        g_variant_unref(reply);
        guint timer = g_timeout_add_seconds(timeout_s, on_wait_timeout, &w);
        if (!w.got)
            g_main_loop_run(w.loop);
        if (w.got)
            g_source_remove(timer);
        if (!w.got) {
            if (g_str_equal(method, "Start"))
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "Jendela izin GNOME tidak dijawab dalam %u detik", timeout_s);
            else
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Portal tidak menjawab (%s)", method);
        } else if (w.response == 1) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Izin berbagi layar dibatalkan");
        } else if (w.response != 0) {
            if (g_str_equal(method, "CreateSession"))
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "GNOME menolak sesi berbagi layar. Biasanya karena layar sedang terkunci: buka kunci, lalu coba lagi");
            else
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal menolak %s", method);
        } else {
            ok = TRUE;
        }
    }
    g_dbus_connection_signal_unsubscribe(p->bus, sub);
    if (ok && results)
        *results = w.results;
    else if (w.results)
        g_variant_unref(w.results);
    g_main_loop_unref(w.loop);
    g_free(req_path);
    g_free(token);
    return ok;
}

static void on_session_closed(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                              const char *signal, GVariant *params, gpointer data)
{
    Portal *p = data;
    tsunagu_event("error", "message", "Berbagi layar dihentikan dari GNOME", NULL);
    if (p->app->loop)
        g_main_loop_quit(p->app->loop);
}

static gboolean read_streams(Portal *p, GVariant *results, GError **error)
{
    GVariantIter *streams = NULL;
    if (!g_variant_lookup(results, "streams", "a(ua{sv})", &streams) || g_variant_iter_n_children(streams) < 1) {
        if (streams)
            g_variant_iter_free(streams);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal tidak mengirim layar");
        return FALSE;
    }
    guint node;
    GVariant *props;
    g_variant_iter_next(streams, "(u@a{sv})", &node, &props);
    p->node = node;
    int x = 0, y = 0, w = 0, h = 0;
    g_variant_lookup(props, "position", "(ii)", &x, &y);
    g_variant_lookup(props, "size", "(ii)", &w, &h);
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    g_variant_unref(props);
    g_variant_iter_free(streams);
    return TRUE;
}

Portal *portal_open(App *app, GError **error)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!bus)
        return NULL;

    Portal *p = g_new0(Portal, 1);
    p->app = app;
    p->bus = bus;
    p->pw_fd = -1;
    p->sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
    g_strdelimit(p->sender, ".", '_');

    GVariant *results = NULL;
    GVariantBuilder opts;

    /* 1. sesi RemoteDesktop (satu sesi untuk video dan masukan) */
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    /* Token unik per proses: sesi lama yang belum sempat ditutup portal tidak menabrak sesi baru. */
    char *session_token = g_strdup_printf("tsunagupad%d", (int) getpid());
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    g_free(session_token);
    if (!request(p, IFACE_RD, "CreateSession", NULL, FALSE, &opts, TIMEOUT_QUICK, &results, error))
        goto fail;
    const char *handle = NULL;
    if (!g_variant_lookup(results, "session_handle", "&s", &handle)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal tidak membuat sesi");
        g_variant_unref(results);
        goto fail;
    }
    p->session = g_strdup(handle);
    g_variant_unref(results);

    /* 2. perangkat masukan; izin diingat sampai dicabut pengguna */
    char *restore = token_load();
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(DEVICE_KEYBOARD | DEVICE_POINTER));
    g_variant_builder_add(&opts, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
    if (restore)
        g_variant_builder_add(&opts, "{sv}", "restore_token", g_variant_new_string(restore));
    g_free(restore);
    if (!request(p, IFACE_RD, "SelectDevices", p->session, FALSE, &opts, TIMEOUT_QUICK, NULL, error))
        goto fail;

    /* 3. sumber video: satu monitor, kursor ikut tergambar di video */
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(SOURCE_MONITOR));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(CURSOR_EMBEDDED));
    if (!request(p, IFACE_SC, "SelectSources", p->session, FALSE, &opts, TIMEOUT_QUICK, NULL, error))
        goto fail;

    /* 4. mulai: di sini GNOME menampilkan jendela izin bila belum pernah disetujui */
    tsunagu_event("portal", "state", "waiting", NULL);
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    const char *wait_env = g_getenv("TSUNAGUPAD_PORTAL_TIMEOUT");   /* detik; untuk pengujian */
    guint wait = wait_env && atoi(wait_env) > 0 ? (guint) atoi(wait_env) : TIMEOUT_USER;
    if (!request(p, IFACE_RD, "Start", p->session, TRUE, &opts, wait, &results, error))
        goto fail;
    if (!read_streams(p, results, error)) {
        g_variant_unref(results);
        goto fail;
    }
    guint devices = 0;
    g_variant_lookup(results, "devices", "u", &devices);
    p->devices = devices;
    const char *new_token = NULL;
    if (!(devices & (DEVICE_KEYBOARD | DEVICE_POINTER)))
        token_clear();   /* kendali ditolak: tanya lagi di sambungan berikutnya, jangan diingat */
    else if (g_variant_lookup(results, "restore_token", "&s", &new_token) && new_token && *new_token)
        token_save(new_token);
    g_variant_unref(results);

    /* 5. soket PipeWire khusus sesi ini untuk pipewiresrc */
    GVariant *fd_opts = g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
    GUnixFDList *fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        p->bus, PORTAL_NAME, PORTAL_PATH, IFACE_SC, "OpenPipeWireRemote",
        g_variant_new("(o@a{sv})", p->session, fd_opts), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL, error);
    if (!reply)
        goto fail;
    gint index;
    g_variant_get(reply, "(h)", &index);
    g_variant_unref(reply);
    p->pw_fd = g_unix_fd_list_get(fds, index, error);
    g_object_unref(fds);
    if (p->pw_fd < 0)
        goto fail;

    p->closed_sub = g_dbus_connection_signal_subscribe(p->bus, PORTAL_NAME, IFACE_SESSION, "Closed",
                                                       p->session, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                       on_session_closed, p, NULL);
    tsunagu_event("portal", "state", "granted", NULL);
    return p;

fail:
    portal_free(p);
    return NULL;
}

void portal_free(Portal *p)
{
    if (!p)
        return;
    if (p->closed_sub)
        g_dbus_connection_signal_unsubscribe(p->bus, p->closed_sub);
    if (p->session) {
        GVariant *r = g_dbus_connection_call_sync(p->bus, PORTAL_NAME, p->session, IFACE_SESSION, "Close",
                                                  NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (r)
            g_variant_unref(r);
    }
    if (p->pw_fd >= 0)
        close(p->pw_fd);
    g_object_unref(p->bus);
    g_free(p->sender);
    g_free(p->session);
    g_free(p);
}

int portal_pipewire_fd(Portal *p)
{
    return p->pw_fd;
}

guint portal_node(Portal *p)
{
    return p->node;
}

void portal_geometry(Portal *p, int *x, int *y, int *w, int *h)
{
    *x = p->x;
    *y = p->y;
    *w = p->w;
    *h = p->h;
}

gboolean portal_can_control(Portal *p)
{
    return (p->devices & (DEVICE_KEYBOARD | DEVICE_POINTER)) != 0;
}

/* ---------- masukan: panggilan sekali jalan, jawabannya diabaikan ---------- */

static GVariant *no_options(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
}

static void notify(Portal *p, const char *method, GVariant *params)
{
    g_dbus_connection_call(p->bus, PORTAL_NAME, PORTAL_PATH, IFACE_RD, method, params, NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

void portal_motion(Portal *p, double x, double y)
{
    notify(p, "NotifyPointerMotionAbsolute",
           g_variant_new("(o@a{sv}udd)", p->session, no_options(), p->node, x, y));
}

void portal_button(Portal *p, int evdev, gboolean down)
{
    notify(p, "NotifyPointerButton",
           g_variant_new("(o@a{sv}iu)", p->session, no_options(), evdev, down ? 1u : 0u));
}

/* axis: 0 vertikal, 1 horizontal; steps > 0 = ke bawah / ke kanan */
void portal_axis(Portal *p, int axis, int steps)
{
    notify(p, "NotifyPointerAxisDiscrete",
           g_variant_new("(o@a{sv}ui)", p->session, no_options(), (guint) axis, steps));
}

void portal_key(Portal *p, int evdev, gboolean down)
{
    notify(p, "NotifyKeyboardKeycode",
           g_variant_new("(o@a{sv}iu)", p->session, no_options(), evdev, down ? 1u : 0u));
}

void portal_keysym(Portal *p, guint32 keysym, gboolean down)
{
    notify(p, "NotifyKeyboardKeysym",
           g_variant_new("(o@a{sv}iu)", p->session, no_options(), (gint32) keysym, down ? 1u : 0u));
}
