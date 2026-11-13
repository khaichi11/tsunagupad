/* Tangkap layar -> encode H.264 -> kirim ke Safari iPad lewat WebRTC.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "tsunagupad.h"
#include "webrtc-lite.h"
#ifdef G_OS_WIN32
#include "win/win.h"
#endif

/* Port UDP media; install.sh membuka rentang yang sama di firewall. */
#define RTP_PORT_MIN 50000
#define RTP_PORT_MAX 50100

/* GstWebRTCICEConnectionState */
#define ICE_CONNECTED 2
#define ICE_COMPLETED 3
#define ICE_FAILED    4

/* Bitrate adaptif: mulai sedang, naik perlahan saat jaringan lega, turun cepat
 * saat paket hilang. Tanpa ini mode Tajam (14 Mbps) bisa melebihi kapasitas
 * Wi-Fi/hotspot sehingga keyframe tak pernah utuh dan iPad terus "loading". */
#define KBPS_MIN   1000
#define KBPS_START 6000

/* Satu sesi = satu pipeline. Callback GStreamer datang dari thread lain,
 * jadi sesi dihitung referensinya dan dicek masih berlaku di thread utama. */
typedef struct {
    App *app;
    guint id;
    GstElement *webrtc;
} Session;

static Session *current;

static struct {
    gint64 connected;       /* waktu ICE tersambung (µs) */
    gint64 calm_since;      /* terakhir kali ada paket hilang (µs) */
    gint64 lost;
    guint64 sent, pli;
    int rough;              /* laporan berturut-turut yang kehilangan paket */
} adapt;

static gboolean debug_enabled(void)
{
    return g_getenv("TSUNAGUPAD_DEBUG") != NULL;
}

static void session_clear(gpointer data)
{
    Session *s = data;
    gst_object_unref(s->webrtc);
}

static Session *session_ref(Session *s)
{
    return g_atomic_rc_box_acquire(s);
}

static void session_unref(gpointer s)
{
    g_atomic_rc_box_release_full(s, session_clear);
}

static void session_unref_closure(gpointer s, GClosure *closure)
{
    session_unref(s);
}

static gboolean session_alive(Session *s)
{
    return s->app->pipeline && s->app->session == s->id;
}

/* ---------- kiriman dari thread GStreamer ke thread utama ---------- */

typedef struct {
    Session *s;
    const char *type;   /* offer | ice | ice-state | error */
    char *text;
    int num;
} Post;

static void post_free(gpointer data)
{
    Post *p = data;
    session_unref(p->s);
    g_free(p->text);
    g_free(p);
}

static gboolean post_dispatch(gpointer data)
{
    Post *p = data;
    App *app = p->s->app;
    if (!session_alive(p->s))
        return G_SOURCE_REMOVE;

    if (g_str_equal(p->type, "ice-state")) {
        if (p->num == ICE_CONNECTED || p->num == ICE_COMPLETED) {
            if (!adapt.connected)
                adapt.connected = adapt.calm_since = g_get_monotonic_time();
            share_keyframe(app);
            tsunagu_event("stream", "state", "playing", NULL);
        } else if (p->num == ICE_FAILED) {
            tsunagu_event("stream", "state", "failed", NULL);
        }
        return G_SOURCE_REMOVE;
    }

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, p->type);
    if (g_str_equal(p->type, "offer")) {
        json_builder_set_member_name(b, "sdp");
        json_builder_add_string_value(b, p->text);
    } else if (g_str_equal(p->type, "ice")) {
        json_builder_set_member_name(b, "candidate");
        json_builder_add_string_value(b, p->text);
        json_builder_set_member_name(b, "sdpMLineIndex");
        json_builder_add_int_value(b, p->num);
    } else {
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, p->text);
        tsunagu_event("error", "message", p->text, NULL);
    }
    json_builder_end_object(b);
    server_send(app, b);
    g_object_unref(b);
    return G_SOURCE_REMOVE;
}

static void post(Session *s, const char *type, char *text, int num)
{
    Post *p = g_new0(Post, 1);
    p->s = session_ref(s);
    p->type = type;
    p->text = text;
    p->num = num;
    g_idle_add_full(G_PRIORITY_DEFAULT, post_dispatch, p, post_free);
}

/* ---------- bitrate adaptif dari laporan RTCP penerima ---------- */

typedef struct {
    Session *s;
    gboolean remote;        /* sudah ada laporan RTCP dari iPad */
    double fraction_lost;
    gint64 lost;
    guint64 sent, pli;
} Report;

static gboolean field_double(const GstStructure *st, const char *field, double *out)
{
    const GValue *v = gst_structure_get_value(st, field);
    if (!v || !g_value_type_transformable(G_VALUE_TYPE(v), G_TYPE_DOUBLE))
        return FALSE;
    GValue d = G_VALUE_INIT;
    g_value_init(&d, G_TYPE_DOUBLE);
    gboolean ok = g_value_transform(v, &d);
    *out = g_value_get_double(&d);
    g_value_unset(&d);
    return ok;
}

static gboolean collect_stat(GQuark field, const GValue *value, gpointer data)
{
    Report *r = data;
    if (!GST_VALUE_HOLDS_STRUCTURE(value))
        return TRUE;
    const GstStructure *st = gst_value_get_structure(value);
    double v;
    if (gst_structure_has_name(st, "remote-inbound-rtp")) {
        r->remote = TRUE;
        if (field_double(st, "fraction-lost", &v))
            r->fraction_lost = MAX(r->fraction_lost, v > 1.0 ? v / 256.0 : v);
        if (field_double(st, "packets-lost", &v))
            r->lost += (gint64) v;
    } else if (gst_structure_has_name(st, "outbound-rtp")) {
        if (field_double(st, "packets-sent", &v))
            r->sent += (guint64) v;
        if (field_double(st, "pli-count", &v))
            r->pli += (guint64) v;
    }
    return TRUE;
}

