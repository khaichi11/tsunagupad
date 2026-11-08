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
    double scale;           /* skala monitor virtual di tata letak */
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
     *
     * Dipakai metadata. Kalau kursor ikut digambar, posisinya hanya terbarui
     * saat ada isi layar yang berubah, jadi kursor terlihat tersendat. Dengan
     * metadata, Mutter mengirim posisi dan gambar kursor terus-menerus walau
     * layarnya diam, dan src/pwcap.c menempelkannya sendiri ke setiap frame. */
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "cursor-mode",
                          g_variant_new_uint32(v->hide_cursor ? 0 : 2));
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

/* Menempatkan monitor virtual di tata letak desktop.
 *
 * Mutter baru membuat monitornya setelah ukuran aliran dinegosiasikan, dan
 * monitor itu belum masuk susunan layar sampai ada konfigurasi yang memuatnya.
 * Tanpa langkah ini layarnya gelap, kursor tidak pernah bisa ke sana, dan
 * susunannya hilang setiap kali sesi dimulai. Susunan layar yang sudah ada
 * disalin apa adanya; yang ditambahkan hanya monitor virtual di sisi kanan. */
#define STATE_TYPE "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"

typedef struct {
    char *mode;
    int width, height;
    double *scales;         /* faktor skala yang didukung mode ini */
    int n_scales;
} ModeInfo;

static void free_mode(gpointer data);

static GHashTable *current_modes(GVariant *monitors, char **virtual_connector)
{
    GHashTable *modes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_mode);
    GVariantIter iter;
    GVariant *spec = NULL, *list = NULL, *props = NULL;
    g_variant_iter_init(&iter, monitors);
    while (g_variant_iter_next(&iter, "(@(ssss)@a(siiddada{sv})@a{sv})", &spec, &list, &props)) {
        char *connector = NULL;
        g_variant_get_child(spec, 0, "s", &connector);
        GVariantIter miter;
        char *id = NULL;
        gint32 w = 0, h = 0;
        gdouble refresh = 0, preferred = 0;
        GVariant *scales = NULL, *mprops = NULL;
        g_variant_iter_init(&miter, list);
        while (g_variant_iter_next(&miter, "(siidd@ad@a{sv})", &id, &w, &h, &refresh,
                                   &preferred, &scales, &mprops)) {
            gboolean current = FALSE;
            g_variant_lookup(mprops, "is-current", "b", &current);
            if (current) {
                ModeInfo *info = g_new0(ModeInfo, 1);
                info->mode = g_strdup(id);
                info->width = w;
                info->height = h;
                gsize n = 0;
                const gdouble *list = g_variant_get_fixed_array(scales, &n, sizeof(gdouble));
                if (list && n) {
                    info->scales = g_memdup2(list, n * sizeof(gdouble));
                    info->n_scales = (int) n;
                }
                g_hash_table_insert(modes, g_strdup(connector), info);
            }
            g_clear_pointer(&id, g_free);
            g_clear_pointer(&scales, g_variant_unref);
            g_clear_pointer(&mprops, g_variant_unref);
        }
        if (connector && g_str_has_prefix(connector, "Meta-") && virtual_connector &&
            !*virtual_connector)
            *virtual_connector = g_strdup(connector);
        g_free(connector);
        g_clear_pointer(&spec, g_variant_unref);
        g_clear_pointer(&list, g_variant_unref);
        g_clear_pointer(&props, g_variant_unref);
    }
    return modes;
}

static void free_mode(gpointer data)
{
    ModeInfo *info = data;
    g_free(info->mode);
    g_free(info->scales);
    g_free(info);
}

