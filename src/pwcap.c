/* Pembaca aliran PipeWire untuk layar kedua: gambar sekaligus metadata kursor.
 *
 * Mutter hanya menggambar ulang monitor virtual kalau ada isi yang berubah.
 * Gerakan kursor saja tidak membuatnya menggambar ulang, tetapi Mutter tetap
 * mengirim buffer berisi metadata kursor (posisi, titik tumpu, dan gambarnya)
 * puluhan kali per detik. Elemen pipewiresrc membuang buffer semacam itu
 * ("Chunk size is 0, skipping"), jadi kursor tidak pernah sampai ke iPad.
 *
 * Berkas ini menyambung sendiri ke node PipeWire milik monitor virtual supaya
 * kedua-duanya terbaca: frame gambar saat ada, dan pembaruan kursor saat layar
 * diam. Kursor sungguhan dari laptop itulah yang nanti ditempel ke frame.
 *
 * Hanya dipakai di Wayland, bersama monitor virtual Mutter.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include "tsunagupad.h"

/* Ruang metadata kursor: struktur + gambar kursor sebesar w x h piksel. */
#define CURSOR_META_SIZE(w, h) \
    (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)

struct _Pwcap {
    struct pw_thread_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook listener;
    struct spa_video_info_raw format;
    int want_w, want_h;
    guint8 *cur;            /* gambar kursor yang sudah diseragamkan jadi BGRA */
    gsize cur_cap;
    int hot_x, hot_y;       /* titik tumpu gambar kursor yang berlaku sekarang */
    PwcapCb cb;
    gpointer user;
};

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    Pwcap *p = data;
    if (!param || id != SPA_PARAM_Format)
        return;

    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
        media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_format_video_raw_parse(param, &p->format);

    /* Minta metadata kursor. Tanpa permintaan ini Mutter tidak mengirimkannya.
     * Jenis buffer ikut disebut: tanpa itu daftar parameter dianggap tidak
     * lengkap dan metadata kursor tidak pernah dialokasikan. */
    uint8_t buf[2048];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[4];
    params[0] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    params[1] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
    params[3] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int((int) CURSOR_META_SIZE(64, 64), (int) CURSOR_META_SIZE(1, 1),
                                 (int) CURSOR_META_SIZE(1024, 1024)));
    pw_stream_update_params(p->stream, params, 4);
    if (g_getenv("TSUNAGUPAD_DEBUG"))
        g_printerr("pwcap: aliran %ux%u, metadata kursor diminta\n", p->format.size.width,
                   p->format.size.height);
}

/* Gambar kursor dari Mutter bisa datang sebagai ARGB atau BGRA, dengan alpha
 * sudah terkali atau belum. Di sini semuanya diseragamkan jadi BGRA dengan
 * alpha terkali supaya penempelannya nanti sesederhana mungkin. */
static void convert_bitmap(Pwcap *p, const struct spa_meta_bitmap *bm, PwcapUpdate *u)
{
    int w = bm->size.width, h = bm->size.height;
    gsize need = (gsize) w * h * 4;
    if (need > p->cur_cap) {
        p->cur = g_realloc(p->cur, need);
        p->cur_cap = need;
    }
    const guint8 *src = SPA_PTROFF(bm, bm->offset, const guint8);
    int stride = bm->stride > 0 ? bm->stride : w * 4;

    int bi = 0, gi = 1, ri = 2, ai = 3;
    switch (bm->format) {
    case SPA_VIDEO_FORMAT_ARGB:
    case SPA_VIDEO_FORMAT_xRGB:
        ai = 0; ri = 1; gi = 2; bi = 3;
        break;
    case SPA_VIDEO_FORMAT_RGBA:
    case SPA_VIDEO_FORMAT_RGBx:
        ri = 0; gi = 1; bi = 2; ai = 3;
        break;
    default:
        break;
    }

    gboolean straight = FALSE;
    for (int y = 0; y < h && !straight; y++) {
        const guint8 *row = src + (gsize) y * stride;
        for (int x = 0; x < w; x++) {
            const guint8 *q = row + x * 4;
            guint8 a = q[ai];
            if (q[bi] > a || q[gi] > a || q[ri] > a) {
                straight = TRUE;
                break;
            }
        }
    }

    for (int y = 0; y < h; y++) {
        const guint8 *row = src + (gsize) y * stride;
        guint8 *out = p->cur + (gsize) y * w * 4;
        for (int x = 0; x < w; x++) {
            const guint8 *q = row + x * 4;
            guint8 a = q[ai];
            guint8 b = q[bi], g = q[gi], r = q[ri];
            if (straight) {
                b = (guint8) ((b * a + 127) / 255);
                g = (guint8) ((g * a + 127) / 255);
                r = (guint8) ((r * a + 127) / 255);
            }
            out[x * 4 + 0] = b;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = r;
            out[x * 4 + 3] = a;
        }
    }

    u->cursor = p->cur;
    u->cursor_w = w;
    u->cursor_h = h;
    u->cursor_stride = w * 4;
}