static void set_bitrate(App *app, int kbps)
{
    kbps = CLAMP(kbps, MIN(KBPS_MIN, app->kbps), app->kbps);
    if (kbps == app->kbps_now)
        return;
    app->kbps_now = kbps;
    g_object_set(app->enc, "bitrate", (guint) (kbps * encoder_bitrate_unit(app->enc_name)), NULL);
    char *text = g_strdup_printf("%d", kbps);
    tsunagu_event("bitrate", "kbps", text, NULL);
    g_free(text);
}

static void report_free(gpointer data)
{
    Report *r = data;
    session_unref(r->s);
    g_free(r);
}

static gboolean report_dispatch(gpointer data)
{
    Report *r = data;
    App *app = r->s->app;
    if (!session_alive(r->s) || !app->enc || !r->remote || !adapt.connected)
        return G_SOURCE_REMOVE;

    gint64 now = g_get_monotonic_time();
    double loss = r->fraction_lost;
    if (adapt.sent && r->sent > adapt.sent && r->lost >= adapt.lost)
        loss = MAX(loss, (double) (r->lost - adapt.lost) / (double) (r->sent - adapt.sent));
    adapt.sent = r->sent;
    adapt.lost = r->lost;
    adapt.pli = r->pli;

    /* Turun hanya kalau paket benar-benar hilang dua laporan berturut-turut.
     * Versi sebelumnya ikut turun begitu ada satu permintaan keyframe, padahal
     * permintaan itu sendiri sering muncul justru karena bitrate baru saja
     * diturunkan: gambarnya naik-turun terus tanpa jaringan benar-benar sempit. */
    if (loss > 0.04) {
        adapt.rough++;
        if (adapt.rough >= 2) {
            adapt.calm_since = now;
            set_bitrate(app, (int) (app->kbps_now * (loss > 0.12 ? 0.75 : 0.9)));
        }
    } else {
        adapt.rough = 0;
        if (now - adapt.calm_since > 2 * G_USEC_PER_SEC)
            set_bitrate(app, (int) (app->kbps_now * 1.10) + 200);
    }
    if (debug_enabled())
        g_printerr("adapt: loss=%.3f pli=%" G_GUINT64_FORMAT " sent=%" G_GUINT64_FORMAT " kbps=%d\n",
                   loss, r->pli, r->sent, app->kbps_now);
    return G_SOURCE_REMOVE;
}

static void on_stats(GstPromise *promise, gpointer data)
{
    Report *r = g_new0(Report, 1);
    r->s = session_ref(data);
    const GstStructure *reply = gst_promise_get_reply(promise);
    if (reply) {
        gst_structure_foreach(reply, collect_stat, r);
        if (debug_enabled()) {
            char *text = gst_structure_to_string(reply);
            g_printerr("stats: %s\n", text);
            g_free(text);
        }
    }
    gst_promise_unref(promise);
    g_idle_add_full(G_PRIORITY_DEFAULT, report_dispatch, r, report_free);
}

static gboolean request_stats(gpointer data)
{
    Session *s = data;
    if (!session_alive(s))
        return G_SOURCE_CONTINUE;   /* dihapus oleh share_stop() */
    GstPromise *promise = gst_promise_new_with_change_func(on_stats, session_ref(s), session_unref);
    g_signal_emit_by_name(s->webrtc, "get-stats", NULL, promise);
    return G_SOURCE_CONTINUE;
}

/* Bawaan rtprtxsend hanya 100 paket (<0,1 detik pada 14 Mbps), terlalu pendek
 * untuk menambal keyframe yang hilang di Wi-Fi. Simpan 1,5 detik. */
static void tune_rtx(GstElement *element)
{
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory || !g_str_equal(GST_OBJECT_NAME(factory), "rtprtxsend"))
        return;
    g_object_set(element, "max-size-packets", 0, "max-size-time", 1500, NULL);
    if (debug_enabled())
        g_printerr("rtx: %s disetel 1500 ms\n", GST_OBJECT_NAME(element));
}

static void on_deep_element_added(GstBin *bin, GstBin *sub_bin, GstElement *element, gpointer data)
{
    tune_rtx(element);
}

/* ---------- negosiasi WebRTC ---------- */

static void on_offer_created(GstPromise *promise, gpointer data)
{
    Session *s = data;
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    if (reply)
        gst_structure_get(reply, "offer", gst_webrtc_session_description_get_type(), &offer, NULL);
    gst_promise_unref(promise);

    if (!offer) {
        post(s, "error", g_strdup("Gagal membuat offer WebRTC"), 0);
        return;
    }
    g_signal_emit_by_name(s->webrtc, "set-local-description", offer, NULL);
    post(s, "offer", gst_sdp_message_as_text(offer->sdp), 0);
    gst_webrtc_session_description_free(offer);
}

static gboolean create_offer(gpointer data)
{
    Session *s = data;
    if (session_alive(s) && !s->app->offer_sent) {
        s->app->offer_sent = TRUE;
        GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, session_ref(s),
                                                               session_unref);
        g_signal_emit_by_name(s->webrtc, "create-offer", NULL, promise);
    }
    return G_SOURCE_REMOVE;
}

/* Offer baru dibuat setelah encoder mengeluarkan SPS/PPS, supaya SDP berisi
 * profile-level-id yang benar (Safari menolak H.264 tanpa itu). */