gboolean virtmon_place(Virtmon *v, GError **error)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        GVariant *state = g_dbus_connection_call_sync(
            v->bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL,
            G_VARIANT_TYPE(STATE_TYPE), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
        if (!state) {
            g_usleep(400 * 1000);
            continue;
        }
        guint32 serial = 0;
        GVariant *monitors = NULL, *logical = NULL, *props = NULL;
        g_variant_get(state, "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})",
                      &serial, &monitors, &logical, &props);

        char *virt = NULL;
        GHashTable *modes = current_modes(monitors, &virt);

        if (!virt) {
            g_hash_table_destroy(modes);
            g_variant_unref(monitors);
            g_variant_unref(logical);
            g_variant_unref(props);
            g_variant_unref(state);
            g_usleep(400 * 1000);
            continue;   /* monitor baru ada setelah aliran dinegosiasikan */
        }

        /* Skala monitor utama dicari dulu: skala monitor virtual harus sama,
         * kalau tidak GNOME mengubah cara XWayland menggambar dan aplikasi X11
         * di monitor lain ikut berubah ukurannya. */
        gdouble main_scale = 1.0;
        {
            GVariantIter first;
            gint32 fx = 0, fy = 0;
            gdouble fscale = 1.0;
            guint32 ftransform = 0;
            gboolean fprimary = FALSE;
            GVariant *fmembers = NULL, *fprops = NULL;
            g_variant_iter_init(&first, logical);
            while (g_variant_iter_next(&first, "(iidub@a(ssss)@a{sv})", &fx, &fy, &fscale,
                                       &ftransform, &fprimary, &fmembers, &fprops)) {
                if (fprimary && fscale > 0)
                    main_scale = fscale;
                g_clear_pointer(&fmembers, g_variant_unref);
                g_clear_pointer(&fprops, g_variant_unref);
            }
        }

        /* Skala yang paling mendekati, dari yang didukung mode monitor virtual. */
        gdouble want_scale = 1.0;
        {
            ModeInfo *vinfo = g_hash_table_lookup(modes, virt);
            gdouble best = -1;
            for (int i = 0; vinfo && i < vinfo->n_scales; i++) {
                gdouble d = ABS(vinfo->scales[i] - main_scale);
                if (best < 0 || d < best) {
                    best = d;
                    want_scale = vinfo->scales[i];
                }
            }
        }
        v->scale = want_scale;

        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
        int right = 0;
        gboolean placed = FALSE, rescaled = FALSE, any_monitor = FALSE;
        GVariantIter liter;
        gint32 lx = 0, ly = 0;
        gdouble scale = 1.0;
        guint32 transform = 0;
        gboolean primary = FALSE;
        GVariant *members = NULL, *lprops = NULL;
        g_variant_iter_init(&liter, logical);
        while (g_variant_iter_next(&liter, "(iidub@a(ssss)@a{sv})", &lx, &ly, &scale, &transform,
                                   &primary, &members, &lprops)) {
            GVariantBuilder mon;
            g_variant_builder_init(&mon, G_VARIANT_TYPE("a(ssa{sv})"));
            GVariantIter it;
            GVariant *member = NULL;
            int width = 0;
            gboolean is_virtual = FALSE;
            g_variant_iter_init(&it, members);
            while (g_variant_iter_next(&it, "@(ssss)", &member)) {
                char *connector = NULL;
                g_variant_get_child(member, 0, "s", &connector);
                ModeInfo *info = g_hash_table_lookup(modes, connector);
                if (g_strcmp0(connector, virt) == 0) {
                    placed = TRUE;
                    is_virtual = TRUE;
                }
                if (info && !width)
                    width = info->width;
                GVariantBuilder empty;
                g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
                g_variant_builder_add(&mon, "(ssa{sv})", connector,
                                      info ? info->mode : "", &empty);
                g_free(connector);
                g_variant_unref(member);
            }
            /* Susunan tersimpan dari GNOME bisa saja memberi monitor virtual
             * skala yang berbeda; itu dibetulkan di sini tanpa mengubah
             * monitor lain. */
            if (is_virtual && ABS(scale - want_scale) > 0.001) {
                scale = want_scale;
                rescaled = TRUE;
            }
            g_variant_builder_add(&out, "(iiduba(ssa{sv}))", lx, ly, scale, transform,
                                  primary, &mon);
            any_monitor = TRUE;
            if (scale > 0)
                right = MAX(right, lx + (int) (width / scale));
            g_clear_pointer(&members, g_variant_unref);
            g_clear_pointer(&lprops, g_variant_unref);
        }

        gboolean ok = TRUE;
        if (placed && rescaled) {
            GVariant *reply = g_dbus_connection_call_sync(
                v->bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
                g_variant_new("(uua(iiduba(ssa{sv}))a{sv})", serial, 1u, &out, NULL),
                NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
            ok = reply != NULL;
            g_clear_pointer(&reply, g_variant_unref);
        } else if (!placed) {
            /* Monitor virtual ditaruh di sebelah kanan layar paling kanan. */
            ModeInfo *info = g_hash_table_lookup(modes, virt);
            GVariantBuilder mon, empty;
            g_variant_builder_init(&mon, G_VARIANT_TYPE("a(ssa{sv})"));
            g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&mon, "(ssa{sv})", virt, info ? info->mode : "", &empty);

            /* Di mesin tanpa layar sama sekali (misalnya Jetson headless),
             * monitor virtual ini satu-satunya, jadi dia yang jadi utama. */
            g_variant_builder_add(&out, "(iiduba(ssa{sv}))", right, 0, want_scale, 0u,
                                  !any_monitor, &mon);

            GVariant *reply = g_dbus_connection_call_sync(
                v->bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
                /* 1 = sementara, tidak ditulis ke monitors.xml.
                 *
                 * Menyimpannya sendiri justru berbahaya: setiap kali jumlah
                 * monitor berubah, GNOME mencari konfigurasi tersimpan yang
                 * cocok, dan satu entri lama yang buruk bisa menarik resolusi
                 * serta skala layar lain ikut berubah. Kalau pengguna menggeser
                 * monitor iPad lewat Pengaturan, Pengaturan sendiri yang
                 * menyimpannya, dan sesi berikutnya tinggal memakainya. */
                g_variant_new("(uua(iiduba(ssa{sv}))a{sv})", serial, 1u, &out, NULL),
                NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
            ok = reply != NULL;
            g_clear_pointer(&reply, g_variant_unref);
        } else {
            g_variant_builder_clear(&out);
        }


        g_hash_table_destroy(modes);
        g_free(virt);
        g_variant_unref(monitors);
        g_variant_unref(logical);
        g_variant_unref(props);
        g_variant_unref(state);
        return ok;
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                "Monitor virtual tidak muncul di tata letak");
    return FALSE;
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
