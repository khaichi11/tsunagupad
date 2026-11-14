/* Tsunagu-Pad untuk Windows: fungsi yang menggantikan bagian khusus Linux.
 *
 *   sys.c      proses, alamat jaringan, kode acak, on/off/status
 *   display.c  daftar monitor, pengawas ukuran layar, layar kedua (monitor virtual)
 *   input.c    SendInput dan pena Windows Ink dengan tekanan dan kemiringan
 *   inhibit.c  menahan layar mati dan kunci otomatis
 *   caps.c     kemampuan mesin ini
 *   receive.c  AirPlay lewat UxPlay
 *   hotspot.c  Mobile Hotspot Windows
 *   service.c  layanan Windows untuk layar login dan sesi berikutnya
 *   setup.c    pembantu pemasang: driver layar virtual, PATH
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib.h>

#include "../tsunagupad.h"

/* Satu monitor aktif. Koordinat dalam piksel fisik pada desktop virtual,
 * karena proses ini sadar DPI per monitor. */
typedef struct {
    char device[64];            /* nama GDI, mis. \\.\DISPLAY1 */
    char name[128];             /* nama dari EDID, mis. "DELL U2419H" */
    guint64 handle;             /* HMONITOR */
    int x, y, width, height;
    int index;                  /* nomor urut mulai 1, sama dengan urutan `tsunagupad monitors` */
    gboolean primary;
    gboolean is_virtual;        /* monitor dari driver layar virtual */
    gboolean internal;          /* layar bawaan laptop */
} WinMonitor;

/* ---------- sys.c ---------- */

/* Dipanggil pertama kali di main(): argumen UTF-8, sadar DPI, keluaran
 * konsol UTF-8, dan lokasi plugin GStreamer yang ikut terpasang. */
char **win_init_process(int *argc, char **argv);
gboolean win_random(guint8 *buf, gsize len);
GPtrArray *win_local_addresses(void);
char *win_hotspot_address(void);       /* alamat di Mobile Hotspot, NULL bila mati */
char *win_find_web_dir(const char *given);
char *win_install_dir(void);            /* folder induk dari bin\ */
char *win_exe_path(void);
char *win_state_dir(void);              /* %LOCALAPPDATA%\tsunagupad, dibuat bila belum ada */
char *win_program_data_dir(void);       /* %ProgramData%\TsunaguPad */
void win_quit_watch(GMainLoop *loop);
void win_stop_stale_share(guint port);
guint32 win_port_owner(guint port);     /* PID yang mendengarkan port TCP, 0 bila tidak ada */
gboolean win_process_is(guint32 pid, const char *exe_name);
gboolean win_signal_quit(guint32 pid, guint timeout_ms);
void *win_create_quit_event(void);      /* HANDLE event bernama untuk diminta berhenti */
gboolean win_console_session(void);     /* proses ini ada di sesi konsol yang aktif */
gboolean win_is_elevated(void);
int win_cmd_on(int argc, char **argv);
int win_cmd_off(void);
int win_cmd_status(void);
char *win_find_uxplay(void);

/* Menjalankan program, menunggu selesai, dan menangkap keluarannya (stdout
 * dan stderr digabung). input boleh NULL; bila ada, dikirim ke stdin. */
gboolean win_run(const char *const *argv, const char *input, char **output, int *exit_code,
                 GError **error);
char *win_quote_args(const char *const *argv);
wchar_t *win_utf16(const char *utf8);
char *win_utf8(const wchar_t *utf16);

/* ---------- display.c ---------- */

GArray *win_monitors(void);             /* WinMonitor, urut: utama dulu */
gboolean win_monitor_find(const char *spec, WinMonitor *out);
gboolean win_monitor_by_device(const char *device, WinMonitor *out);
int win_cmd_monitors(void);
void win_screen_watch(App *app, GMainLoop *loop, gboolean *changed);

gboolean vdisp_available(void);
guint win_monitor_gpu_vendor(const char *device);   /* 0x8086 Intel, 0x10DE NVIDIA, 0x1002 AMD */
Vdisp *vdisp_open(int width, int height, double scale, GError **error);
const char *vdisp_device(Vdisp *v);
void vdisp_close(Vdisp *v);
int win_cmd_second_off(void);
gboolean vdisp_detach_all(void);

/* ---------- input.c ---------- */

void win_follow_input_desktop(gboolean force);

/* ---------- receive.c ---------- */

int win_cmd_receive(const char *name, int fps, gboolean fullscreen, gboolean pin, gboolean no_audio);

/* ---------- hotspot.c ---------- */

int win_cmd_hotspot(int argc, char **argv);
const char *win_hotspot_capability(void);  /* yes | no | unknown */
gboolean win_hotspot_active(void);

/* ---------- service.c ---------- */

#define TSUNAGUPAD_SERVICE_NAME L"TsunaguPad"
int win_cmd_service(int argc, char **argv);
gboolean win_service_installed(void);
gboolean win_service_request(const char *command, char **reply, GError **error);

/* ---------- setup.c ---------- */

int win_cmd_setup(int argc, char **argv);
gboolean display_add_mode(int width, int height, GError **error);