static GstPadProbeReturn on_pay_event(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    Session *s = data;
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(ev) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;

    GstCaps *caps;
    gst_event_parse_caps(ev, &caps);
    if (!gst_structure_has_field(gst_caps_get_structure(caps, 0), "sprop-parameter-sets"))
        return GST_PAD_PROBE_OK;

    GstCaps *pref = gst_caps_copy(caps);
    GstStructure *st = gst_caps_get_structure(pref, 0);
    gst_structure_remove_fields(st, "ssrc", "timestamp-offset", "seqnum-offset", NULL);
    gst_structure_set(st, "level-asymmetry-allowed", G_TYPE_STRING, "1", NULL);

    GObject *trans = NULL;
    g_signal_emit_by_name(s->webrtc, "get-transceiver", 0, &trans);
    if (trans) {
        g_object_set(trans, "codec-preferences", pref, NULL);
        g_object_unref(trans);
    }
    gst_caps_unref(pref);

    g_timeout_add_full(G_PRIORITY_DEFAULT, 30, create_offer, session_ref(s), session_unref);
    return GST_PAD_PROBE_REMOVE;
}

static void on_ice_candidate(GstElement *webrtc, guint mline, gchar *candidate, gpointer data)
{
    post(data, "ice", g_strdup(candidate), mline);
}

static void on_ice_state(GObject *webrtc, GParamSpec *pspec, gpointer data)
{
    guint state = 0;
    g_object_get(webrtc, "ice-connection-state", &state, NULL);
    post(data, "ice-state", NULL, state);
}

#ifdef G_OS_WIN32
/* Paling banyak tiga kali dalam setengah menit; lebih dari itu berarti
 * kesalahannya menetap dan iPad diberi tahu. */
static gint64 retry_times[3];

static gboolean share_retry_allowed(void)
{
    gint64 now = g_get_monotonic_time();
    guint recent = 0;
    for (guint i = 0; i < G_N_ELEMENTS(retry_times); i++)
        if (retry_times[i] && now - retry_times[i] < 30 * G_USEC_PER_SEC)
            recent++;
    if (recent >= G_N_ELEMENTS(retry_times))
        return FALSE;
    memmove(retry_times + 1, retry_times, sizeof retry_times - sizeof retry_times[0]);
    retry_times[0] = now;
    return TRUE;
}

static gboolean share_restart_later(gpointer data)
{
    App *app = data;
    if (!app->client || app->pipeline)
        return G_SOURCE_REMOVE;     /* iPad sudah pergi, atau aliran sudah jalan lagi */
    GError *err = NULL;
    if (!share_start(app, &err)) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "error");
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, err->message);
        json_builder_end_object(b);
        server_send(app, b);
        g_object_unref(b);
        tsunagu_event("error", "message", err->message, NULL);
        g_error_free(err);
    }
    return G_SOURCE_REMOVE;
}
#endif

static gboolean on_bus(GstBus *bus, GstMessage *m, gpointer data)
{
    Session *s = data;
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
        GError *err = NULL;
        gst_message_parse_warning(m, &err, NULL);
        g_printerr("GStreamer (peringatan): %s\n", err->message);
        g_error_free(err);
    } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR && session_alive(s)) {
        GError *err = NULL;
        char *debug = NULL;
        gst_message_parse_error(m, &err, &debug);
        g_printerr("GStreamer: %s\n%s\n", err->message, debug ? debug : "");
        char *text = g_strdup_printf("Streaming berhenti: %s", err->message);
        g_error_free(err);
        g_free(debug);

        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "error");
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, text);
        json_builder_end_object(b);
#ifndef G_OS_WIN32
        server_send(s->app, b);
        g_object_unref(b);
#endif
#ifdef G_OS_WIN32
        /* Di Windows tangkapan layar bisa putus karena hal yang wajar: tata
         * letak monitor berubah, layar dikunci, atau pindah ke layar login.
         * Alirannya dinyalakan lagi sendiri beberapa kali; halaman iPad
         * menerima offer baru tanpa perlu disentuh. */
        App *app = s->app;
        share_stop(app);
        if (share_retry_allowed()) {
            tsunagu_event("warning", "message", text, NULL);
            g_timeout_add_seconds(1, share_restart_later, app);
        } else {
            server_send(app, b);
            tsunagu_event("error", "message", text, NULL);
        }
        g_object_unref(b);
        g_free(text);
        return G_SOURCE_REMOVE;
#else
        tsunagu_event("error", "message", text, NULL);
        g_free(text);

        share_stop(s->app);
        return G_SOURCE_REMOVE;
#endif
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- API ---------- */

#ifndef G_OS_WIN32
static gboolean place_virtual_monitor(gpointer data)
{
    Session *s = data;
    if (!session_alive(s) || !s->app->virtmon)
        return G_SOURCE_REMOVE;
    GError *err = NULL;
    if (!virtmon_place(s->app->virtmon, &err)) {
        tsunagu_event("warning", "message",
                      err ? err->message : "Monitor virtual gagal ditempatkan", NULL);
        g_clear_error(&err);
    } else {
        tsunagu_event("second", "state", "placed", NULL);
    }
    return G_SOURCE_REMOVE;
}

#endif

/* Pengukur bawaan untuk mencari sumber patah-patah: laju frame di dua titik
 * dan jarak terlama antar frame, yang justru paling terasa di mata. */
typedef struct {
    const char *label;
    guint frames;
    gint64 window, last;
    double gap_max_ms;
} Meter;

static Meter meter_raw = { "kompositor" };
static Meter meter_src = { "sumber" };
static Meter meter_enc = { "encoder" };

