/* Tangkap layar -> encode H.264 -> kirim ke Safari iPad lewat WebRTC.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "tsunagupad.h"
#include "webrtc-lite.h"

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
    g_object_set(app->enc, "bitrate", (guint) kbps, NULL);
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
    /* Permintaan keyframe (PLI) di awal sambungan itu wajar; setelahnya tanda gambar rusak. */
    gboolean picture_lost = adapt.sent && r->pli > adapt.pli &&
                            now - adapt.connected > 3 * G_USEC_PER_SEC;
    adapt.sent = r->sent;
    adapt.lost = r->lost;
    adapt.pli = r->pli;

    if (loss > 0.03 || picture_lost) {
        adapt.calm_since = now;
        set_bitrate(app, (int) (app->kbps_now * (loss > 0.10 ? 0.6 : 0.8)));
    } else if (now - adapt.calm_since > 3 * G_USEC_PER_SEC) {
        set_bitrate(app, (int) (app->kbps_now * 1.08) + 150);
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
        server_send(s->app, b);
        g_object_unref(b);
        tsunagu_event("error", "message", text, NULL);
        g_free(text);

        share_stop(s->app);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---------- API ---------- */

gboolean share_start(App *app, GError **error)
{
    share_stop(app);
    app->session++;
    memset(&adapt, 0, sizeof adapt);
    app->kbps_now = encoder_can_retune(app->enc_name) ? MIN(app->kbps, KBPS_START) : app->kbps;

    char *src;
    if (app->test_source)
        src = g_strdup_printf("videotestsrc is-live=true pattern=ball ! "
                              "video/x-raw,format=BGRx,width=%d,height=%d,framerate=%d/1",
                              app->rw, app->rh, app->fps);
    else
        src = g_strdup_printf("ximagesrc use-damage=false show-pointer=true "
                              "startx=%d starty=%d endx=%d endy=%d ! video/x-raw,framerate=%d/1",
                              app->rx, app->ry, app->rx + app->rw - 1, app->ry + app->rh - 1, app->fps);
    char *enc = encoder_fragment(app->enc_name, app->out_w, app->out_h, app->fps,
                                 app->kbps_now, app->kbps);

    /* queue leaky: kalau encoder sibuk, frame lama dibuang, bukan ditumpuk (anti-lag). */
    char *desc = g_strdup_printf(
        "%s ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! %s ! "
        "video/x-h264,profile=constrained-baseline,stream-format=byte-stream ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay name=pay config-interval=-1 aggregate-mode=zero-latency mtu=1200 ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
        "webrtcbin name=webrtc bundle-policy=max-bundle",
        src, enc);
    g_free(src);
    g_free(enc);

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

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        share_stop(app);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Pipeline tidak bisa dijalankan");
        return FALSE;
    }
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
