/* Deklarasi minimal GstWebRTC + GstSDP.
 *
 * Header resmi ada di libgstreamer-plugins-bad1.0-dev, tapi di Ubuntu paket itu
 * ikut menarik OpenCV dan ratusan MB dependensi. Tsunagu-Pad hanya butuh beberapa
 * fungsi di bawah ini, jadi cukup dideklarasikan di sini lalu di-link langsung
 * ke libgstwebrtc-1.0.so.0 dan libgstsdp-1.0.so.0 (ABI stabil sejak 1.x).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib-object.h>

typedef struct _GstSDPMessage GstSDPMessage;

typedef enum {
    GST_SDP_OK = 0,
    GST_SDP_EINVAL = -1,
} GstSDPResult;

GstSDPResult gst_sdp_message_new_from_text(const gchar *text, GstSDPMessage **msg);
gchar *gst_sdp_message_as_text(const GstSDPMessage *msg);
GstSDPResult gst_sdp_message_free(GstSDPMessage *msg);

typedef enum {
    GST_WEBRTC_SDP_TYPE_OFFER = 1,
    GST_WEBRTC_SDP_TYPE_PRANSWER,
    GST_WEBRTC_SDP_TYPE_ANSWER,
    GST_WEBRTC_SDP_TYPE_ROLLBACK,
} GstWebRTCSDPType;

typedef struct {
    GstWebRTCSDPType type;
    GstSDPMessage *sdp;
} GstWebRTCSessionDescription;

GType gst_webrtc_session_description_get_type(void);
GstWebRTCSessionDescription *gst_webrtc_session_description_new(GstWebRTCSDPType type,
                                                                GstSDPMessage *sdp);
void gst_webrtc_session_description_free(GstWebRTCSessionDescription *desc);

/* GstWebRTCRTPTransceiverDirection */
#define ICHI_TRANSCEIVER_SENDONLY 2