static GstPadProbeReturn meter_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    Meter *m = data;
    gint64 t = g_get_monotonic_time();
    if (m->last)
        m->gap_max_ms = MAX(m->gap_max_ms, (t - m->last) / 1000.0);
    m->last = t;
    m->frames++;
    if (!m->window)
        m->window = t;
    if (t - m->window >= 2 * G_USEC_PER_SEC) {
        double secs = (t - m->window) / (double) G_USEC_PER_SEC;
        g_printerr("pipeline %s: %.1f fps, jeda antar frame terlama %.0f ms\n",
                   m->label, m->frames / secs, m->gap_max_ms);
        m->frames = 0;
        m->gap_max_ms = 0;
        m->window = t;
    }
    return GST_PAD_PROBE_OK;
}

static void meter_attach(GstElement *pipeline, const char *name, Meter *m)
{
    GstElement *el = gst_bin_get_by_name(GST_BIN(pipeline), name);
    if (!el)
        return;
    GstPad *pad = gst_element_get_static_pad(el, "src");
    if (pad) {
        memset(m, 0, sizeof *m);
        m->label = (m == &meter_raw) ? "kompositor" : (m == &meter_src) ? "sumber" : "encoder";
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
                          meter_probe, m, NULL);
        gst_object_unref(pad);
    }
    gst_object_unref(el);
}

#ifndef G_OS_WIN32
/* Layar yang diam tidak perlu di-encode ulang 30 kali per detik. Frame identik
 * dibuang sebelum sampai ke encoder, kecuali beberapa frame pertama dan satu
 * dari tiap IDLE_EVERY: itu menjaga gambar tetap menajam, dan iPad tidak
 * menganggap videonya macet. Kursor ikut tergambar di frame X11, jadi gerakan
 * kursor selalu terhitung sebagai perubahan. */
#define IDLE_SETTLE 3
#define IDLE_EVERY  15

typedef struct {
    guint8 *prev;
    gsize size;
    guint same;
} IdleSkip;

static void idle_skip_free(gpointer data)
{
    IdleSkip *d = data;
    g_free(d->prev);
    g_free(d);
}

static GstPadProbeReturn idle_skip_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    IdleSkip *d = data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map;
    if (!buf || !gst_buffer_map(buf, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;

    if (d->prev && d->size == map.size && memcmp(d->prev, map.data, map.size) == 0) {
        d->same++;
    } else {
        if (d->size != map.size) {
            d->prev = g_realloc(d->prev, map.size);
            d->size = map.size;
        }
        memcpy(d->prev, map.data, map.size);
        d->same = 0;
    }
    gst_buffer_unmap(buf, &map);

    if (d->same <= IDLE_SETTLE || d->same % IDLE_EVERY == 0)
        return GST_PAD_PROBE_OK;
    return GST_PAD_PROBE_DROP;
}

static void idle_skip_attach(GstElement *pipeline)
{
    GstElement *el = gst_bin_get_by_name(GST_BIN(pipeline), "cap");
    if (!el)
        return;
    GstPad *pad = gst_element_get_static_pad(el, "src");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, idle_skip_probe,
                          g_new0(IdleSkip, 1), idle_skip_free);
        gst_object_unref(pad);
    }
    gst_object_unref(el);
}

#endif

#ifndef G_OS_WIN32
/* ---------- layar kedua: gambar + kursor asli dari laptop ---------- */

/* Monitor virtual Mutter hanya digambar ulang kalau isinya berubah, jadi
 * gerakan kursor saja tidak menghasilkan frame baru. Yang tetap dikirim Mutter
 * adalah metadata kursor: posisi dan gambarnya, puluhan kali per detik.
 *
 * Aliran itu dibaca langsung oleh src/pwcap.c. Frame layar terakhir disimpan di
 * sini, kursor sungguhan ditempel di atas salinannya, lalu hasilnya didorong ke
 * pipeline lewat appsrc. Dari sisi iPad, kursornya terlihat seperti di monitor
 * biasa: selalu ada dan ikut bergerak meski layarnya diam. */
#define FEED_GAP_US 12000       /* jarak minimal antar frame yang didorong */

static struct {
    GstElement *src;            /* appsrc */
    Pwcap *cap;
    GMutex lock;
    guint8 *bg;                 /* frame layar terakhir, tanpa kursor */
    gsize bg_size;
    int width, height, stride;
    guint8 *cursor;             /* gambar kursor BGRA, alpha sudah terkali */
    int cursor_w, cursor_h;
    int x, y;                   /* sudut kiri-atas gambar kursor */
    gboolean cursor_valid;
    gint64 last_push;
    guint pending;
    guint keepalive;            /* denyut agar aliran tidak pernah kosong */
    GstBuffer *last;            /* frame terakhir yang sudah jadi, siap diulang */
    int fps;
} feed;

/* Dipanggil dengan feed.lock dipegang. */
static void cursor_blend(guint8 *data, int stride, int w, int h)
{
    for (int cy = 0; cy < feed.cursor_h; cy++) {
        int py = feed.y + cy;
        if (py < 0 || py >= h)
            continue;
        const guint8 *s = feed.cursor + (gsize) cy * feed.cursor_w * 4;
        guint8 *d = data + (gsize) py * stride;
        for (int cx = 0; cx < feed.cursor_w; cx++) {
            int px = feed.x + cx;
            if (px < 0 || px >= w)
                continue;
            const guint8 *sp = s + cx * 4;
            guint a = sp[3];
            if (!a)
                continue;
            guint8 *dp = d + (gsize) px * 4;
            guint inv = 255 - a;
            for (int c = 0; c < 3; c++)
                dp[c] = (guint8) MIN(255u, sp[c] + (dp[c] * inv + 127) / 255);
        }
    }
}

/* Memberi cap waktu dari jam pipeline lalu mendorongnya ke appsrc. Kalau cap
 * waktunya diserahkan ke appsrc, frame pertama bisa bercap waktu dari sebelum
 * pipeline punya base time, dan pencampur akan menunggu selamanya. */
