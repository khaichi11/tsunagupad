/* Tsunagu-Pad, satukan layar Ubuntu dan iPad.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#ifndef TSUNAGUPAD_VERSION
#define TSUNAGUPAD_VERSION "dev"
#endif

typedef struct _Input Input;
typedef struct _Client Client;
typedef struct _Portal Portal;

typedef struct {
    /* opsi dari command line */
    guint port;
    int rx, ry, rw, rh;         /* area layar yang dibagikan (koordinat root X11; di Wayland: 0,0 dan ukuran layar) */
    gboolean have_region;
    char *backend;              /* auto | x11 | portal */
    Portal *portal;             /* sesi Wayland (xdg-desktop-portal); NULL di sesi X11 */
    char *quality;              /* hemat | lancar | seimbang | tajam */
    char *encoder;              /* auto | nvidia | intel | cpu */
    char *pen_mode;             /* auto | tablet | mouse */
    gboolean allow_input;
    gboolean test_source;
    char *web_dir;
    char *token;

    /* hasil turunan dari opsi */
    int out_w, out_h, fps;
    int kbps;                   /* batas atas bitrate dari preset kualitas */
    const char *enc_name;

    /* keadaan saat berjalan */
    GMainLoop *loop;
    SoupServer *server;
    Client *client;             /* iPad yang sedang terhubung (sudah lolos token) */
    guint session;              /* naik setiap ada klien baru, untuk membuang callback basi */
    GstElement *pipeline, *webrtc, *pay, *enc;
    gboolean offer_sent;
    int kbps_now;               /* bitrate encoder saat ini (adaptif) */
    guint stats_timer;
    Input *input;
} App;

/* main.c: kirim event satu baris JSON ke stdout untuk extension panel.
 * Argumen berupa pasangan kunci/nilai string, diakhiri NULL. */
void tsunagu_event(const char *event, ...) G_GNUC_NULL_TERMINATED;

/* server.c */
gboolean server_start(App *app, GError **error);
void server_stop(App *app);
void server_send(App *app, JsonBuilder *b);     /* kirim ke klien aktif */
const char *server_client_addr(App *app);

/* share.c */
gboolean share_start(App *app, GError **error);
void share_stop(App *app);
void share_on_answer(App *app, const char *sdp);
void share_on_ice(App *app, int mline, const char *candidate);
void share_keyframe(App *app);

/* encoder.c */
const char *encoder_pick(const char *wanted);
char *encoder_fragment(const char *name, int w, int h, int fps, int kbps, int max_kbps);
gboolean encoder_can_retune(const char *name);

/* playout.c */
void playout_delay_attach(GstElement *payloader);

/* portal.c: tangkap layar dan masukan di sesi Wayland (xdg-desktop-portal) */
Portal *portal_open(App *app, GError **error);   /* menunggu jendela izin GNOME */
void portal_free(Portal *p);
int portal_pipewire_fd(Portal *p);
guint portal_node(Portal *p);
void portal_geometry(Portal *p, int *x, int *y, int *w, int *h);
gboolean portal_can_control(Portal *p);
void portal_motion(Portal *p, double x, double y);              /* piksel di dalam layar yang dibagikan */
void portal_button(Portal *p, int evdev, gboolean down);
void portal_axis(Portal *p, int axis, int steps);               /* 0 vertikal, 1 horizontal; positif = bawah/kanan */
void portal_key(Portal *p, int evdev, gboolean down);
void portal_keysym(Portal *p, guint32 keysym, gboolean down);

/* input.c */
Input *input_new(App *app);
void input_free(Input *in);
void input_handle(Input *in, JsonObject *msg);
void input_release_all(Input *in);

/* qr.c */
char *qr_rows(const char *text);
