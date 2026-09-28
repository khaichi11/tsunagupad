/* Ekstensi RTP "playout-delay" (WebRTC).
 *
 * Tanpa ekstensi ini Safari/Chrome menahan setiap frame di jitter buffer
 * (umumnya 50-150 ms di Wi-Fi) sebelum ditampilkan. Dengan min = max = 0 ms,
 * penerima masuk mode latensi rendah: frame ditampilkan begitu selesai
 * didekode. Cocok untuk layar interaktif; di jaringan buruk gambar bisa sedikit
 * patah-patah, tetapi tidak tertinggal.
 *
 * Format: 3 byte, MIN (12 bit) lalu MAX (12 bit), satuan 10 ms.
 * SPDX-License-Identifier: MIT
 */
#include <gst/rtp/gstrtphdrext.h>

#include "tsunagupad.h"

#define PLAYOUT_DELAY_URI "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay"
/* ID 1-14 untuk header satu byte; webrtcbin memakai ID kecil untuk "mid". */
#define PLAYOUT_DELAY_ID 14

typedef struct {
    GstRTPHeaderExtension parent;
} TsunaguPlayoutDelay;

typedef struct {
    GstRTPHeaderExtensionClass parent_class;
} TsunaguPlayoutDelayClass;

G_DEFINE_TYPE(TsunaguPlayoutDelay, tsunagu_playout_delay, GST_TYPE_RTP_HEADER_EXTENSION)

static GstRTPHeaderExtensionFlags get_supported_flags(GstRTPHeaderExtension *ext)
{
    return GST_RTP_HEADER_EXTENSION_ONE_BYTE;
}

static gsize get_max_size(GstRTPHeaderExtension *ext, const GstBuffer *input_meta)
{
    return 3;
}

static gssize write_extension(GstRTPHeaderExtension *ext, const GstBuffer *input_meta,
                              GstRTPHeaderExtensionFlags write_flags, GstBuffer *output,
                              guint8 *data, gsize size)
{
    if (size < 3)
        return -1;
    data[0] = data[1] = data[2] = 0;
    return 3;
}

static gboolean read_extension(GstRTPHeaderExtension *ext, GstRTPHeaderExtensionFlags read_flags,
                               const guint8 *data, gsize size, GstBuffer *buffer)
{
    return TRUE;
}

static void tsunagu_playout_delay_class_init(TsunaguPlayoutDelayClass *klass)
{
    GstRTPHeaderExtensionClass *ext_class = GST_RTP_HEADER_EXTENSION_CLASS(klass);
    ext_class->get_supported_flags = get_supported_flags;
    ext_class->get_max_size = get_max_size;
    ext_class->write = write_extension;
    ext_class->read = read_extension;

    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "Tsunagu-Pad playout delay",
                                          GST_RTP_HDREXT_ELEMENT_CLASS,
                                          "Minta penerima WebRTC menampilkan frame tanpa buffer",
                                          "Tsunagu-Pad");
    gst_rtp_header_extension_class_set_uri(ext_class, PLAYOUT_DELAY_URI);
}

static void tsunagu_playout_delay_init(TsunaguPlayoutDelay *self)
{
}

/* Payloader menulis ekstensi di tiap paket dan menambahkan extmap ke caps,
 * sehingga webrtcbin otomatis mencantumkannya di SDP offer. */
void playout_delay_attach(GstElement *payloader)
{
    GstRTPHeaderExtension *ext = g_object_new(tsunagu_playout_delay_get_type(), NULL);
    gst_rtp_header_extension_set_id(ext, PLAYOUT_DELAY_ID);
    g_signal_emit_by_name(payloader, "add-extension", ext);
    gst_object_unref(ext);
}