static void feed_send(GstElement *src, GstBuffer *out, int fps)
{
    GstClock *clock = gst_element_get_clock(src);
    if (clock) {
        GstClockTime base = gst_element_get_base_time(src);
        GstClockTime now = gst_clock_get_time(clock);
        GST_BUFFER_PTS(out) = now > base ? now - base : 0;
        GST_BUFFER_DTS(out) = GST_BUFFER_PTS(out);
        GST_BUFFER_DURATION(out) = GST_SECOND / MAX(fps, 1);
        gst_object_unref(clock);
    }

    GstFlowReturn ret = GST_FLOW_OK;
    g_signal_emit_by_name(src, "push-buffer", out, &ret);
    gst_buffer_unref(out);

    if (g_getenv("TSUNAGUPAD_DEBUG")) {
        static int n;
        static gint64 t0;
        gint64 now = g_get_monotonic_time();
        n++;
        if (!t0)
            t0 = now;
        if (now - t0 > 2000000) {
            g_printerr("pipeline layar kedua: %.1f fps dikirim ke encoder (%s)\n",
                       n / ((now - t0) / 1e6), gst_flow_get_name(ret));
            n = 0;
            t0 = now;
        }
    }
}

static void feed_push(void)
{
    g_mutex_lock(&feed.lock);
    if (!feed.src || !feed.bg) {
        g_mutex_unlock(&feed.lock);
        return;
    }
    GstBuffer *out = gst_buffer_new_allocate(NULL, feed.bg_size, NULL);
    GstMapInfo map;
    if (!gst_buffer_map(out, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(out);
        g_mutex_unlock(&feed.lock);
        return;
    }
    memcpy(map.data, feed.bg, feed.bg_size);
    if (feed.cursor_valid && feed.cursor)
        cursor_blend(map.data, feed.stride, feed.width, feed.height);
    gst_buffer_unmap(out, &map);

    GstElement *src = gst_object_ref(feed.src);
    int fps = feed.fps;
    feed.last_push = g_get_monotonic_time();
    gst_buffer_replace(&feed.last, out);
    g_mutex_unlock(&feed.lock);

    feed_send(src, out, fps);
    gst_object_unref(src);
}

static gboolean feed_push_later(gpointer data)
{
    g_mutex_lock(&feed.lock);
    feed.pending = 0;
    g_mutex_unlock(&feed.lock);
    feed_push();
    return G_SOURCE_REMOVE;
}

/* Dorong sekarang bila jedanya sudah cukup; kalau belum, satu dorongan susulan
 * dijadwalkan supaya posisi kursor terakhir tetap sampai tanpa membanjiri encoder. */
static void feed_request(void)
{
    g_mutex_lock(&feed.lock);
    gint64 wait = FEED_GAP_US - (g_get_monotonic_time() - feed.last_push);
    gboolean now = wait <= 0;
    if (!now && !feed.pending)
        feed.pending = g_timeout_add((guint) (wait / 1000) + 1, feed_push_later, NULL);
    g_mutex_unlock(&feed.lock);
    if (now)
        feed_push();
}

/* Layar bisa diam sama sekali dan kursor tidak bergerak, tetapi alirannya tidak
 * boleh ikut melambat: laju yang turun membuat gerakan berikutnya terasa patah,
 * dan encoder berhenti memperhalus gambar diam sehingga sisa keyframe terakhir
 * tertinggal buram. Frame terakhir yang sudah jadi dikirim ulang apa adanya,
 * tanpa disalin lagi, jadi menjaga laju penuh nyaris tidak memakan tenaga. */
static gboolean feed_keepalive(gpointer data)
{
    g_mutex_lock(&feed.lock);
    gint64 gap = G_USEC_PER_SEC / MAX(feed.fps, 1);
    GstBuffer *again = NULL;
    GstElement *src = NULL;
    int fps = feed.fps;
    if (feed.src && feed.last && g_get_monotonic_time() - feed.last_push > gap - 2000) {
        again = gst_buffer_copy(feed.last);
        src = gst_object_ref(feed.src);
        feed.last_push = g_get_monotonic_time();
    }
    g_mutex_unlock(&feed.lock);

    if (again) {
        feed_send(src, again, fps);
        gst_object_unref(src);
    }
    return G_SOURCE_CONTINUE;
}

/* Datang dari thread PipeWire. */
static void on_capture(const PwcapUpdate *u, gpointer user)
{
    gboolean changed = FALSE;
    g_mutex_lock(&feed.lock);
    if (u->pixels && u->width > 0 && u->height > 0 && u->stride > 0) {
        /* Disalin rapat selebar gambar: stride dari PipeWire bisa berbeda
         * dengan yang diharapkan pipeline. */
        int stride = u->width * 4;
        gsize need = (gsize) stride * u->height;
        if (need != feed.bg_size) {
            g_free(feed.bg);
            feed.bg = g_malloc(need);
            feed.bg_size = need;
        }
        if (u->stride == stride) {
            memcpy(feed.bg, u->pixels, need);
        } else {
            for (int y = 0; y < u->height; y++)
                memcpy(feed.bg + (gsize) y * stride, u->pixels + (gsize) y * u->stride, stride);
        }
        feed.width = u->width;
        feed.height = u->height;
        feed.stride = stride;
        changed = TRUE;
    }
    if (u->cursor && u->cursor_w > 0 && u->cursor_h > 0) {
        gsize need = (gsize) u->cursor_w * u->cursor_h * 4;
        g_free(feed.cursor);
        feed.cursor = g_memdup2(u->cursor, need);
        feed.cursor_w = u->cursor_w;
        feed.cursor_h = u->cursor_h;
        changed = TRUE;
    }
    if (u->cursor_meta) {
        /* Kursor hilang dari gambar begitu penunjuk pindah ke layar lain. */
        if (feed.cursor_valid != u->cursor_valid)
            changed = TRUE;
        feed.cursor_valid = u->cursor_valid;
        if (u->cursor_valid && (feed.x != u->cursor_x || feed.y != u->cursor_y)) {
            feed.x = u->cursor_x;
            feed.y = u->cursor_y;
            changed = TRUE;
        }
    }
    g_mutex_unlock(&feed.lock);

    /* Mutter juga mengabarkan kursor yang bergerak di layar lain. Tanpa
     * penyaringan ini, frame yang isinya sama persis disalin ulang enam puluh
     * kali sedetik hanya karena penunjuk digerakkan di layar laptop. */
    if (changed)
        feed_request();
}

static void feed_clear(void)
{
    g_mutex_lock(&feed.lock);
    if (feed.pending) {
        g_source_remove(feed.pending);
        feed.pending = 0;
    }
    if (feed.keepalive) {
        g_source_remove(feed.keepalive);
        feed.keepalive = 0;
    }
    Pwcap *cap = feed.cap;
    feed.cap = NULL;
    gst_clear_object(&feed.src);
    g_mutex_unlock(&feed.lock);

    pwcap_close(cap);       /* menunggu thread PipeWire berhenti */

    g_mutex_lock(&feed.lock);
    g_clear_pointer(&feed.bg, g_free);
    g_clear_pointer(&feed.cursor, g_free);
    gst_buffer_replace(&feed.last, NULL);
    feed.bg_size = 0;
    feed.cursor_valid = FALSE;
    g_mutex_unlock(&feed.lock);
}

#endif /* !G_OS_WIN32 */

gboolean share_start(App *app, GError **error)
{
    share_stop(app);
    app->session++;
    memset(&adapt, 0, sizeof adapt);
    app->kbps_now = encoder_can_retune(app->enc_name) ? MIN(app->kbps, KBPS_START) : app->kbps;

    char *src;
#ifdef G_OS_WIN32
    if (app->test_source) {
        src = g_strdup_printf("videotestsrc is-live=true pattern=ball ! "
                              "video/x-raw,format=BGRx,width=%d,height=%d,framerate=%d/1",
                              app->rw, app->rh, app->fps);
    } else {
        /* Monitor dicari lagi lewat namanya setiap kali aliran dimulai:
         * handle-nya bisa berganti setelah tata letak layar diubah, dan layar
         * kedua bisa sudah digeser pengguna di Pengaturan Tampilan.
         *
         * DXGI Desktop Duplication menyalin layar di GPU dan mengulang frame
         * terakhir bila layar diam, jadi aliran tetap berdetak pada laju
         * penuh. Kursor ikut digambar oleh elemennya sendiri. */
        WinMonitor mon;
        if (!win_monitor_by_device(app->monitor, &mon)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Monitor %s tidak ditemukan", app->monitor ? app->monitor : "?");
            return FALSE;
        }
        char *crop;
        if (app->crop) {
            crop = g_strdup_printf(" crop-x=%d crop-y=%d crop-width=%d crop-height=%d",
                                   app->crop_x, app->crop_y, app->rw, app->rh);
            app->rx = mon.x + app->crop_x;
            app->ry = mon.y + app->crop_y;
        } else {
            crop = g_strdup("");
            app->rx = mon.x;
            app->ry = mon.y;
            app->rw = mon.width;
            app->rh = mon.height;
        }
        /* Tekstur GPU bila encoder-nya bisa membacanya; memori biasa hanya
         * untuk encoder cadangan tanpa Direct3D. */
        src = g_strdup_printf("d3d11screencapturesrc name=capture monitor-handle=%" G_GUINT64_FORMAT
                              " show-cursor=%s%s ! capsfilter caps=\"video/x-raw(memory:D3D11Memory),"
                              "framerate=%d/1; video/x-raw,framerate=%d/1\"",
                              mon.handle, app->hide_cursor ? "false" : "true", crop, app->fps, app->fps);
        g_free(crop);
    }
#else
    if (app->virtmon) {
        /* Ukuran monitor virtual mengikuti caps yang diminta di sini. Layar
         * kedua sering diam, dan aliran yang berhenti membuat dekoder di iPad
         * menyusun ulang setiap kali gambar berubah lagi. vacompositor berdetak
         * pada laju tetap dan mengulang frame terakhir, seluruhnya di GPU. */
        if (g_str_has_prefix(app->enc_name, "intel"))
            src = g_strdup_printf(
                "appsrc name=feed is-live=true format=time block=false "
                "max-bytes=0 max-time=0 max-buffers=2 leaky-type=downstream ! "
                "identity name=raw silent=true ! "
                "queue max-size-buffers=1 leaky=downstream ! mix.sink_0 "
                "vacompositor name=mix force-live=true latency=0 ! "
                "video/x-raw(memory:VAMemory),format=NV12,width=%d,height=%d,framerate=%d/1",
                app->rw, app->rh, app->fps);
        else
            src = g_strdup_printf(
                "appsrc name=feed is-live=true format=time block=false "
                "max-bytes=0 max-time=0 max-buffers=2 leaky-type=downstream ! "
                "identity name=raw silent=true ! "
                "videorate max-rate=%d ! video/x-raw(ANY)",
                app->fps);
    } else if (app->test_source)
        src = g_strdup_printf("videotestsrc is-live=true pattern=ball ! "
                              "video/x-raw,format=BGRx,width=%d,height=%d,framerate=%d/1",
                              app->rw, app->rh, app->fps);
    else if (app->portal) {
        /* Mutter hanya mengirim frame saat layar berubah. keepalive-time mengulang frame
         * terakhir supaya iPad yang baru tersambung tetap mendapat keyframe.
         * max-framerate dinegosiasikan ke mutter: layar 165 Hz tidak lagi menyalin 165
         * frame per detik ke memori biasa (itu yang membebani gnome-shell).
         * Encoder VA bisa membaca DMA-BUF langsung dari GPU tanpa salinan lewat CPU.
         * Caps dengan dua pilihan membuat GStreamer memilih DMA-BUF bila rantai di
         * belakangnya menerima, dan jatuh ke memori biasa bila tidak.
         * TSUNAGUPAD_CAPTURE_CAPS menggantikan filter caps untuk pengujian. */
        const char *override = g_getenv("TSUNAGUPAD_CAPTURE_CAPS");
        gboolean va = gst_element_factory_find("vacompositor") != NULL &&
                      gst_element_factory_find("vapostproc") != NULL;
        char *caps;
        if (override && *override)
            caps = g_strdup(override);
        else if (va)
            caps = g_strdup_printf("video/x-raw(memory:DMABuf),max-framerate=%d/1; "
                                   "video/x-raw,max-framerate=%d/1", app->fps, app->fps);
        else
            caps = g_strdup_printf("video/x-raw,max-framerate=%d/1", app->fps);

        if (va) {
            /* Sama seperti layar kedua: pencampur VA berdetak pada laju tetap
             * dan mengulang frame terakhir, jadi aliran tidak pernah melambat
             * saat layar diam. Tanpa ini gerakan berikutnya terasa tersendat
             * karena dekoder di iPad baru harus mengejar lagi.
             *
             * Penyekalaan ikut dikerjakan di GPU di sini. Untuk encoder NVENC
             * hasilnya diturunkan lagi ke memori biasa, dan itu tetap jauh
             * lebih murah daripada menyekala layar 2560x1600 dengan CPU. */
            const char *tail = g_str_has_prefix(app->enc_name, "intel")
                ? ""
                : " ! vapostproc ! video/x-raw,format=NV12";
            src = g_strdup_printf(
                "pipewiresrc fd=%d path=%u do-timestamp=true keepalive-time=500 ! "
                "capsfilter caps=\"%s\" ! queue max-size-buffers=1 leaky=downstream ! "
                "vapostproc ! video/x-raw(memory:VAMemory),format=NV12,width=%d,height=%d ! "
                "mix.sink_0 vacompositor name=mix force-live=true latency=0 ! "
                "video/x-raw(memory:VAMemory),format=NV12,width=%d,height=%d,framerate=%d/1%s",
                portal_pipewire_fd(app->portal), portal_node(app->portal), caps,
                app->out_w, app->out_h, app->out_w, app->out_h, app->fps, tail);
        } else {
            src = g_strdup_printf(
                "pipewiresrc fd=%d path=%u do-timestamp=true keepalive-time=500 ! "
                "capsfilter caps=\"%s\" ! videorate drop-only=true max-rate=%d ! "
                "video/x-raw(ANY)",
                portal_pipewire_fd(app->portal), portal_node(app->portal), caps, app->fps);
        }
        g_free(caps);
    } else
        src = g_strdup_printf("ximagesrc use-damage=false show-pointer=true "
                              "startx=%d starty=%d endx=%d endy=%d ! video/x-raw,framerate=%d/1",
                              app->rx, app->ry, app->rx + app->rw - 1, app->ry + app->rh - 1, app->fps);
#endif /* G_OS_WIN32 */
    char *enc = encoder_fragment(app->enc_name, app->out_w, app->out_h, app->fps,
                                 app->kbps_now, app->kbps);

    /* queue leaky: kalau encoder sibuk, frame lama dibuang, bukan ditumpuk (anti-lag). */
    char *desc = g_strdup_printf(
        "%s ! identity name=cap silent=true ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! %s ! "
        "video/x-h264,profile=(string){ constrained-baseline, baseline },"
        "stream-format=byte-stream ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay name=pay config-interval=-1 aggregate-mode=zero-latency mtu=1200 ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
        "webrtcbin name=webrtc bundle-policy=max-bundle",
        src, enc);
    g_free(src);
    g_free(enc);

    if (g_getenv("TSUNAGUPAD_DEBUG"))
        g_printerr("pipeline: %s\n", desc);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (err) {
        g_propagate_error(error, err);
        if (pipeline)
            gst_object_unref(pipeline);
        return FALSE;
    }

    app->pipeline = pipeline;
    app->webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    app->pay = gst_bin_get_by_name(GST_BIN(pipeline), "pay");
    app->enc = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
    app->offer_sent = FALSE;
#ifndef G_OS_WIN32
    if (app->virtmon) {
        GstElement *feedsrc = gst_bin_get_by_name(GST_BIN(pipeline), "feed");
        if (feedsrc) {
            /* Caps lewat properti: appsrc harus bisa bernegosiasi sejak awal. */
            GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                                "format", G_TYPE_STRING, "BGRx",
                                                "width", G_TYPE_INT, app->rw,
                                                "height", G_TYPE_INT, app->rh,
                                                "framerate", GST_TYPE_FRACTION, app->fps, 1,
                                                NULL);
            g_object_set(feedsrc, "caps", caps, NULL);
            gst_caps_unref(caps);
        }
        g_mutex_lock(&feed.lock);
        feed.src = feedsrc;         /* kepemilikan pindah ke feed */
        feed.width = app->rw;
        feed.height = app->rh;
        feed.stride = app->rw * 4;
        feed.fps = app->fps;
        g_mutex_unlock(&feed.lock);

    }
#endif
    playout_delay_attach(app->pay);

    Session *s = g_atomic_rc_box_new0(Session);
    s->app = app;
    s->id = app->session;
    s->webrtc = gst_object_ref(app->webrtc);
    current = s;

    GObject *ice = NULL;
    g_object_get(app->webrtc, "ice-agent", &ice, NULL);
    if (ice) {
        g_object_set(ice, "min-rtp-port", RTP_PORT_MIN, "max-rtp-port", RTP_PORT_MAX, NULL);
        g_object_unref(ice);
    }

    /* Kirim saja (iPad tidak mengirim video balik) + NACK untuk menambal paket hilang di WiFi. */
    GObject *trans = NULL;
    g_signal_emit_by_name(app->webrtc, "get-transceiver", 0, &trans);
    if (trans) {
        g_object_set(trans, "direction", ICHI_TRANSCEIVER_SENDONLY, "do-nack", TRUE, NULL);
        g_object_unref(trans);
    }

    g_signal_connect(app->webrtc, "deep-element-added", G_CALLBACK(on_deep_element_added), NULL);
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(app->webrtc));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        tune_rtx(g_value_get_object(&item));
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);

    g_signal_connect_data(app->webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate),
                          session_ref(s), session_unref_closure, 0);
    g_signal_connect_data(app->webrtc, "notify::ice-connection-state", G_CALLBACK(on_ice_state),
                          session_ref(s), session_unref_closure, 0);

    GstPad *pad = gst_element_get_static_pad(app->pay, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_pay_event, session_ref(s),
                      session_unref);
    gst_object_unref(pad);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus, session_ref(s), session_unref);
    gst_object_unref(bus);

    /* Cadangan kalau caps berisi SPS/PPS tidak kunjung datang. */
    g_timeout_add_full(G_PRIORITY_DEFAULT, 2500, create_offer, session_ref(s), session_unref);

    if (app->enc && encoder_can_retune(app->enc_name))
        app->stats_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, request_stats,
                                              session_ref(s), session_unref);

    if (debug_enabled()) {
        meter_attach(pipeline, "raw", &meter_raw);
        meter_attach(pipeline, "cap", &meter_src);
        meter_attach(pipeline, "pay", &meter_enc);
    }

