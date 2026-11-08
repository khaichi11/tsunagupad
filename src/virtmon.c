/* Layar kedua di Wayland: monitor virtual dari Mutter.
 *
 * GNOME dapat membuat monitor yang tidak terhubung ke perangkat keras apa pun
 * lewat org.gnome.Mutter.ScreenCast.RecordVirtual, jalur yang juga dipakai
 * GNOME Remote Desktop. Monitor itu muncul di Pengaturan Tampilan dengan nama
 * Meta-0, jendela bisa diseret ke sana, dan isinya dikirim sebagai node
 * PipeWire. Masukan dikirim balik lewat sesi RemoteDesktop yang sama dengan
 * koordinat relatif terhadap monitor itu.
 *
 * Ini hanya tersedia di Wayland. Di Xorg berkas ini tidak pernah dipakai.
 *
 * SPDX-License-Identifier: MIT
 */
#include "tsunagupad.h"

#define MUTTER_RD      "org.gnome.Mutter.RemoteDesktop"
#define MUTTER_RD_PATH "/org/gnome/Mutter/RemoteDesktop"
#define MUTTER_SC      "org.gnome.Mutter.ScreenCast"
#define MUTTER_SC_PATH "/org/gnome/Mutter/ScreenCast"

struct _Virtmon {
    GDBusConnection *bus;
    char *rd_session;       /* sesi RemoteDesktop, dipakai untuk masukan */
    char *sc_session;
    char *stream;           /* jalur stream, dipakai sebagai acuan koordinat */
    guint32 node;
    int width, height;
    gboolean hide_cursor;
};

static gboolean is_wayland(void)
{
    return g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0 ||
           g_getenv("WAYLAND_DISPLAY") != NULL;
}

gboolean virtmon_available(void)
{
    if (!is_wayland())
        return FALSE;       /* layar kedua hanya ada di Wayland */
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return FALSE;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, MUTTER_SC, MUTTER_SC_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", MUTTER_SC, "Version"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 1500, NULL, NULL);
    gboolean ok = reply != NULL;
    g_clear_pointer(&reply, g_variant_unref);
    g_object_unref(bus);
    return ok;
}

typedef struct {
    GMainLoop *loop;
    guint32 node;
    gboolean got;
} NodeWait;

static void on_stream_added(GDBusConnection *bus, const char *sender, const char *path,
                            const char *iface, const char *signal, GVariant *params,
                            gpointer data)
{
    NodeWait *w = data;
    g_variant_get(params, "(u)", &w->node);
    w->got = TRUE;
    g_main_loop_quit(w->loop);
}

static gboolean node_timeout(gpointer data)
{
    g_main_loop_quit(((NodeWait *) data)->loop);
    return G_SOURCE_REMOVE;
}