static void on_process(void *data)
{
    Pwcap *p = data;
    struct pw_buffer *pwb = pw_stream_dequeue_buffer(p->stream);
    if (!pwb)
        return;

    struct spa_buffer *buf = pwb->buffer;
    PwcapUpdate u;
    memset(&u, 0, sizeof u);

    if (buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk &&
        buf->datas[0].chunk->size > 0) {
        u.pixels = (const guint8 *) buf->datas[0].data + buf->datas[0].chunk->offset;
        u.width = p->format.size.width;
        u.height = p->format.size.height;
        u.stride = buf->datas[0].chunk->stride;
        if (u.stride <= 0)
            u.stride = u.width * 4;
    }

    struct spa_meta_cursor *mc = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof *mc);
    u.cursor_meta = mc != NULL;
    if (mc && spa_meta_cursor_is_valid(mc)) {
        u.cursor_valid = TRUE;
        /* Titik tumpu hanya ikut dikirim bersama gambar kursornya. Pembaruan
         * posisi biasa datang tanpa itu, jadi nilai terakhir yang dipakai;
         * kalau tidak, gambar kursor melenceng sejauh titik tumpunya. */
        if (mc->bitmap_offset) {
            struct spa_meta_bitmap *bm = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
            if (bm->size.width > 0 && bm->size.height > 0) {
                convert_bitmap(p, bm, &u);
                p->hot_x = mc->hotspot.x;
                p->hot_y = mc->hotspot.y;
            }
        }
        u.cursor_x = mc->position.x - p->hot_x;
        u.cursor_y = mc->position.y - p->hot_y;
    }

    if (g_getenv("TSUNAGUPAD_DEBUG")) {
        static int frames, cursors;
        static gint64 t0;
        if (u.pixels)
            frames++;
        if (u.cursor_valid)
            cursors++;
        gint64 now = g_get_monotonic_time();
        if (!t0)
            t0 = now;
        if (now - t0 > 2000000) {
            g_printerr("pwcap: %d frame layar, %d kabar kursor dalam %.1f s "
                       "(kursor di %d,%d titik tumpu %d,%d)\n", frames, cursors,
                       (now - t0) / 1e6, u.cursor_x, u.cursor_y, p->hot_x, p->hot_y);
            frames = cursors = 0;
            t0 = now;
        }
    }
    if (u.pixels || u.cursor_meta)
        p->cb(&u, p->user);
    pw_stream_queue_buffer(p->stream, pwb);
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

Pwcap *pwcap_open(guint32 node, int width, int height, PwcapCb cb, gpointer user, GError **error)
{
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        pw_init(NULL, NULL);
        g_once_init_leave(&once, 1);
    }

    Pwcap *p = g_new0(Pwcap, 1);
    p->cb = cb;
    p->user = user;
    p->want_w = width;
    p->want_h = height;

    p->loop = pw_thread_loop_new("tsunagupad-pw", NULL);
    if (!p->loop) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "PipeWire tidak bisa dijalankan");
        pwcap_close(p);
        return NULL;
    }

    pw_thread_loop_lock(p->loop);
    p->ctx = pw_context_new(pw_thread_loop_get_loop(p->loop), NULL, 0);
    if (p->ctx)
        p->core = pw_context_connect(p->ctx, NULL, 0);
    if (!p->core) {
        pw_thread_loop_unlock(p->loop);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Tidak bisa menyambung ke PipeWire");
        pwcap_close(p);
        return NULL;
    }

    p->stream = pw_stream_new(p->core, "tsunagupad-layar-kedua",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                PW_KEY_MEDIA_CATEGORY, "Capture",
                                                PW_KEY_MEDIA_ROLE, "Screen", NULL));
    pw_stream_add_listener(p->stream, &p->listener, &stream_events, p);

    /* Ukuran diminta pasti: ukuran monitor virtual mengikuti permintaan ini. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t) width, (uint32_t) height);
    struct spa_fraction fps_max = SPA_FRACTION(1000, 1);
    struct spa_fraction fps_min = SPA_FRACTION(0, 1);
    struct spa_fraction fps_def = SPA_FRACTION(60, 1);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                               SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&fps_def, &fps_min, &fps_max));

    int ret = pw_stream_connect(p->stream, PW_DIRECTION_INPUT, node,
                                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                                params, 1);
    pw_thread_loop_unlock(p->loop);
    if (ret < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Aliran monitor virtual tidak bisa dibuka (%s)", g_strerror(-ret));
        pwcap_close(p);
        return NULL;
    }
    pw_thread_loop_start(p->loop);
    return p;
}

void pwcap_close(Pwcap *p)
{
    if (!p)
        return;
    if (p->loop)
        pw_thread_loop_stop(p->loop);
    if (p->stream) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }
    if (p->core) {
        pw_core_disconnect(p->core);
        p->core = NULL;
    }
    if (p->ctx) {
        pw_context_destroy(p->ctx);
        p->ctx = NULL;
    }
    if (p->loop) {
        pw_thread_loop_destroy(p->loop);
        p->loop = NULL;
    }
    g_free(p->cur);
    g_free(p);
}