#ifndef G_OS_WIN32
    /* Hanya jalur X11: frame PipeWire bisa berupa DMA-BUF yang mahal dibaca CPU.
     * Di Windows frame berada di memori GPU, jadi pembandingan ini dilewati. */
    if (!app->virtmon && !app->portal)
        idle_skip_attach(pipeline);
#endif

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        share_stop(app);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Pipeline tidak bisa dijalankan");
        return FALSE;
    }
#ifndef G_OS_WIN32
    if (app->virtmon) {
        /* Aliran baru dibaca setelah pipeline berjalan: appsrc memberi cap waktu
         * dari jam pipeline, dan sebelum PLAYING cap waktunya belum berlaku. */
        GError *cerr = NULL;
        Pwcap *cap = pwcap_open(virtmon_node(app->virtmon), app->rw, app->rh,
                                on_capture, NULL, &cerr);
        if (!cap) {
            g_propagate_error(error, cerr);
            share_stop(app);
            return FALSE;
        }
        g_mutex_lock(&feed.lock);
        feed.cap = cap;
        feed.keepalive = g_timeout_add(MAX(1000 / MAX(app->fps, 1), 4), feed_keepalive, NULL);
        g_mutex_unlock(&feed.lock);

        /* Monitor virtual baru dibuat Mutter setelah aliran dinegosiasikan,
         * jadi penempatannya dilakukan sesaat setelah pipeline berjalan. */
        g_timeout_add_full(G_PRIORITY_DEFAULT, 400, place_virtual_monitor, session_ref(s),
                           session_unref);
    }