static GVariant *mutter_call(Virtmon *v, const char *name, const char *path, const char *iface,
                             const char *method, GVariant *args, const char *reply_type,
                             GError **error)
{
    return g_dbus_connection_call_sync(v->bus, name, path, iface, method, args,
                                       reply_type ? G_VARIANT_TYPE(reply_type) : NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
}

Virtmon *virtmon_open(int width, int height, gboolean hide_cursor, GError **error)
{
    Virtmon *v = g_new0(Virtmon, 1);
    v->width = width;
    v->height = height;
    v->hide_cursor = hide_cursor;
    v->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!v->bus) {
        virtmon_close(v);
        return NULL;
    }

    GVariant *reply = mutter_call(v, MUTTER_RD, MUTTER_RD_PATH, MUTTER_RD, "CreateSession",
                                  NULL, "(o)", error);
    if (!reply) {
        virtmon_close(v);
        return NULL;
    }
    g_variant_get(reply, "(o)", &v->rd_session);
    g_variant_unref(reply);

    reply = mutter_call(v, MUTTER_RD, v->rd_session, "org.freedesktop.DBus.Properties", "Get",
                        g_variant_new("(ss)", MUTTER_RD ".Session", "SessionId"), "(v)", error);
    if (!reply) {
        virtmon_close(v);
        return NULL;
    }
    GVariant *inner = NULL;
    g_variant_get(reply, "(v)", &inner);
    const char *session_id = g_variant_get_string(inner, NULL);

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "remote-desktop-session-id",
                          g_variant_new_string(session_id));
    g_variant_unref(inner);
    g_variant_unref(reply);

    reply = mutter_call(v, MUTTER_SC, MUTTER_SC_PATH, MUTTER_SC, "CreateSession",
                        g_variant_new("(a{sv})", &b), "(o)", error);
    if (!reply) {
        virtmon_close(v);
        return NULL;
    }
    g_variant_get(reply, "(o)", &v->sc_session);
    g_variant_unref(reply);

    /* Nilai cursor-mode pada Mutter: 0 sembunyi, 1 ikut digambar, 2 metadata.
     * Di layar kedua kursor harus terlihat, karena penunjuk bisa saja digerakkan
     * dari laptop dan iPad tidak punya cara lain untuk tahu posisinya. */
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "cursor-mode",
                          g_variant_new_uint32(v->hide_cursor ? 0 : 1));
    reply = mutter_call(v, MUTTER_SC, v->sc_session, MUTTER_SC ".Session", "RecordVirtual",
                        g_variant_new("(a{sv})", &b), "(o)", error);
    if (!reply) {
        virtmon_close(v);
        return NULL;
    }
    g_variant_get(reply, "(o)", &v->stream);
    g_variant_unref(reply);

    NodeWait wait = { g_main_loop_new(NULL, FALSE), 0, FALSE };
    guint sub = g_dbus_connection_signal_subscribe(v->bus, MUTTER_SC, MUTTER_SC ".Stream",
                                                   "PipeWireStreamAdded", v->stream, NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NONE, on_stream_added,
                                                   &wait, NULL);
    reply = mutter_call(v, MUTTER_RD, v->rd_session, MUTTER_RD ".Session", "Start", NULL, NULL,
                        error);
    if (!reply) {
        g_dbus_connection_signal_unsubscribe(v->bus, sub);
        g_main_loop_unref(wait.loop);
        virtmon_close(v);
        return NULL;
    }
    g_variant_unref(reply);

    guint timer = g_timeout_add_seconds(5, node_timeout, &wait);
    g_main_loop_run(wait.loop);
    g_source_remove(timer);
    g_dbus_connection_signal_unsubscribe(v->bus, sub);
    g_main_loop_unref(wait.loop);
    if (!wait.got) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "Monitor virtual tidak memberi aliran gambar");
        virtmon_close(v);
        return NULL;
    }
    v->node = wait.node;
    return v;
}

guint32 virtmon_node(Virtmon *v) { return v->node; }

void virtmon_size(Virtmon *v, int *w, int *h)
{
    *w = v->width;
    *h = v->height;
}

static void notify(Virtmon *v, const char *method, GVariant *args)
{
    g_dbus_connection_call(v->bus, MUTTER_RD, v->rd_session, MUTTER_RD ".Session", method, args,
                           NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

/* x dan y dalam 0..1 terhadap monitor virtual. */
void virtmon_pointer(Virtmon *v, double x, double y)
{
    notify(v, "NotifyPointerMotionAbsolute",
           g_variant_new("(sdd)", v->stream, CLAMP(x, 0.0, 1.0) * v->width,
                         CLAMP(y, 0.0, 1.0) * v->height));
}

void virtmon_button(Virtmon *v, int button, gboolean press)
{
    notify(v, "NotifyPointerButton", g_variant_new("(ib)", button, press));
}

void virtmon_scroll(Virtmon *v, int dx, int dy)
{
    if (dy)
        notify(v, "NotifyPointerAxisDiscrete", g_variant_new("(ui)", 0u, dy));
    if (dx)
        notify(v, "NotifyPointerAxisDiscrete", g_variant_new("(ui)", 1u, dx));
}

void virtmon_key(Virtmon *v, int keycode, gboolean press)
{
    notify(v, "NotifyKeyboardKeycode", g_variant_new("(ub)", (guint32) keycode, press));
}

void virtmon_close(Virtmon *v)
{
    if (!v)
        return;
    if (v->bus && v->rd_session) {
        g_dbus_connection_call_sync(v->bus, MUTTER_RD, v->rd_session, MUTTER_RD ".Session",
                                    "Stop", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    }
    g_free(v->stream);
    g_free(v->sc_session);
    g_free(v->rd_session);
    g_clear_object(&v->bus);
    g_free(v);
}
