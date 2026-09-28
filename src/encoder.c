/* Memilih encoder H.264 tercepat yang benar-benar jalan di mesin ini.
 * SPDX-License-Identifier: MIT
 */
#include "tsunagupad.h"

typedef struct {
    const char *name;
    const char *fmt;        /* argumen berurutan: lebar, tinggi, kbps, gop, vbv(kbit) */
    gboolean retune;        /* bitrate bisa diubah saat stream berjalan */
} Encoder;

/* Urutan = prioritas untuk mode "auto". Semua tanpa B-frame agar tidak ada
 * penundaan urutan frame, dan profil constrained-baseline supaya Safari mau.
 * Elemen encoder bernama "enc" supaya bitrate-nya bisa diatur ulang.
 *
 * NVENC hanya menerima NV12/Y444 dari memori sistem. Konversi dilakukan
 * videoconvertscale di CPU (~5 ms/frame pada 1920x1200) karena elemen
 * cudaconvertscale butuh NVRTC dari CUDA Toolkit yang umumnya tidak terpasang.
 * Encoder Intel "intel-lp" (VDEnc) didahulukan dari vaapi lama karena bitrate-nya
 * bisa diubah saat berjalan, sehingga bitrate adaptif tetap bekerja. */
static const Encoder encoders[] = {
    { "nvidia",
      "videoconvertscale n-threads=4 ! video/x-raw,format=NV12,width=%d,height=%d ! "
      "nvautogpuh264enc name=enc preset=p1 tune=ultra-low-latency rate-control=cbr "
      "bitrate=%d gop-size=%d vbv-buffer-size=%d b-frames=0 zero-reorder-delay=true",
      TRUE },
    { "intel-lp",
      "vapostproc ! video/x-raw(memory:VAMemory),format=NV12,width=%d,height=%d ! "
      "vah264lpenc name=enc rate-control=cbr bitrate=%d key-int-max=%d cpb-size=%d "
      "b-frames=0 ref-frames=1 target-usage=7",
      TRUE },
    { "intel",
      "vaapipostproc ! video/x-raw(memory:VASurface),format=NV12,width=%d,height=%d ! "
      "vaapih264enc name=enc rate-control=cbr bitrate=%d keyframe-period=%d max-bframes=0 cpb-length=100",
      FALSE },
    { "cpu",
      "videoconvertscale n-threads=4 ! video/x-raw,format=I420,width=%d,height=%d ! "
      "x264enc name=enc tune=zerolatency speed-preset=ultrafast bitrate=%d key-int-max=%d bframes=0 "
      "vbv-buf-capacity=100 threads=4 sliced-threads=true",
      TRUE },
};

static const Encoder *find(const char *name)
{
    for (guint i = 0; i < G_N_ELEMENTS(encoders); i++)
        if (g_strcmp0(encoders[i].name, name) == 0)
            return &encoders[i];
    return NULL;
}

gboolean encoder_can_retune(const char *name)
{
    const Encoder *e = find(name);
    return e && e->retune;
}

char *encoder_fragment(const char *name, int w, int h, int fps, int kbps, int max_kbps)
{
    const Encoder *e = find(name);
    if (!e)
        return NULL;
    /* Keyframe tiap detik mempercepat pemulihan saat paket hilang. VBV dihitung
     * dari bitrate maksimum supaya bitrate adaptif tetap bisa naik ke batasnya. */
    int gop = fps;
    int vbv = MAX(MAX(kbps, max_kbps) / MAX(fps, 1), 50);
    return g_strdup_printf(e->fmt, w, h, kbps, gop, vbv);
}

/* Jalankan 5 frame uji; driver bisa saja menolak walau plugin-nya ada. */
static gboolean probe(const Encoder *e)
{
    char *frag = encoder_fragment(e->name, 640, 360, 30, 2000, 2000);
    char *desc = g_strdup_printf(
        "videotestsrc num-buffers=5 ! video/x-raw,format=BGRx,width=640,height=360,framerate=30/1 ! "
        "%s ! video/x-h264,profile=constrained-baseline ! h264parse ! fakesink", frag);
    g_free(frag);

    GError *err = NULL;
    GstElement *p = gst_parse_launch(desc, &err);
    g_free(desc);
    if (err) {
        if (g_getenv("TSUNAGUPAD_DEBUG"))
            g_printerr("probe %s: %s\n", e->name, err->message);
        g_clear_error(&err);
        if (p)
            gst_object_unref(p);
        return FALSE;
    }

    gboolean ok = FALSE;
    if (gst_element_set_state(p, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        GstBus *bus = gst_element_get_bus(p);
        GstMessage *m = gst_bus_timed_pop_filtered(bus, 8 * GST_SECOND,
                                                   GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
        if (m && !ok && g_getenv("TSUNAGUPAD_DEBUG")) {
            GError *perr = NULL;
            gst_message_parse_error(m, &perr, NULL);
            g_printerr("probe %s: %s\n", e->name, perr ? perr->message : "gagal");
            g_clear_error(&perr);
        }
        if (m)
            gst_message_unref(m);
        gst_object_unref(bus);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

const char *encoder_pick(const char *wanted)
{
    if (wanted && g_strcmp0(wanted, "auto") != 0) {
        const Encoder *e = find(wanted);
        return e && probe(e) ? e->name : NULL;
    }
    for (guint i = 0; i < G_N_ELEMENTS(encoders); i++)
        if (probe(&encoders[i]))
            return encoders[i].name;
    return NULL;
}