#endif
    tsunagu_event("stream", "state", "starting", NULL);
    return TRUE;
}

void share_stop(App *app)
{
    if (!app->pipeline)
        return;
    app->session++;

    if (app->stats_timer) {
        g_source_remove(app->stats_timer);
        app->stats_timer = 0;
    }

    GstBus *bus = gst_element_get_bus(app->pipeline);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);

    if (current) {
        g_signal_handlers_disconnect_by_data(app->webrtc, current);
        session_unref(current);
        current = NULL;
    }
#ifndef G_OS_WIN32
    feed_clear();
#endif
    gst_element_set_state(app->pipeline, GST_STATE_NULL);
    gst_clear_object(&app->enc);
    gst_clear_object(&app->pay);
    gst_clear_object(&app->webrtc);
    gst_clear_object(&app->pipeline);
    tsunagu_event("stream", "state", "stopped", NULL);
}

void share_on_answer(App *app, const char *sdp)
{
    if (!app->webrtc)
        return;
    GstSDPMessage *msg = NULL;
    if (gst_sdp_message_new_from_text(sdp, &msg) != GST_SDP_OK) {
        tsunagu_event("error", "message", "Jawaban SDP dari iPad tidak valid", NULL);
        return;
    }
    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, msg);
    g_signal_emit_by_name(app->webrtc, "set-remote-description", answer, NULL);
    gst_webrtc_session_description_free(answer);
}

void share_on_ice(App *app, int mline, const char *candidate)
{
    if (app->webrtc)
        g_signal_emit_by_name(app->webrtc, "add-ice-candidate", (guint) mline, candidate);
}

/* Minta keyframe baru (saat baru tersambung atau iPad melihat gambar rusak). */
void share_keyframe(App *app)
{
    if (!app->pay)
        return;
    GstStructure *st = gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL);
    GstPad *pad = gst_element_get_static_pad(app->pay, "src");
    gst_pad_send_event(pad, gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, st));
    gst_object_unref(pad);
}
