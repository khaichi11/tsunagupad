/* Tsunagu-Pad, ikon di area notifikasi Windows.
 *
 * Padanan dari tombol Quick Settings GNOME (extension/): hanya antarmuka;
 * kerja beratnya dilakukan tsunagupad.exe yang dijalankan sebagai proses anak.
 * Baris JSON dari keluarannya dibaca sama persis seperti oleh extension.
 *
 *   klik kiri   panel kecil: tombol nyala/mati, QR sambungan, QR Wi-Fi hotspot
 *   klik kanan  menu: mode, monitor, layar kedua, kualitas, hotspot, dan lainnya
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define WM_TRAY        (WM_APP + 1)
#define WM_CHILD_LINE  (WM_APP + 2)
#define WM_CHILD_EXIT  (WM_APP + 3)
#define WM_TASK_DONE   (WM_APP + 4)
#define WM_PANEL_BACK  (WM_APP + 5)

#define SHARE_PORT 8765
#define RECEIVER_NAME "Tsunagu-Pad Windows"
#define QR_MAX 196

enum {
    ID_TOGGLE = 100, ID_MODE_SHARE, ID_MODE_RECEIVE, ID_SECOND, ID_FULLSCREEN, ID_SHOW_PANEL,
    ID_COPY, ID_RESET_TOKEN, ID_HOTSPOT, ID_HOTSPOT_CONFIG, ID_BOOT, ID_AUTOSTART, ID_EXIT,
    ID_SETTINGS, ID_MONITOR_PRIMARY = 300, ID_MONITOR_BASE = 301,
    ID_SIZE_BASE = 400, ID_SCALE_BASE = 420, ID_QUALITY_BASE = 440,
    ID_HOTSPOT_SAVE = 500, ID_HOTSPOT_CANCEL,
};

enum { TIMER_POLL = 1, TIMER_RECEIVER_RESTART, TIMER_COPY, TIMER_DISPLAY, TIMER_PROMOTE };
#define HOTKEY_PANEL 1

static const struct { const char *id, *label; } second_sizes[] = {
    { "1280x800", "Ringan (1280×800)" },
    { "1640x1024", "Sedang (1640×1024)" },
    { "2360x1640", "Tajam (2360×1640)" },
};
static const struct { const char *id, *label; } second_scales[] = {
    { "1", "100%" }, { "1.25", "125%" }, { "1.5", "150%" }, { "2", "200%" }, { "ikut", "Ikut monitor utama" },
};
static const struct { const char *id, *label, *caption; } qualities[] = {
    { "hemat", "Hemat", "Hemat data, untuk jaringan lemah" },
    { "lancar", "Lancar", "720p, paling responsif" },
    { "seimbang", "Seimbang", "1600 px, jernih dan tetap lancar" },
    { "tajam", "Tajam", "Resolusi asli layar" },
    { "monitor", "Monitor", "Sampai 1080p, 30 fps, tulisan tajam" },
};

/* ---------- keadaan ---------- */

typedef struct {
    char *mode;             /* share | receive */
    gboolean second;
    char *second_size;
    char *second_scale;
    char *monitor_name;     /* nama GDI, mis. \\.\DISPLAY2; kosong = utama */
    char *quality;
    gboolean fullscreen;
} Config;

typedef struct {
    char device[64], name[128];
    int width, height;
    gboolean primary, is_virtual;
} Monitor;

static Config config;
static HWND main_hwnd, panel_hwnd;
static HINSTANCE instance;
static NOTIFYICONDATAW nid;
static UINT taskbar_created;
static HICON icon_idle, icon_active;
static HFONT font_normal, font_bold, font_small;
static int dpi = 96;

static char *bin_dir, *daemon_exe;

/* proses anak */
static HANDLE child, child_job;
static DWORD child_pid;
static guint child_id;
static char *child_kind;
static gint64 started_at;
static int quick_exits;
static gboolean starting;

static char *status_text;
static char *receiver_error;    /* pesan galat terakhir dari UxPlay/GStreamer */
static char *receiver_network;  /* hotspot | lan: tempat penerima diumumkan */
static char **urls;
static char *qr_rows;
static char *last_client;
static gboolean panel_wanted;

/* kemampuan */
static gboolean caps_second, caps_service, caps_known;
static char *caps_hotspot;  /* yes | no | unknown */

/* hotspot */
static gboolean hotspot_active, hotspot_busy;
static char *hotspot_ssid, *hotspot_password, *hotspot_qr, *hotspot_qr_text;
static char *hotspot_shared;    /* nama jaringan internet laptop, mis. eduroam */

/* layanan */
static gboolean service_enabled, service_share;

static GArray *monitors;

static void on_command(UINT id);
static gboolean copied;
static HFONT font_icon, font_icon_small;

static int S(int px)
{
    return MulDiv(px, dpi, 96);
}

static wchar_t *W(const char *utf8)
{
    return (wchar_t *) g_utf8_to_utf16(utf8 ? utf8 : "", -1, NULL, NULL, NULL);
}

static char *U(const wchar_t *w)
{
    return g_utf16_to_utf8((const gunichar2 *) (w ? w : L""), -1, NULL, NULL, NULL);
}

static void set_str(char **slot, const char *value)
{
    g_free(*slot);
    *slot = g_strdup(value);
}

/* ---------- pengaturan (sama dengan panel.json milik extension) ---------- */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "tsunagupad", "panel.json", NULL);
}

static void config_load(void)
{
    set_str(&config.mode, "share");
    set_str(&config.second_size, second_sizes[0].id);
    set_str(&config.second_scale, "1");
    set_str(&config.monitor_name, "");
    set_str(&config.quality, "lancar");
    config.second = FALSE;
    config.fullscreen = FALSE;

    char *path = config_path();
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, path, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        const char *v;
        if ((v = json_object_get_string_member_with_default(o, "mode", NULL)) &&
            (g_str_equal(v, "share") || g_str_equal(v, "receive")))
            set_str(&config.mode, v);
        if ((v = json_object_get_string_member_with_default(o, "secondSize", NULL)))
            set_str(&config.second_size, v);
        if ((v = json_object_get_string_member_with_default(o, "secondScale", NULL)))
            set_str(&config.second_scale, v);
        if ((v = json_object_get_string_member_with_default(o, "monitorName", NULL)))
            set_str(&config.monitor_name, v);
        if ((v = json_object_get_string_member_with_default(o, "quality", NULL)))
            set_str(&config.quality, v);
        config.second = json_object_get_boolean_member_with_default(o, "second", FALSE);
        config.fullscreen = json_object_get_boolean_member_with_default(o, "fullscreen", FALSE);
    }
    g_object_unref(p);
    g_free(path);
}

static void config_save(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mode");
    json_builder_add_string_value(b, config.mode);
    json_builder_set_member_name(b, "second");
    json_builder_add_boolean_value(b, config.second);
    json_builder_set_member_name(b, "secondSize");
    json_builder_add_string_value(b, config.second_size);
    json_builder_set_member_name(b, "secondScale");
    json_builder_add_string_value(b, config.second_scale);
    json_builder_set_member_name(b, "monitorName");
    json_builder_add_string_value(b, config.monitor_name);
    json_builder_set_member_name(b, "quality");
    json_builder_add_string_value(b, config.quality);
    json_builder_set_member_name(b, "fullscreen");
    json_builder_add_boolean_value(b, config.fullscreen);
    json_builder_set_member_name(b, "tuning");
    json_builder_add_int_value(b, 2);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_pretty(g, TRUE);
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    json_generator_to_file(g, path, NULL);
    g_free(dir);
    g_free(path);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
}

/* ---------- menjalankan tsunagupad.exe ---------- */

static char *quote_args(const char *const *argv)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        if (i)
            g_string_append_c(out, ' ');
        if (*arg && !strpbrk(arg, " \t\n\v\"")) {
            g_string_append(out, arg);
            continue;
        }
        g_string_append_c(out, '"');
        for (const char *p = arg;; p++) {
            guint slashes = 0;
            while (*p == '\\') {
                p++;
                slashes++;
            }
            if (!*p) {
                for (guint k = 0; k < slashes * 2; k++)
                    g_string_append_c(out, '\\');
                break;
            }
            for (guint k = 0; k < (*p == '"' ? slashes * 2 + 1 : slashes); k++)
                g_string_append_c(out, '\\');
            g_string_append_c(out, *p);
        }
        g_string_append_c(out, '"');
    }
    return g_string_free(out, FALSE);
}

/* Menjalankan tsunagupad.exe sampai selesai (untuk perintah singkat). */
static char *run_tool(const char *const *args, const char *input, int *code)
{
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, daemon_exe);
    for (guint i = 0; args[i]; i++)
        g_ptr_array_add(argv, (char *) args[i]);
    g_ptr_array_add(argv, NULL);
    char *cmd = quote_args((const char *const *) argv->pdata);
    g_ptr_array_free(argv, TRUE);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out_r, out_w, in_r, in_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
        g_free(cmd);
        return NULL;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi = { 0 };
    wchar_t *wcmd = W(cmd);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    g_free(wcmd);
    g_free(cmd);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(in_w);
        return NULL;
    }
    DWORD n;
    if (input)
        WriteFile(in_w, input, (DWORD) strlen(input), &n, NULL);
    CloseHandle(in_w);
    GString *buf = g_string_new(NULL);
    char chunk[4096];
    while (ReadFile(out_r, chunk, sizeof chunk, &n, NULL) && n > 0)
        g_string_append_len(buf, chunk, n);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code)
        *code = (int) exit_code;
    return g_string_free(buf, FALSE);
}

/* Objek JSON terakhir di keluaran sebuah perintah. */
static JsonObject *last_json(const char *out, JsonParser **keep)
{
    if (!out)
        return NULL;
    JsonObject *found = NULL;
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = (int) g_strv_length(lines) - 1; i >= 0 && !found; i--) {
        char *l = g_strstrip(lines[i]);
        if (l[0] != '{')
            continue;
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, l, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            found = json_node_get_object(json_parser_get_root(p));
            *keep = p;
        } else {
            g_object_unref(p);
        }
    }
    g_strfreev(lines);
    return found;
}

/* ---------- pekerjaan latar ---------- */

typedef enum { TASK_CAPS, TASK_HOTSPOT_STATUS, TASK_HOTSPOT_SET, TASK_HOTSPOT_CONFIG, TASK_WIFI_QR,
               TASK_SERVICE_STATUS, TASK_SERVICE_TOGGLE, TASK_MONITORS } TaskKind;

typedef struct {
    TaskKind kind;
    char *arg, *arg2, *input;
    char *output;
    int code;
} Task;

static DWORD WINAPI task_thread(LPVOID p)
{
    Task *t = p;
    switch (t->kind) {
    case TASK_CAPS: {
        const char *a[] = { "capabilities", NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_HOTSPOT_STATUS: {
        const char *a[] = { "hotspot", "status", NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_HOTSPOT_SET: {
        const char *a[] = { "hotspot", t->arg, NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_HOTSPOT_CONFIG: {
        const char *a[] = { "hotspot", "config", t->arg, NULL };
        t->output = run_tool(a, t->input, &t->code);
        break;
    }
    case TASK_WIFI_QR: {
        const char *a[] = { "qr", t->arg, NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_SERVICE_STATUS: {
        const char *a[] = { "service", "status", NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_MONITORS: {
        const char *a[] = { "monitors", NULL };
        t->output = run_tool(a, NULL, &t->code);
        break;
    }
    case TASK_SERVICE_TOGGLE: {
        /* Menyalakan atau mematikan layanan butuh administrator: UAC. */
        wchar_t *file = W(daemon_exe);
        char *params = g_str_equal(t->arg, "enable")
            ? g_strdup_printf("service enable --token-file \"%s\"", t->arg2)
            : g_strdup("service disable");
        wchar_t *wparams = W(params);
        SHELLEXECUTEINFOW sei = { .cbSize = sizeof sei };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = file;
        sei.lpParameters = wparams;
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 60000);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            t->code = (int) code;
            CloseHandle(sei.hProcess);
        } else {
            t->code = -1;
        }
        g_free(file);
        g_free(wparams);
        g_free(params);
        break;
    }
    }
    PostMessageW(main_hwnd, WM_TASK_DONE, 0, (LPARAM) t);
    return 0;
}

static void task_run(TaskKind kind, const char *arg, const char *arg2, const char *input)
{
    Task *t = g_new0(Task, 1);
    t->kind = kind;
    t->arg = g_strdup(arg);
    t->arg2 = g_strdup(arg2);
    t->input = g_strdup(input);
    HANDLE h = CreateThread(NULL, 0, task_thread, t, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* ---------- notifikasi dan ikon ---------- */

static void notify(const char *message)
{
    NOTIFYICONDATAW n = nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcscpy(n.szInfoTitle, L"Tsunagu-Pad");
    wchar_t *w = W(message);
    wcsncpy(n.szInfo, w, G_N_ELEMENTS(n.szInfo) - 1);
    n.szInfo[G_N_ELEMENTS(n.szInfo) - 1] = 0;
    g_free(w);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

/* Ikon digambar sendiri: tablet dengan layar; terisi biru saat aktif. */
static HICON make_icon(gboolean active, int size)
{
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *) &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    guint32 *px = bits;
    double s = size / 16.0;
    guint32 frame = active ? 0xFF0A84FF : 0xFFFFFFFF;
    guint32 shade = 0xFF202020;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double fx = x / s, fy = y / s;
            guint32 v = 0;
            /* badan tablet: persegi panjang bersudut bulat 2..14 x 3..13 */
            gboolean body = fx >= 1.5 && fx < 14.5 && fy >= 2.5 && fy < 13.5;
            gboolean corner = (fx < 2.5 || fx >= 13.5) && (fy < 3.5 || fy >= 12.5);
            if (body && !corner) {
                v = shade;
                gboolean edge = fx < 2.5 || fx >= 13.5 || fy < 3.5 || fy >= 12.5;
                if (edge)
                    v = frame;
                else if (active)
                    v = 0xFF0A84FF;
                if (!active && !edge && fx >= 6 && fx < 10 && fy >= 6.5 && fy < 9.5)
                    v = 0xFFFFFFFF;   /* titik di tengah layar */
            }
            px[y * size + x] = v;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

static gboolean running(void)
{
    return child != NULL || service_share;
}

static void update_tray(void)
{
    HICON icon = running() ? icon_active : icon_idle;
    char *tip = g_strdup_printf("Tsunagu-Pad — %s", status_text ? status_text : "");
    wchar_t *w = W(tip);
    wchar_t next[G_N_ELEMENTS(nid.szTip)];
    wcsncpy(next, w, G_N_ELEMENTS(next) - 1);
    next[G_N_ELEMENTS(next) - 1] = 0;
    g_free(w);
    g_free(tip);
    /* Statistik datang tiap detik; Explorer hanya diberi tahu bila berubah. */
    if (nid.hIcon == icon && wcscmp(nid.szTip, next) == 0)
        return;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = icon;
    wcscpy(nid.szTip, next);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void sync_ui(void);
static void position_panel(gboolean raise);

/* ---------- proses tsunagupad ---------- */

typedef struct {
    HANDLE pipe, process;
    guint id;
} Reader;

static DWORD WINAPI reader_thread(LPVOID p)
{
    Reader *r = p;
    GString *line = g_string_new(NULL);
    char buf[4096];
    DWORD n;
    while (ReadFile(r->pipe, buf, sizeof buf, &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                g_strchomp(line->str);
                PostMessageW(main_hwnd, WM_CHILD_LINE, r->id, (LPARAM) g_strdup(line->str));
                g_string_truncate(line, 0);
            } else {
                g_string_append_c(line, buf[i]);
            }
        }
    }
    if (line->len)
        PostMessageW(main_hwnd, WM_CHILD_LINE, r->id, (LPARAM) g_strdup(line->str));
    g_string_free(line, TRUE);
    CloseHandle(r->pipe);
    WaitForSingleObject(r->process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(r->process, &code);
    CloseHandle(r->process);
    PostMessageW(main_hwnd, WM_CHILD_EXIT, r->id, (LPARAM) code);
    g_free(r);
    return 0;
}

static gboolean child_spawn(GPtrArray *args, const char *kind)
{
    g_ptr_array_add(args, NULL);
    char *cmd = quote_args((const char *const *) args->pdata);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out_r, out_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        g_free(cmd);
        return FALSE;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = out_w;
    si.hStdError = out_w;   /* digabung, supaya pesan UxPlay/GStreamer tidak membisu */
    PROCESS_INFORMATION pi = { 0 };
    wchar_t *wcmd = W(cmd);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             NULL, NULL, &si, &pi);
    g_free(wcmd);
    g_free(cmd);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(out_r);
        return FALSE;
    }
    /* Job object: bila panel ini mati mendadak, proses anaknya ikut berhenti. */
    if (child_job)
        AssignProcessToJobObject(child_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    HANDLE dup = NULL;
    DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    child = pi.hProcess;
    child_pid = pi.dwProcessId;
    child_id++;
    set_str(&child_kind, kind);
    started_at = g_get_monotonic_time();

    Reader *r = g_new0(Reader, 1);
    r->pipe = out_r;
    r->process = dup;
    r->id = child_id;
    HANDLE t = CreateThread(NULL, 0, reader_thread, r, 0, NULL);
    if (t)
        CloseHandle(t);
    return TRUE;
}

/* Meminta proses berhenti dengan rapi lewat event bernama, sama seperti SIGTERM
 * di Linux: tsunagupad.exe melepas monitor virtual dan tombol yang masih
 * tertekan sebelum keluar. */
static gboolean signal_quit(DWORD pid, HANDLE process, DWORD timeout)
{
    HANDLE event = NULL;
    static const wchar_t *const fmt[] = { L"Local\\tsunagupad-quit-%lu", L"Global\\tsunagupad-quit-%lu" };
    for (guint i = 0; i < G_N_ELEMENTS(fmt) && !event; i++) {
        wchar_t name[64];
        _snwprintf(name, G_N_ELEMENTS(name), fmt[i], (unsigned long) pid);
        name[63] = 0;
        event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    }
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
    HANDLE p = process;
    if (!p)
        p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!p)
        return FALSE;
    gboolean gone = WaitForSingleObject(p, event ? timeout : 0) == WAIT_OBJECT_0;
    if (!gone && TerminateProcess(p, 1))
        gone = WaitForSingleObject(p, 2000) == WAIT_OBJECT_0;
    if (!process)
        CloseHandle(p);
    return gone;
}

static void child_stop(void)
{
    KillTimer(main_hwnd, TIMER_RECEIVER_RESTART);
    if (!child)
        return;
    HANDLE p = child;
    DWORD pid = child_pid;
    child = NULL;
    child_pid = 0;
    child_id++;   /* baris dan kabar keluar dari proses lama diabaikan */
    set_str(&child_kind, NULL);
    signal_quit(pid, p, 6000);
    CloseHandle(p);
}

/* PID yang mendengarkan port berbagi layar, untuk mengenali sesi milik layanan. */
static DWORD port_owner(guint port)
{
    DWORD size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        ERROR_INSUFFICIENT_BUFFER)
        return 0;
    size += 1024;
    MIB_TCPTABLE_OWNER_PID *t = g_malloc(size);
    DWORD pid = 0;
    if (GetExtendedTcpTable(t, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR)
        for (DWORD i = 0; i < t->dwNumEntries && !pid; i++)
            if (ntohs((u_short) t->table[i].dwLocalPort) == port)
                pid = t->table[i].dwOwningPid;
    g_free(t);
    return pid;
}

static gboolean pid_is_daemon(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    PROCESSENTRY32W e = { .dwSize = sizeof e };
    gboolean match = FALSE;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == pid) {
            match = _wcsicmp(e.szExeFile, L"tsunagupad.exe") == 0;
            break;
        }
    CloseHandle(snap);
    return match;
}

static void refresh_service_share(void)
{
    DWORD pid = port_owner(SHARE_PORT);
    gboolean now = pid && pid != child_pid && pid_is_daemon(pid) && !child;
    if (now != service_share) {
        service_share = now;
        sync_ui();
    }
}

/* ---------- monitor ---------- */

/* Daftar monitor disimpan dan diperbarui di latar belakang (saat program
 * mulai dan saat susunan layar berubah). Membuka menu tidak lagi menunggu
 * tsunagupad.exe selesai, yang dulu membuat panel membeku sesaat. */
static gboolean monitors_loaded;

static void parse_monitors(const char *out)
{
    if (!monitors)
        monitors = g_array_new(FALSE, TRUE, sizeof(Monitor));
    JsonParser *p = NULL;
    JsonObject *o = last_json(out, &p);
    JsonArray *list = o && json_object_has_member(o, "monitors") ? json_object_get_array_member(o, "monitors") : NULL;
    GArray *fresh = g_array_new(FALSE, TRUE, sizeof(Monitor));
    for (guint i = 0; list && i < json_array_get_length(list); i++) {
        JsonObject *m = json_array_get_object_element(list, i);
        Monitor mon;
        memset(&mon, 0, sizeof mon);
        g_strlcpy(mon.device, json_object_get_string_member_with_default(m, "device", ""), sizeof mon.device);
        g_strlcpy(mon.name, json_object_get_string_member_with_default(m, "name", ""), sizeof mon.name);
        mon.width = (int) json_object_get_int_member_with_default(m, "width", 0);
        mon.height = (int) json_object_get_int_member_with_default(m, "height", 0);
        mon.primary = json_object_get_boolean_member_with_default(m, "primary", FALSE);
        mon.is_virtual = json_object_get_boolean_member_with_default(m, "virtual", FALSE);
        if (!mon.is_virtual)
            g_array_append_val(fresh, mon);
    }
    if (list) {
        g_array_set_size(monitors, 0);
        g_array_append_vals(monitors, fresh->data, fresh->len);
        monitors_loaded = TRUE;
    }
    g_array_free(fresh, TRUE);
    if (p)
        g_object_unref(p);
}

static void load_monitors(void)
{
    if (monitors_loaded)
        return;
    const char *a[] = { "monitors", NULL };
    char *out = run_tool(a, NULL, NULL);
    parse_monitors(out);
    g_free(out);
}

static const Monitor *selected_monitor(void)
{
    if (!monitors)
        return NULL;
    for (guint i = 0; i < monitors->len; i++) {
        const Monitor *m = &g_array_index(monitors, Monitor, i);
        if (config.monitor_name && *config.monitor_name && g_ascii_strcasecmp(m->device, config.monitor_name) == 0)
            return m;
    }
    for (guint i = 0; i < monitors->len; i++)
        if (g_array_index(monitors, Monitor, i).primary)
            return &g_array_index(monitors, Monitor, i);
    return monitors->len ? &g_array_index(monitors, Monitor, 0) : NULL;
}

/* ---------- mulai dan berhenti ---------- */

static gboolean share_mode(void)
{
    return g_str_equal(config.mode, "share");
}

static void start(void)
{
    if (quick_exits == 0)
        set_str(&receiver_error, NULL);
    if (starting || child)
        return;
    starting = TRUE;
    GPtrArray *args = g_ptr_array_new_with_free_func(NULL);
    g_ptr_array_add(args, daemon_exe);
    const char *kind = config.mode;
    if (share_mode()) {
        g_ptr_array_add(args, "share");
        g_ptr_array_add(args, "--quality");
        g_ptr_array_add(args, config.quality);
        if (config.second && caps_second) {
            g_ptr_array_add(args, "--second");
            g_ptr_array_add(args, config.second_size);
            g_ptr_array_add(args, "--scale");
            g_ptr_array_add(args, config.second_scale);
        } else {
            load_monitors();
            const Monitor *m = selected_monitor();
            if (m) {
                if (config.monitor_name && *config.monitor_name && g_ascii_strcasecmp(m->device, config.monitor_name) != 0) {
                    char *msg = g_strdup_printf("Monitor %s terlepas; Tsunagu-Pad pindah ke %s.", config.monitor_name, m->name);
                    notify(msg);
                    g_free(msg);
                }
                g_ptr_array_add(args, "--monitor");
                g_ptr_array_add(args, (char *) m->device);
            }
        }
    } else {
        g_ptr_array_add(args, "receive");
        g_ptr_array_add(args, "--name");
        g_ptr_array_add(args, RECEIVER_NAME);
        if (config.fullscreen)
            g_ptr_array_add(args, "--fullscreen");
    }
    g_strfreev(urls);
    urls = NULL;
    set_str(&qr_rows, NULL);
    set_str(&last_client, NULL);
    if (child_spawn(args, kind)) {
        set_str(&status_text, share_mode() ? "Menyiapkan…" : "Menunggu iPad…");
        panel_wanted = share_mode();
    } else {
        notify("tsunagupad.exe tidak bisa dijalankan. Pasang ulang Tsunagu-Pad.");
    }
    g_ptr_array_free(args, TRUE);
    starting = FALSE;
    sync_ui();
}

static void restart(void)
{
    child_stop();
    start();
}

static void toggle(void)
{
    if (child) {
        child_stop();
        set_str(&status_text, NULL);
        sync_ui();
    } else if (service_share) {
        /* Sesi milik layanan (layar login / sesi berikutnya): dihentikan
         * sampai sesi berikutnya, seperti `systemctl --user stop` di Linux. */
        DWORD pid = port_owner(SHARE_PORT);
        if (pid)
            signal_quit(pid, NULL, 6000);
        service_share = FALSE;
        sync_ui();
    } else {
        start();
    }
}

static void on_child_line(const char *line)
{
    gboolean share = g_strcmp0(child_kind, "share") == 0;
    if (line[0] == '{') {
        JsonParser *p = json_parser_new();
        if (!json_parser_load_from_data(p, line, -1, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            g_object_unref(p);
            return;
        }
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        const char *ev = json_object_get_string_member_with_default(o, "event", "");
        const char *state = json_object_get_string_member_with_default(o, "state", "");
        const char *message = json_object_get_string_member_with_default(o, "message", "");
        const char *addr = json_object_get_string_member_with_default(o, "addr", "");
        if (g_str_equal(ev, "receiver")) {
            set_str(&receiver_network, json_object_get_string_member_with_default(o, "network", "lan"));
        } else if (g_str_equal(ev, "ready")) {
            g_strfreev(urls);
            urls = g_strsplit(json_object_get_string_member_with_default(o, "urls", ""), " ", -1);
            set_str(&qr_rows, json_object_get_string_member_with_default(o, "qr", ""));
            set_str(&status_text, "Menunggu iPad…");
            if (panel_wanted) {
                panel_wanted = FALSE;
                SendMessageW(main_hwnd, WM_COMMAND, ID_SHOW_PANEL, 0);
            }
        } else if (g_str_equal(ev, "client")) {
            if (g_str_equal(state, "connected")) {
                set_str(&status_text, "Terhubung ke iPad");
                /* Setiap perangkat baru yang memegang kendali diberitahukan. */
                if (*addr && g_strcmp0(addr, last_client) != 0) {
                    set_str(&last_client, addr);
                    char *msg = g_strdup_printf("iPad tersambung dari %s dan bisa mengendalikan layar ini.", addr);
                    notify(msg);
                    g_free(msg);
                }
            } else if (g_str_equal(state, "disconnected")) {
                set_str(&status_text, "Menunggu iPad…");
            } else if (g_str_equal(state, "rejected")) {
                set_str(&status_text, "Kode akses ditolak");
            } else if (g_str_equal(state, "locked")) {
                char *msg = g_strdup_printf("Terlalu banyak kode akses salah dari %s; alamat itu diblokir sebentar.", addr);
                notify(msg);
                g_free(msg);
            }
        } else if (g_str_equal(ev, "stats")) {
            const char *fps = json_object_get_string_member_with_default(o, "fps", "0");
            char *text = atoi(fps) > 0 ? g_strdup_printf("iPad · %s fps", fps) : g_strdup("Terhubung ke iPad");
            set_str(&status_text, text);
            g_free(text);
        } else if (g_str_equal(ev, "error")) {
            notify(message);
        } else if (g_str_equal(ev, "warning")) {
            /* Gangguan aliran yang dipulihkan sendiri cukup di log. */
            if (!g_str_has_prefix(message, "Streaming berhenti"))
                notify(message);
        }
        g_object_unref(p);
    } else if (!share) {
        /* Log UxPlay */
        char *text = g_ascii_strdown(line, -1);
        if (strstr(text, "error") || strstr(text, "failed") || strstr(text, "no element")) {
            char *clean = g_strstrip(g_strdup(line));
            if (*clean && !g_str_has_prefix(clean, "***"))
                set_str(&receiver_error, clean);
            g_free(clean);
        }
        if (strstr(text, "connection request from"))
            set_str(&status_text, "iPad menyambung…");
        else if (strstr(text, "begin streaming"))
            set_str(&status_text, "Menampilkan layar iPad");
        else if (strstr(text, "connection closed"))
            set_str(&status_text, "Menunggu iPad…");
        g_free(text);
    }
    sync_ui();
}

static void on_child_exit(DWORD code)
{
    char *kind = g_strdup(child_kind);
    if (child) {
        CloseHandle(child);
        child = NULL;
        child_pid = 0;
    }
    set_str(&child_kind, NULL);

    if (g_strcmp0(kind, "receive") == 0 && !share_mode()) {
        gboolean quick = g_get_monotonic_time() - started_at < 10 * G_USEC_PER_SEC;
        quick_exits = quick ? quick_exits + 1 : 0;
        if (quick_exits >= 3) {
            quick_exits = 0;
            char *msg = receiver_error
                ? g_strdup_printf("Penerima AirPlay tidak bisa berjalan: %s", receiver_error)
                : g_strdup("Penerima AirPlay berhenti berulang kali. Pastikan tidak ada UxPlay lain yang berjalan, "
                           "lalu nyalakan lagi.");
            notify(msg);
            g_free(msg);
            set_str(&status_text, NULL);
        } else {
            set_str(&status_text, "Penerima berhenti, menyalakan ulang…");
            SetTimer(main_hwnd, TIMER_RECEIVER_RESTART, 2000, NULL);
        }
    } else if (g_strcmp0(kind, "share") == 0 && code == 3) {
        /* Ukuran atau letak monitor berubah: nyalakan lagi dengan ukuran baru. */
        start();
    } else {
        set_str(&status_text, NULL);
    }
    g_free(kind);
    sync_ui();
}

/* ---------- hotspot ---------- */

static char *wifi_qr_text(const char *ssid, const char *password)
{
    GString *out = g_string_new("WIFI:T:WPA;S:");
    for (int k = 0; k < 2; k++) {
        const char *s = k ? password : ssid;
        for (const char *p = s; p && *p; p++) {
            if (strchr("\\;,:\"", *p))
                g_string_append_c(out, '\\');
            g_string_append_c(out, *p);
        }
        g_string_append(out, k ? ";;" : ";P:");
    }
    return g_string_free(out, FALSE);
}

static void hotspot_apply(JsonObject *o)
{
    const char *ev = json_object_get_string_member_with_default(o, "event", "");
    if (g_str_equal(ev, "error")) {
        char *msg = g_strdup_printf("Hotspot gagal: %s", json_object_get_string_member_with_default(o, "message", "?"));
        notify(msg);
        g_free(msg);
        return;
    }
    gboolean was_active = hotspot_active;
    hotspot_active = g_strcmp0(json_object_get_string_member_with_default(o, "state", ""), "active") == 0;
    /* Penerima AirPlay diumumkan di hotspot bila menyala, di jaringan biasa
     * bila tidak; saat hotspot berubah, penerimanya dinyalakan ulang. */
    if (was_active != hotspot_active && g_strcmp0(child_kind, "receive") == 0)
        restart();
    set_str(&hotspot_ssid, json_object_get_string_member_with_default(o, "ssid", ""));
    set_str(&hotspot_password, json_object_get_string_member_with_default(o, "password", ""));
    set_str(&hotspot_shared, json_object_get_string_member_with_default(o, "shared", ""));
    if (hotspot_active && hotspot_ssid && *hotspot_ssid) {
        char *text = wifi_qr_text(hotspot_ssid, hotspot_password);
        if (g_strcmp0(text, hotspot_qr_text) != 0) {
            set_str(&hotspot_qr_text, text);
            task_run(TASK_WIFI_QR, text, NULL, NULL);
        }
        g_free(text);
    }
}

static void on_task_done(Task *t)
{
    JsonParser *p = NULL;
    JsonObject *o = last_json(t->output, &p);
    switch (t->kind) {
    case TASK_MONITORS:
        parse_monitors(t->output);
        break;
    case TASK_CAPS:
        if (o) {
            caps_known = TRUE;
            caps_second = g_strcmp0(json_object_get_string_member_with_default(o, "second", ""), "yes") == 0;
            caps_service = g_strcmp0(json_object_get_string_member_with_default(o, "service", ""), "yes") == 0;
            set_str(&caps_hotspot, json_object_get_string_member_with_default(o, "hotspot", "unknown"));
            if (caps_service)
                task_run(TASK_SERVICE_STATUS, NULL, NULL, NULL);
            if (g_strcmp0(caps_hotspot, "no") != 0)
                task_run(TASK_HOTSPOT_STATUS, NULL, NULL, NULL);
        }
        break;
    case TASK_HOTSPOT_STATUS:
        if (o && !hotspot_busy)
            hotspot_apply(o);
        break;
    case TASK_HOTSPOT_SET:
        hotspot_busy = FALSE;
        if (o)
            hotspot_apply(o);
        /* Alamat komputer untuk iPad berubah, jadi QR sambungan dibuat ulang. */
        if (g_strcmp0(child_kind, "share") == 0)
            restart();
        break;
    case TASK_HOTSPOT_CONFIG:
        if (o) {
            hotspot_apply(o);
            if (g_str_equal(json_object_get_string_member_with_default(o, "event", ""), "hotspot")) {
                char *msg = g_strdup_printf("Hotspot disimpan: “%s”.", hotspot_ssid ? hotspot_ssid : "");
                notify(msg);
                g_free(msg);
            }
        } else {
            notify("Hotspot gagal disimpan.");
        }
        break;
    case TASK_WIFI_QR:
        if (o && g_strcmp0(t->arg, hotspot_qr_text) == 0)
            set_str(&hotspot_qr, json_object_get_string_member_with_default(o, "rows", ""));
        break;
    case TASK_SERVICE_STATUS:
        if (o) {
            service_enabled = g_strcmp0(json_object_get_string_member_with_default(o, "enabled", ""), "yes") == 0;
            caps_service = g_strcmp0(json_object_get_string_member_with_default(o, "installed", ""), "yes") == 0;
        }
        break;
    case TASK_SERVICE_TOGGLE:
        if (t->code == 0)
            notify(g_str_equal(t->arg, "enable")
                   ? "iPad aktif di layar login dan sesi berikutnya."
                   : "iPad tidak lagi aktif di sesi berikutnya.");
        else if (t->code != -1)
            notify("Pengaturan layanan gagal diubah.");
        task_run(TASK_SERVICE_STATUS, NULL, NULL, NULL);
        break;
    }
    if (p)
        g_object_unref(p);
    g_free(t->arg);
    g_free(t->arg2);
    if (t->input) {
        memset(t->input, 0, strlen(t->input));
        g_free(t->input);
    }
    g_free(t->output);
    g_free(t);
    sync_ui();
}

/* ---------- lain-lain ---------- */

static char *token_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "tsunagupad", "token", NULL);
}

static void reset_token(void)
{
    char *path = token_path();
    g_remove(path);
    g_free(path);
    if (g_strcmp0(child_kind, "share") == 0)
        restart();
    if (service_enabled)
        notify("Kode akses lama tidak berlaku lagi. Pindai QR yang baru. Matikan lalu nyalakan lagi "
               "\"iPad aktif di sesi berikutnya\" agar layar login juga memakai kode baru.");
    else
        notify("Kode akses lama tidak berlaku lagi. Pindai QR yang baru dari iPad.");
}

static void copy_address(void)
{
    if (!urls || !urls[0] || !OpenClipboard(main_hwnd))
        return;
    EmptyClipboard();
    wchar_t *w = W(urls[0]);
    gsize bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), w, bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    g_free(w);
    copied = TRUE;
    if (panel_hwnd)
        InvalidateRect(panel_hwnd, NULL, FALSE);
    SetTimer(main_hwnd, TIMER_COPY, 1500, NULL);
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static gboolean autostart_enabled(void)
{
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"Tsunagu-Pad", RRF_RT_REG_SZ, NULL, NULL, &size) == ERROR_SUCCESS;
}

static void autostart_set(gboolean on)
{
    if (on) {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(NULL, exe, G_N_ELEMENTS(exe));
        wchar_t value[MAX_PATH * 2 + 32];
        _snwprintf(value, G_N_ELEMENTS(value), L"\"%ls\" --autostart", exe);
        value[G_N_ELEMENTS(value) - 1] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"Tsunagu-Pad", REG_SZ, value,
                        (DWORD) ((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"Tsunagu-Pad");
    }
}

/* ---------- menu ---------- */

/* ---------- panel ----------
 * Windows tidak mengizinkan aplikasi lain menambah tombol ke Quick Settings,
 * jadi Tsunagu-Pad punya panel sendiri tepat di atas jam, dengan tema yang
 * sama dengan Windows 11. Semua pilihan ada di satu panel yang bisa digulir:
 * sakelar utama, arah, QR, layar, kualitas, hotspot, dan pengaturan. Tanpa
 * menu putih bawaan Windows dan tanpa halaman terpisah. */

#define PANEL_W 360
#define SS 3        /* bentuk digambar 3x lalu diperkecil supaya sudutnya halus */
#define SWITCH_W 40
#define SWITCH_H 20
#define MAX_OPTS 6

enum { ROW_TITLE, ROW_HEADER, ROW_SEGMENT, ROW_RADIO, ROW_TOGGLE, ROW_ACTION, ROW_NOTE, ROW_INFO,
       ROW_QR, ROW_BUTTONS, ROW_FIELD };

typedef struct {
    int kind;
    UINT cmd;                   /* perintah on_command; ROW_FIELD: nomor isian; ROW_QR: 1 = hotspot */
    char *label, *caption;
    gboolean checked, enabled;
    const char *qr;             /* ROW_QR: modul QR (dipinjam) */
    int nopts, selected;        /* ROW_SEGMENT, ROW_BUTTONS (selected = tombol utama) */
    char *opts[MAX_OPTS];
    UINT opt_cmd[MAX_OPTS];
    RECT opt_box[MAX_OPTS];
    RECT box, hit;              /* koordinat isi panel, sebelum digulir */
} Row;

static Row rows[64];
static int nrows;
static int hover_row = -1, hover_opt = -1;
static gboolean mouse_tracking;
static int panel_height, content_height, scroll_y;
static int qr_dip = QR_MAX, hqr_dip = 150;      /* diperkecil bila layar pendek */
static gboolean hotspot_editing;
static HWND edit_ssid, edit_password;
static WNDPROC edit_base_proc;
static char *hotspot_note;                      /* pesan isian hotspot yang salah */
static HBRUSH field_brush;
static COLORREF field_brush_color = CLR_INVALID;

static gboolean dark_theme(void)
{
    DWORD light = 0, size = sizeof light;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) != ERROR_SUCCESS)
        return TRUE;
    return light == 0;
}

typedef struct {
    COLORREF bg, fg, muted, tile, tile_hover, border, accent, accent_hover, on_fg, line;
} Palette;

static Palette palette(void)
{
    if (dark_theme())
        return (Palette) { RGB(0x20, 0x20, 0x20), RGB(0xFF, 0xFF, 0xFF), RGB(0xA6, 0xA6, 0xA6),
                           RGB(0x2D, 0x2D, 0x2D), RGB(0x37, 0x37, 0x37), RGB(0x3A, 0x3A, 0x3A),
                           RGB(0x4C, 0xC2, 0xFF), RGB(0x47, 0xB1, 0xE8), RGB(0x00, 0x00, 0x00),
                           RGB(0x33, 0x33, 0x33) };
    return (Palette) { RGB(0xF3, 0xF3, 0xF3), RGB(0x1B, 0x1B, 0x1B), RGB(0x60, 0x60, 0x60),
                       RGB(0xFB, 0xFB, 0xFB), RGB(0xF0, 0xF0, 0xF0), RGB(0xE0, 0xE0, 0xE0),
                       RGB(0x00, 0x5F, 0xB8), RGB(0x19, 0x6E, 0xBF), RGB(0xFF, 0xFF, 0xFF),
                       RGB(0xE5, 0xE5, 0xE5) };
}

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM data)
{
    *(gboolean *) data = TRUE;
    return 0;
}

static HFONT icon_font(int px)
{
    /* Ikon diambil dari font ikon bawaan Windows (Segoe Fluent Icons di
     * Windows 11, Segoe MDL2 Assets di Windows 10), tanpa berkas gambar. */
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Segoe Fluent Icons");
    HDC dc = GetDC(NULL);
    gboolean fluent = FALSE;
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM) &fluent, 0);
    ReleaseDC(NULL, dc);
    lf.lfHeight = -px;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    return CreateFontIndirectW(&lf);
}

/* Jaringan kampus (eduroam dan sejenisnya) memisahkan perangkat satu sama
 * lain, jadi iPad tidak bisa menyambung ke laptop walau di Wi-Fi yang sama. */
static gboolean isolated_network(void)
{
    if (hotspot_active || !hotspot_shared || !*hotspot_shared)
        return FALSE;
    char *low = g_ascii_strdown(hotspot_shared, -1);
    gboolean campus = strstr(low, "eduroam") != NULL;
    g_free(low);
    return campus;
}

static char *info_text(void)
{
    gboolean isolated = isolated_network() && g_strcmp0(caps_hotspot, "no") != 0;
    const char *tip = "\n\nJaringan ini (%s) memisahkan perangkat, jadi iPad tidak bisa menyambung ke laptop. "
                      "Nyalakan Hotspot, lalu sambungkan iPad ke Wi-Fi hotspot itu.";
    if (g_strcmp0(child_kind, "receive") == 0) {
        GString *s = g_string_new(NULL);
        if (hotspot_active && hotspot_ssid && *hotspot_ssid)
            g_string_append_printf(s, "Sambungkan iPad ke Wi-Fi “%s” (QR di bawah), lalu ", hotspot_ssid);
        else
            g_string_append(s, "Di iPad: ");
        g_string_append_printf(s, "buka Pusat Kontrol, lalu Pencerminan Layar (Screen Mirroring), lalu pilih “%s”",
                               RECEIVER_NAME);
        if (isolated)
            g_string_append_printf(s, tip, hotspot_shared);
        return g_string_free(s, FALSE);
    }
    if (!urls || !urls[0])
        return NULL;
    GString *s = g_string_new("Pindai dengan kamera iPad atau buka di Safari:\n");
    g_string_append(s, urls[0]);
    for (int i = 1; urls[i] && i < 3; i++)
        if (*urls[i])
            g_string_append_printf(s, "\natau %s", urls[i]);
    if (isolated)
        g_string_append_printf(s, tip, hotspot_shared);
    return g_string_free(s, FALSE);
}

static char *hotspot_text(void)
{
    return g_strdup_printf("Wi-Fi “%s”\nSandi: %s", hotspot_ssid ? hotspot_ssid : "",
                           hotspot_password ? hotspot_password : "");
}

static int text_height(HDC dc, HFONT font, const char *text, int width)
{
    wchar_t *w = W(text);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = { 0, 0, width, 0 };
    DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_CENTER);
    SelectObject(dc, old);
    g_free(w);
    return r.bottom;
}

static void shape(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = border == CLR_INVALID ? GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, SS, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left * SS, r.top * SS, r.right * SS, r.bottom * SS, radius * 2 * SS, radius * 2 * SS);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (border != CLR_INVALID)
        DeleteObject(p);
}

static void draw_text(HDC dc, const char *text, RECT r, HFONT font, COLORREF color, UINT flags)
{
    wchar_t *w = W(text);
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, w, -1, &r, flags | DT_NOPREFIX);
    g_free(w);
}

static void draw_glyph(HDC dc, const wchar_t *glyph, RECT r, HFONT font, COLORREF color)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint_qr(HDC dc, RECT r, const char *rows)
{
    char **lines = g_strsplit(rows, ",", -1);
    int n = (int) g_strv_length(lines);
    HBRUSH black = GetStockObject(BLACK_BRUSH);
    if (n > 0) {
        int size = r.right - r.left;
        int m = MAX(1, size / (n + 4));
        int off = (size - m * n) / 2;
        for (int y = 0; y < n; y++)
            for (int x = 0; lines[y][x]; x++)
                if (lines[y][x] == '1') {
                    RECT c = { r.left + off + x * m, r.top + off + y * m, r.left + off + (x + 1) * m,
                               r.top + off + (y + 1) * m };
                    FillRect(dc, &c, black);
                }
    }
    g_strfreev(lines);
}


/* ---------- isi panel ---------- */

static Row *add_row(int kind, UINT cmd, const char *label, const char *caption, gboolean checked, gboolean enabled)
{
    if (nrows >= (int) G_N_ELEMENTS(rows))
        return NULL;
    Row *r = &rows[nrows++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->cmd = cmd;
    r->label = g_strdup(label);
    r->caption = g_strdup(caption);
    r->checked = checked;
    r->enabled = enabled;
    r->selected = -1;
    return r;
}

static void row_opt(Row *r, const char *label, UINT cmd, gboolean selected)
{
    if (!r || r->nopts >= MAX_OPTS)
        return;
    r->opts[r->nopts] = g_strdup(label);
    r->opt_cmd[r->nopts] = cmd;
    if (selected)
        r->selected = r->nopts;
    r->nopts++;
}

static void clear_rows(void)
{
    for (int i = 0; i < nrows; i++) {
        g_free(rows[i].label);
        g_free(rows[i].caption);
        for (int k = 0; k < rows[i].nopts; k++)
            g_free(rows[i].opts[k]);
    }
    nrows = 0;
}

/* "Ringan (1280×800)" -> "Ringan" untuk tombol segmen. */
static char *short_label(const char *label)
{
    const char *p = strstr(label, " (");
    return p ? g_strndup(label, p - label) : g_strdup(label);
}

static void build_rows(void)
{
    clear_rows();
    gboolean share = share_mode();
    gboolean on = running();
    const char *state = starting ? "Menyiapkan…"
                      : on ? (status_text ? status_text : service_share ? "Aktif · layanan" : "Aktif")
                      : "Mati";
    add_row(ROW_TITLE, ID_TOGGLE, "Tsunagu-Pad", state, on, !starting);

    Row *dir = add_row(ROW_SEGMENT, 0, NULL, NULL, FALSE, !starting);
    row_opt(dir, "Windows ke iPad", ID_MODE_SHARE, share);
    row_opt(dir, "iPad ke Windows", ID_MODE_RECEIVE, !share);

    /* Cara menyambung: QR dan alamat, atau petunjuk AirPlay. */
    gboolean show_qr = g_strcmp0(child_kind, "share") == 0 && qr_rows && *qr_rows;
    if (show_qr) {
        Row *q = add_row(ROW_QR, 0, NULL, NULL, FALSE, TRUE);
        if (q)
            q->qr = qr_rows;
    }
    char *info = info_text();
    if (info)
        add_row(ROW_INFO, 0, info, NULL, FALSE, TRUE);
    else if (!on && !starting)
        add_row(ROW_INFO, 0, share ? "Nyalakan sakelar di atas, lalu pindai QR-nya dengan kamera iPad."
                                   : "Nyalakan sakelar di atas untuk menerima layar iPad lewat AirPlay.",
                NULL, FALSE, TRUE);
    g_free(info);
    if (show_qr) {
        Row *b = add_row(ROW_BUTTONS, 0, NULL, NULL, FALSE, TRUE);
        row_opt(b, copied ? "Tersalin" : "Salin alamat", ID_COPY, FALSE);
        row_opt(b, "Kode akses baru", ID_RESET_TOKEN, FALSE);
    }

    add_row(ROW_HEADER, 0, "Layar", NULL, FALSE, TRUE);
    if (share) {
        gboolean second = config.second && caps_second;
        if (caps_second)
            add_row(ROW_TOGGLE, ID_SECOND, "Jadikan iPad layar kedua", "Layar tambahan di samping layar yang ada",
                    second, TRUE);
        if (second) {
            /* Keterangan: resolusi pilihan yang sedang dipakai, mis. 1280×800. */
            char *cap = NULL;
            for (guint i = 0; i < G_N_ELEMENTS(second_sizes); i++) {
                const char *open = strchr(second_sizes[i].label, '(');
                if (g_strcmp0(config.second_size, second_sizes[i].id) == 0 && open)
                    cap = g_strndup(open + 1, strcspn(open + 1, ")"));
            }
            Row *z = add_row(ROW_SEGMENT, 0, "Ukuran", cap, FALSE, TRUE);
            g_free(cap);
            for (guint i = 0; i < G_N_ELEMENTS(second_sizes); i++) {
                char *l = short_label(second_sizes[i].label);
                row_opt(z, l, ID_SIZE_BASE + i, g_strcmp0(config.second_size, second_sizes[i].id) == 0);
                g_free(l);
            }
            Row *k = add_row(ROW_SEGMENT, 0, "Skala", NULL, FALSE, TRUE);
            for (guint i = 0; i < G_N_ELEMENTS(second_scales); i++)
                row_opt(k, g_str_equal(second_scales[i].id, "ikut") ? "Ikut" : second_scales[i].label,
                        ID_SCALE_BASE + i, g_strcmp0(config.second_scale, second_scales[i].id) == 0);
        } else if (monitors && monitors->len > 1) {
            const Monitor *sel = selected_monitor();
            for (guint i = 0; i < monitors->len && i < 8; i++) {
                const Monitor *x = &g_array_index(monitors, Monitor, i);
                char *cap = g_strdup_printf("%d×%d%s", x->width, x->height, x->primary ? " · utama" : "");
                add_row(ROW_RADIO, ID_MONITOR_BASE + i, x->name, cap, sel == x, TRUE);
                g_free(cap);
            }
        }
        const char *qid = service_share ? "monitor" : config.quality;
        const char *qcap = NULL;
        for (guint i = 0; i < G_N_ELEMENTS(qualities); i++)
            if (g_strcmp0(qid, qualities[i].id) == 0)
                qcap = qualities[i].caption;
        Row *q = add_row(ROW_SEGMENT, 0, "Kualitas", qcap, FALSE, TRUE);
        for (guint i = 0; i < G_N_ELEMENTS(qualities); i++)
            row_opt(q, qualities[i].label, ID_QUALITY_BASE + i, g_strcmp0(qid, qualities[i].id) == 0);
    } else {
        add_row(ROW_TOGGLE, ID_FULLSCREEN, "Layar penuh", "Layar iPad memenuhi monitor", config.fullscreen, TRUE);
    }

    if (g_strcmp0(caps_hotspot, "no") != 0 || hotspot_active) {
        add_row(ROW_HEADER, 0, "Hotspot", NULL, FALSE, TRUE);
        add_row(ROW_TOGGLE, ID_HOTSPOT, "Hotspot langsung",
                hotspot_busy ? "Sedang diproses…"
                : hotspot_active ? "iPad tersambung langsung ke laptop ini"
                : "Untuk jaringan kampus seperti eduroam",
                hotspot_active, !hotspot_busy);
        if (hotspot_active && hotspot_qr && *hotspot_qr && !hotspot_editing) {
            Row *q = add_row(ROW_QR, 1, NULL, NULL, FALSE, TRUE);
            if (q)
                q->qr = hotspot_qr;
            char *t = hotspot_text();
            add_row(ROW_INFO, 0, t, NULL, FALSE, TRUE);
            g_free(t);
        }
        if (hotspot_editing) {
            add_row(ROW_FIELD, 1, "Nama Wi-Fi", NULL, FALSE, TRUE);
            add_row(ROW_FIELD, 2, "Kata sandi (8–63 karakter)", NULL, FALSE, TRUE);
            if (hotspot_note)
                add_row(ROW_NOTE, 0, hotspot_note, NULL, FALSE, TRUE);
            Row *b = add_row(ROW_BUTTONS, 0, NULL, NULL, FALSE, TRUE);
            row_opt(b, "Batal", ID_HOTSPOT_CANCEL, FALSE);
            row_opt(b, "Simpan", ID_HOTSPOT_SAVE, TRUE);
        } else {
            add_row(ROW_ACTION, ID_HOTSPOT_CONFIG, "Ubah nama dan kata sandi", NULL, FALSE, TRUE);
        }
    }

    add_row(ROW_HEADER, 0, "Lainnya", NULL, FALSE, TRUE);
    if (caps_service)
        add_row(ROW_TOGGLE, ID_BOOT, "iPad aktif di sesi berikutnya", "Termasuk layar login Windows",
                service_enabled, TRUE);
    add_row(ROW_TOGGLE, ID_AUTOSTART, "Mulai saat masuk Windows", NULL, autostart_enabled(), TRUE);
    add_row(ROW_NOTE, 0, "Ctrl+Alt+I membuka atau menutup panel ini.", NULL, FALSE, TRUE);
    add_row(ROW_ACTION, ID_EXIT, "Keluar dari Tsunagu-Pad", "Panel dan ikon di samping jam ditutup", FALSE, TRUE);
}

static RECT title_switch(const Row *r)
{
    int cy = r->box.top + S(32);
    RECT sw = { r->box.right - S(14) - S(SWITCH_W), cy - S(SWITCH_H) / 2, r->box.right - S(14), cy + S(SWITCH_H) / 2 };
    return sw;
}

static RECT field_box(const Row *r)
{
    RECT f = { r->box.left + S(14), r->box.top + S(22), r->box.right - S(14), r->box.top + S(22) + S(36) };
    return f;
}

static void layout_rows(HDC dc)
{
    build_rows();
    int x0 = S(12), x1 = S(PANEL_W) - S(12), y = S(6);
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        int h;
        switch (r->kind) {
        case ROW_TITLE:
            h = S(64);
            break;
        case ROW_HEADER:
            y += S(8);
            h = S(26);
            break;
        case ROW_SEGMENT:
            if (r->label)
                y += S(8);
            h = (r->label ? S(22) : 0) + S(34) + (r->caption && *r->caption ? S(22) : 0) + S(6);
            break;
        case ROW_NOTE:
            h = text_height(dc, font_small, r->label, x1 - x0 - S(28)) + S(10);
            break;
        case ROW_INFO:
            h = text_height(dc, font_small, r->label, x1 - x0 - S(20)) + S(12);
            break;
        case ROW_QR:
            h = S(r->cmd == 1 ? hqr_dip : qr_dip) + S(22);
            break;
        case ROW_BUTTONS:
            h = S(48);
            break;
        case ROW_FIELD:
            h = S(62);
            break;
        default:
            h = r->caption && *r->caption ? S(52) : S(40);
        }
        SetRect(&r->box, x0, y, x1, y + h);
        SetRectEmpty(&r->hit);
        switch (r->kind) {
        case ROW_TITLE: {
            RECT sw = title_switch(r);
            InflateRect(&sw, S(6), S(10));
            r->hit = sw;
            break;
        }
        case ROW_SEGMENT: {
            int top = y + (r->label ? S(22) : 0);
            int gx0 = x0 + S(8), gx1 = x1 - S(8), gap = S(4);
            int ow = r->nopts ? (gx1 - gx0 - S(6) - gap * (r->nopts - 1)) / r->nopts : 0;
            for (int k = 0; k < r->nopts; k++) {
                int ox = gx0 + S(3) + k * (ow + gap);
                SetRect(&r->opt_box[k], ox, top + S(3), ox + ow, top + S(31));
            }
            break;
        }
        case ROW_BUTTONS: {
            int gx0 = x0 + S(8), gx1 = x1 - S(8), gap = S(8);
            int bw = r->nopts ? (gx1 - gx0 - gap * (r->nopts - 1)) / r->nopts : 0;
            for (int k = 0; k < r->nopts; k++)
                SetRect(&r->opt_box[k], gx0 + k * (bw + gap), y + S(8), gx0 + k * (bw + gap) + bw, y + S(40));
            break;
        }
        case ROW_QR: {
            int size = S(r->cmd == 1 ? hqr_dip : qr_dip), cx = (x0 + x1) / 2;
            SetRect(&r->opt_box[0], cx - size / 2, y + S(10), cx + size / 2, y + S(10) + size);
            break;
        }
        case ROW_RADIO:
        case ROW_TOGGLE:
        case ROW_ACTION:
            r->hit = r->box;
            break;
        }
        y += h + S(2);
    }
    content_height = y + S(8);
}

static void clamp_scroll(void)
{
    int max = MAX(0, content_height - panel_height);
    scroll_y = CLAMP(scroll_y, 0, max);
}

static void paint_switch(HDC sdc, Palette c, RECT sw, gboolean on, gboolean enabled)
{
    int cy = (sw.top + sw.bottom) / 2;
    if (on) {
        shape(sdc, sw, S(SWITCH_H) / 2, enabled ? c.accent : c.muted, CLR_INVALID);
        RECT knob = { sw.right - S(16), cy - S(6), sw.right - S(4), cy + S(6) };
        shape(sdc, knob, S(6), c.on_fg, CLR_INVALID);
    } else {
        shape(sdc, sw, S(SWITCH_H) / 2, c.bg, c.muted);
        RECT knob = { sw.left + S(5), cy - S(5), sw.left + S(15), cy + S(5) };
        shape(sdc, knob, S(5), c.muted, CLR_INVALID);
    }
}

static void paint_rows_shapes(HDC sdc, Palette c)
{
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        gboolean hot = hover_row == i && r->enabled;
        int cy = (r->box.top + r->box.bottom) / 2;
        switch (r->kind) {
        case ROW_TITLE:
            paint_switch(sdc, c, title_switch(r), r->checked, r->enabled);
            break;
        case ROW_SEGMENT: {
            if (!r->nopts)
                break;
            RECT group = { r->opt_box[0].left - S(3), r->opt_box[0].top - S(3), r->opt_box[r->nopts - 1].right + S(3),
                           r->opt_box[0].bottom + S(3) };
            shape(sdc, group, S(6), c.tile, c.border);
            for (int k = 0; k < r->nopts; k++) {
                if (k == r->selected)
                    shape(sdc, r->opt_box[k], S(4), r->enabled ? c.accent : c.muted, CLR_INVALID);
                else if (hot && hover_opt == k)
                    shape(sdc, r->opt_box[k], S(4), c.tile_hover, CLR_INVALID);
            }
            break;
        }
        case ROW_RADIO:
            if (r->checked || hot)
                shape(sdc, r->box, S(6), hot ? c.tile_hover : c.tile, CLR_INVALID);
            if (r->checked) {
                /* Penanda pilihan seperti daftar di Windows 11: garis aksen di kiri. */
                RECT pill = { r->box.left + S(3), cy - S(8), r->box.left + S(6), cy + S(8) };
                shape(sdc, pill, S(2), r->enabled ? c.accent : c.muted, CLR_INVALID);
            }
            break;
        case ROW_ACTION:
        case ROW_TOGGLE:
            if (hot)
                shape(sdc, r->box, S(6), c.tile_hover, CLR_INVALID);
            if (r->kind == ROW_TOGGLE) {
                RECT sw = { r->box.right - S(14) - S(SWITCH_W), cy - S(SWITCH_H) / 2, r->box.right - S(14),
                            cy + S(SWITCH_H) / 2 };
                paint_switch(sdc, c, sw, r->checked, r->enabled);
            }
            break;
        case ROW_QR: {
            RECT q = r->opt_box[0];
            InflateRect(&q, S(6), S(6));
            shape(sdc, q, S(10), RGB(255, 255, 255), CLR_INVALID);
            break;
        }
        case ROW_BUTTONS:
            for (int k = 0; k < r->nopts; k++) {
                gboolean h = hot && hover_opt == k;
                if (k == r->selected)
                    shape(sdc, r->opt_box[k], S(4), h ? c.accent_hover : c.accent, CLR_INVALID);
                else
                    shape(sdc, r->opt_box[k], S(4), h ? c.tile_hover : c.tile, c.border);
            }
            break;
        case ROW_FIELD:
            shape(sdc, field_box(r), S(4), c.tile, c.border);
            break;
        }
    }
}

static void paint_rows_text(HDC dc, Palette c)
{
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        COLORREF fg = r->enabled ? c.fg : c.muted;
        int right = r->box.right - S(14) - (r->kind == ROW_TOGGLE ? S(SWITCH_W) + S(12) : 0);
        RECT t = { r->box.left + S(14), r->box.top, right, r->box.bottom };
        switch (r->kind) {
        case ROW_TITLE: {
            RECT sw = title_switch(r);
            RECT icon = { r->box.left + S(8), r->box.top + S(14), r->box.left + S(40), r->box.top + S(50) };
            draw_glyph(dc, L"", icon, font_icon, c.fg);
            RECT a = { r->box.left + S(46), r->box.top + S(12), sw.left - S(10), r->box.top + S(34) };
            RECT b = { r->box.left + S(46), r->box.top + S(34), sw.left - S(10), r->box.top + S(54) };
            draw_text(dc, r->label, a, font_bold, c.fg, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            draw_text(dc, r->caption, b, font_small, c.muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            break;
        }
        case ROW_HEADER:
            draw_text(dc, r->label, t, font_small, c.muted, DT_SINGLELINE | DT_BOTTOM | DT_LEFT | DT_END_ELLIPSIS);
            break;
        case ROW_SEGMENT: {
            if (r->label) {
                RECT l = { t.left, r->box.top, t.right, r->box.top + S(20) };
                draw_text(dc, r->label, l, font_small, c.muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            }
            for (int k = 0; k < r->nopts; k++)
                draw_text(dc, r->opts[k], r->opt_box[k], font_small,
                          k == r->selected ? c.on_fg : fg, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
            if (r->caption && *r->caption && r->nopts) {
                RECT cap = { t.left, r->opt_box[0].bottom + S(5), t.right, r->opt_box[0].bottom + S(25) };
                draw_text(dc, r->caption, cap, font_small, c.muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
            break;
        }
        case ROW_NOTE:
            t.top += S(4);
            draw_text(dc, r->label, t, font_small, c.muted, DT_WORDBREAK | DT_LEFT);
            break;
        case ROW_INFO: {
            RECT ir = { r->box.left + S(10), r->box.top + S(4), r->box.right - S(10), r->box.bottom };
            draw_text(dc, r->label, ir, font_small, c.fg, DT_WORDBREAK | DT_CENTER);
            break;
        }
        case ROW_QR:
            paint_qr(dc, r->opt_box[0], r->qr);
            break;
        case ROW_BUTTONS:
            for (int k = 0; k < r->nopts; k++)
                draw_text(dc, r->opts[k], r->opt_box[k], font_normal, k == r->selected ? c.on_fg : c.fg,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
            break;
        case ROW_FIELD: {
            RECT l = { t.left, r->box.top, t.right, r->box.top + S(20) };
            draw_text(dc, r->label, l, font_small, c.muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            break;
        }
        default:
            if (r->caption && *r->caption) {
                RECT a = { t.left, r->box.top + S(7), t.right, r->box.top + S(28) };
                RECT b = { t.left, r->box.top + S(28), t.right, r->box.bottom - S(5) };
                draw_text(dc, r->label, a, font_normal, fg, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
                draw_text(dc, r->caption, b, font_small, c.muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            } else {
                draw_text(dc, r->label, t, font_normal, fg, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
        }
    }
}

static void paint_panel(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    int w = cr.right, ht = cr.bottom;
    Palette c = palette();
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, ht);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    layout_rows(dc);
    clamp_scroll();

    /* Lapisan bentuk: digambar 3x lebih besar lalu diperkecil dengan HALFTONE. */
    HDC sdc = CreateCompatibleDC(wdc);
    HBITMAP sbmp = CreateCompatibleBitmap(wdc, w * SS, ht * SS);
    HGDIOBJ old_sbmp = SelectObject(sdc, sbmp);
    RECT all = { 0, 0, w * SS, ht * SS };
    HBRUSH bg = CreateSolidBrush(c.bg);
    FillRect(sdc, &all, bg);
    DeleteObject(bg);
    SetViewportOrgEx(sdc, 0, -scroll_y * SS, NULL);
    paint_rows_shapes(sdc, c);
    SetViewportOrgEx(sdc, 0, 0, NULL);
    if (content_height > ht) {
        /* Penanda gulir tipis di kanan. */
        int track = MAX(S(24), ht * ht / content_height);
        int pos = (ht - track) * scroll_y / MAX(1, content_height - ht);
        RECT bar = { w - S(6), pos + S(4), w - S(3), pos + track - S(4) };
        shape(sdc, bar, S(2), c.muted, CLR_INVALID);
    }
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, 0, 0, w, ht, sdc, 0, 0, w * SS, ht * SS, SRCCOPY);
    SelectObject(sdc, old_sbmp);
    DeleteObject(sbmp);
    DeleteDC(sdc);

    SetBkMode(dc, TRANSPARENT);
    SetViewportOrgEx(dc, 0, -scroll_y, NULL);
    paint_rows_text(dc, c);
    SetViewportOrgEx(dc, 0, 0, NULL);

    BitBlt(wdc, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

/* ---------- isian hotspot ----------
 * Kotak EDIT biasa di dalam panel, diwarnai sesuai tema lewat
 * WM_CTLCOLOREDIT, dan ikut bergeser saat panel digulir. */

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            PostMessageW(panel_hwnd, WM_COMMAND, ID_HOTSPOT_SAVE, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(panel_hwnd, WM_COMMAND, ID_HOTSPOT_CANCEL, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            SetFocus(h == edit_ssid ? edit_password : edit_ssid);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\t' || wp == 27))
        return 0;   /* tanpa bunyi "ding" */
    if (msg == WM_MOUSEWHEEL)
        return SendMessageW(panel_hwnd, msg, wp, lp);
    return CallWindowProcW(edit_base_proc, h, msg, wp, lp);
}

static HWND make_field(const char *text)
{
    wchar_t *w = W(text ? text : "");
    HWND e = CreateWindowExW(0, L"EDIT", w, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                             panel_hwnd, NULL, instance, NULL);
    g_free(w);
    SendMessageW(e, WM_SETFONT, (WPARAM) font_normal, TRUE);
    SendMessageW(e, EM_SETLIMITTEXT, 63, 0);
    edit_base_proc = (WNDPROC) SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR) edit_proc);
    return e;
}

static void place_fields(void)
{
    if (!hotspot_editing) {
        if (edit_ssid)
            DestroyWindow(edit_ssid);
        if (edit_password)
            DestroyWindow(edit_password);
        edit_ssid = edit_password = NULL;
        return;
    }
    if (!edit_ssid) {
        edit_ssid = make_field(hotspot_ssid);
        edit_password = make_field(hotspot_password);
    }
    for (int i = 0; i < nrows; i++) {
        if (rows[i].kind != ROW_FIELD)
            continue;
        HWND e = rows[i].cmd == 1 ? edit_ssid : edit_password;
        RECT f = field_box(&rows[i]);
        OffsetRect(&f, 0, -scroll_y);
        int eh = S(20), top = (f.top + f.bottom - eh) / 2;
        if (f.bottom < 0 || f.top > panel_height) {
            ShowWindow(e, SW_HIDE);
            continue;
        }
        MoveWindow(e, f.left + S(10), top, f.right - f.left - S(20), eh, TRUE);
        ShowWindow(e, SW_SHOWNA);
    }
}

/* Menggulir supaya baris pertama dengan jenis itu terlihat utuh. */
static void scroll_to_kind(int kind, gboolean to_end)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].kind == kind || (to_end && i == nrows - 1)) {
            if (to_end)
                scroll_y = content_height;
            else if (rows[i].box.bottom + S(70) > scroll_y + panel_height)
                scroll_y = rows[i].box.bottom + S(70) - panel_height;
            break;
        }
    clamp_scroll();
}

static void hotspot_edit(gboolean on)
{
    hotspot_editing = on;
    set_str(&hotspot_note, NULL);
    position_panel(FALSE);
    if (on)
        scroll_to_kind(ROW_FIELD, FALSE);
    place_fields();
    InvalidateRect(panel_hwnd, NULL, FALSE);
    if (on && edit_ssid)
        SetFocus(edit_ssid);
    else if (panel_hwnd)
        SetFocus(panel_hwnd);
}

static void hotspot_save(void)
{
    wchar_t ws[128] = L"", wp[128] = L"";
    if (edit_ssid)
        GetWindowTextW(edit_ssid, ws, G_N_ELEMENTS(ws));
    if (edit_password)
        GetWindowTextW(edit_password, wp, G_N_ELEMENTS(wp));
    char *ssid = U(ws), *password = U(wp);
    g_strstrip(ssid);
    gboolean ssid_ok = strlen(ssid) >= 1 && strlen(ssid) <= 32;
    for (const char *p = ssid; ssid_ok && *p; p++)
        ssid_ok = (guchar) *p >= 0x20 && *p != 0x7f;
    gboolean pass_ok = strlen(password) >= 8 && strlen(password) <= 63;
    for (const char *p = password; pass_ok && *p; p++)
        pass_ok = *p >= 0x20 && *p <= 0x7e;
    gboolean saved = FALSE;
    if (!ssid_ok) {
        set_str(&hotspot_note, "Nama Wi-Fi harus 1–32 byte.");
    } else if (!pass_ok) {
        set_str(&hotspot_note, "Kata sandi harus 8–63 karakter: huruf, angka, simbol, atau spasi "
                               "(tanpa emoji atau huruf beraksen).");
    } else {
        char *input = g_strdup_printf("%s\n", password);
        task_run(TASK_HOTSPOT_CONFIG, ssid, NULL, input);
        memset(input, 0, strlen(input));
        g_free(input);
        saved = TRUE;
    }
    SecureZeroMemory(wp, sizeof wp);
    memset(password, 0, strlen(password));
    g_free(password);
    g_free(ssid);
    if (saved) {
        hotspot_edit(FALSE);
    } else {
        position_panel(FALSE);
        place_fields();
    }
}

/* ---------- letak dan masukan ---------- */

static void position_panel(gboolean raise)
{
    if (!panel_hwnd)
        return;
    /* Di atas area notifikasi pada monitor tempat taskbar berada. */
    APPBARDATA abd = { .cbSize = sizeof abd };
    HMONITOR mon = SHAppBarMessage(ABM_GETTASKBARPOS, &abd)
        ? MonitorFromRect(&abd.rc, MONITOR_DEFAULTTOPRIMARY) : MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int max_h = (wa.bottom - wa.top) - S(24);
    /* Layar pendek: QR diperkecil dulu; sisanya bisa digulir. */
    HDC dc = GetDC(panel_hwnd);
    qr_dip = QR_MAX;
    hqr_dip = 150;
    layout_rows(dc);
    while (content_height > max_h && qr_dip > 150) {
        qr_dip -= 12;
        hqr_dip = MAX(110, hqr_dip - 10);
        layout_rows(dc);
    }
    ReleaseDC(panel_hwnd, dc);
    panel_height = MIN(content_height, max_h);
    clamp_scroll();
    int width = S(PANEL_W), x = wa.right - width - S(12), y = MAX(wa.top + S(12), wa.bottom - panel_height - S(12));
    RECT now;
    GetWindowRect(panel_hwnd, &now);
    if (raise)
        SetWindowPos(panel_hwnd, HWND_TOPMOST, x, y, width, panel_height, SWP_NOACTIVATE);
    else if (now.left != x || now.top != y || now.right - now.left != width || now.bottom - now.top != panel_height)
        SetWindowPos(panel_hwnd, NULL, x, y, width, panel_height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    place_fields();
    InvalidateRect(panel_hwnd, NULL, FALSE);
}

static void panel_click(POINT pt)
{
    pt.y += scroll_y;
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        if (!r->enabled)
            continue;
        UINT cmd = 0;
        if (r->kind == ROW_SEGMENT || r->kind == ROW_BUTTONS) {
            for (int k = 0; k < r->nopts && !cmd; k++)
                if (PtInRect(&r->opt_box[k], pt))
                    cmd = r->kind == ROW_SEGMENT && k == r->selected ? 0 : r->opt_cmd[k];
            if (!PtInRect(&r->box, pt))
                continue;
        } else if (!IsRectEmpty(&r->hit) && PtInRect(&r->hit, pt)) {
            cmd = r->cmd;
        } else {
            continue;
        }
        if (cmd == ID_HOTSPOT_SAVE)
            hotspot_save();
        else if (cmd == ID_HOTSPOT_CANCEL)
            hotspot_edit(FALSE);
        else if (cmd == ID_HOTSPOT_CONFIG)
            hotspot_edit(TRUE);
        else if (cmd)
            on_command(cmd);
        return;
    }
}

static void panel_hover(POINT pt)
{
    pt.y += scroll_y;
    int row = -1, opt = -1;
    for (int i = 0; i < nrows && row < 0; i++) {
        Row *r = &rows[i];
        if (!r->enabled)
            continue;
        if (r->kind == ROW_SEGMENT || r->kind == ROW_BUTTONS) {
            for (int k = 0; k < r->nopts; k++)
                if (PtInRect(&r->opt_box[k], pt)) {
                    row = i;
                    opt = k;
                }
        } else if (!IsRectEmpty(&r->hit) && PtInRect(&r->hit, pt)) {
            row = i;
        }
    }
    if (row != hover_row || opt != hover_opt) {
        hover_row = row;
        hover_opt = opt;
        InvalidateRect(panel_hwnd, NULL, FALSE);
    }
    if (!mouse_tracking) {
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, panel_hwnd, 0 };
        mouse_tracking = TrackMouseEvent(&tme);
    }
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint_panel(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT pt = { (short) LOWORD(lp), (short) HIWORD(lp) };
        panel_hover(pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        mouse_tracking = FALSE;
        hover_row = hover_opt = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        int before = scroll_y;
        scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) * S(48) / WHEEL_DELTA;
        clamp_scroll();
        if (scroll_y != before) {
            place_fields();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short) LOWORD(lp), (short) HIWORD(lp) };
        panel_click(pt);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            ShowWindow(h, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (hotspot_editing)
                hotspot_edit(FALSE);
            else
                ShowWindow(h, SW_HIDE);
        } else if (wp == VK_DOWN || wp == VK_UP || wp == VK_NEXT || wp == VK_PRIOR) {
            int step = wp == VK_DOWN ? S(48) : wp == VK_UP ? -S(48) : wp == VK_NEXT ? panel_height : -panel_height;
            scroll_y += step;
            clamp_scroll();
            place_fields();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        Palette c = palette();
        if (field_brush_color != c.tile) {
            if (field_brush)
                DeleteObject(field_brush);
            field_brush = CreateSolidBrush(c.tile);
            field_brush_color = c.tile;
        }
        SetTextColor((HDC) wp, c.fg);
        SetBkColor((HDC) wp, c.tile);
        return (LRESULT) field_brush;
    }
    case WM_SETTINGCHANGE:
        InvalidateRect(h, NULL, FALSE);   /* tema gelap/terang berganti */
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_HOTSPOT_SAVE)
            hotspot_save();
        else if (LOWORD(wp) == ID_HOTSPOT_CANCEL)
            hotspot_edit(FALSE);
        else if (lp == 0)       /* bukan pemberitahuan dari kotak isian */
            on_command(LOWORD(wp));
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_panel(void)
{
    if (!panel_hwnd) {
        panel_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"TsunaguPadPanel", L"Tsunagu-Pad",
                                     WS_POPUP | WS_CLIPCHILDREN, 0, 0, S(PANEL_W), S(300), NULL, NULL, instance, NULL);
        /* Sudut membulat dan bingkai tipis seperti flyout Windows 11
         * (diabaikan dengan aman di Windows 10). */
        typedef HRESULT (WINAPI *SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        SetAttr set_attr = dwm ? (SetAttr) (void *) GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        if (set_attr) {
            DWORD round = 2;           /* DWMWCP_ROUND */
            set_attr(panel_hwnd, 33, &round, sizeof round);    /* DWMWA_WINDOW_CORNER_PREFERENCE */
            BOOL dark = dark_theme();
            set_attr(panel_hwnd, 20, &dark, sizeof dark);      /* DWMWA_USE_IMMERSIVE_DARK_MODE */
        }
    }
    hotspot_editing = FALSE;
    scroll_y = 0;
    position_panel(TRUE);
    ShowWindow(panel_hwnd, SW_SHOW);
    SetForegroundWindow(panel_hwnd);
}

/* Statistik datang tiap detik saat streaming. Panel hanya diberi ukuran
 * baru tanpa diangkat lagi ke paling atas (HWND_TOPMOST hanya saat dibuka),
 * dan tray hanya diperbarui bila isinya berubah. */
static void sync_ui(void)
{
    update_tray();
    if (panel_hwnd && IsWindowVisible(panel_hwnd))
        position_panel(FALSE);
}

/* Windows 11 menyembunyikan ikon baru di balik tanda ^. Sekali saja, ikon
 * ini ditampilkan langsung di samping jam (di sebelah Quick Settings);
 * setelah itu pilihan pengguna di Pengaturan, Taskbar, tidak diubah lagi. */
static gboolean promote_tray_icon(void)
{
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                      KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS)
        return FALSE;
    gboolean done = FALSE;
    wchar_t name[256];
    for (DWORD i = 0; !done; i++) {
        DWORD len = G_N_ELEMENTS(name);
        if (RegEnumKeyExW(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        wchar_t path[MAX_PATH * 2];
        DWORD size = sizeof path - sizeof(wchar_t);
        memset(path, 0, sizeof path);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS)
            continue;
        size_t n = wcslen(path), m = wcslen(L"\\tsunagupad-tray.exe");
        if (n < m || _wcsicmp(path + n - m, L"\\tsunagupad-tray.exe") != 0)
            continue;
        DWORD promoted = 0, psize = sizeof promoted;
        if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, NULL, &promoted, &psize) != ERROR_SUCCESS) {
            promoted = 1;
            RegSetKeyValueW(root, name, L"IsPromoted", REG_DWORD, &promoted, sizeof promoted);
        }
        done = TRUE;
    }
    RegCloseKey(root);
    return done;
}

/* ---------- jendela utama (tersembunyi) ---------- */

static void on_command(UINT id)
{
    if (id == ID_TOGGLE) {
        toggle();
    } else if (id == ID_SHOW_PANEL) {
        show_panel();
    } else if (id == ID_MODE_SHARE || id == ID_MODE_RECEIVE) {
        const char *mode = id == ID_MODE_SHARE ? "share" : "receive";
        if (!g_str_equal(config.mode, mode)) {
            set_str(&config.mode, mode);
            config_save();
            if (child)
                restart();
        }
    } else if (id == ID_SECOND) {
        config.second = !config.second;
        config_save();
        if (g_strcmp0(child_kind, "share") == 0)
            restart();
    } else if (id >= ID_SIZE_BASE && id < ID_SIZE_BASE + G_N_ELEMENTS(second_sizes)) {
        set_str(&config.second_size, second_sizes[id - ID_SIZE_BASE].id);
        config_save();
        if (g_strcmp0(child_kind, "share") == 0 && config.second)
            restart();
    } else if (id >= ID_SCALE_BASE && id < ID_SCALE_BASE + G_N_ELEMENTS(second_scales)) {
        set_str(&config.second_scale, second_scales[id - ID_SCALE_BASE].id);
        config_save();
        if (g_strcmp0(child_kind, "share") == 0 && config.second)
            restart();
    } else if (id >= ID_QUALITY_BASE && id < ID_QUALITY_BASE + G_N_ELEMENTS(qualities)) {
        set_str(&config.quality, qualities[id - ID_QUALITY_BASE].id);
        config_save();
        /* Panel mengambil alih sesi layanan: program baru menghentikan yang lama sendiri. */
        if (g_strcmp0(child_kind, "share") == 0 || service_share) {
            service_share = FALSE;
            restart();
        }
    } else if (id >= ID_MONITOR_BASE && monitors && id < ID_MONITOR_BASE + monitors->len) {
        /* Disimpan per nama perangkat, bukan urutan: urutan berubah saat kabel dicolok. */
        set_str(&config.monitor_name, g_array_index(monitors, Monitor, id - ID_MONITOR_BASE).device);
        config_save();
        if (g_strcmp0(child_kind, "share") == 0)
            restart();
    } else if (id == ID_FULLSCREEN) {
        config.fullscreen = !config.fullscreen;
        config_save();
        if (g_strcmp0(child_kind, "receive") == 0)
            restart();
    } else if (id == ID_COPY) {
        copy_address();
    } else if (id == ID_RESET_TOKEN) {
        reset_token();
    } else if (id == ID_HOTSPOT) {
        if (!hotspot_busy) {
            hotspot_busy = TRUE;
            task_run(TASK_HOTSPOT_SET, hotspot_active ? "off" : "on", NULL, NULL);
        }
    } else if (id == ID_HOTSPOT_CONFIG) {
        show_panel();
        hotspot_edit(TRUE);
    } else if (id == ID_SETTINGS) {
        show_panel();
        scroll_to_kind(ROW_HEADER, TRUE);
        place_fields();
        InvalidateRect(panel_hwnd, NULL, FALSE);
    } else if (id == ID_BOOT) {
        char *token = token_path();
        if (!service_enabled && !g_file_test(token, G_FILE_TEST_EXISTS))
            notify("Nyalakan Tsunagu-Pad sekali dulu supaya kode aksesnya dibuat.");
        else
            task_run(TASK_SERVICE_TOGGLE, service_enabled ? "disable" : "enable", token, NULL);
        g_free(token);
    } else if (id == ID_AUTOSTART) {
        autostart_set(!autostart_enabled());
    } else if (id == ID_EXIT) {
        DestroyWindow(main_hwnd);
    }
    sync_ui();
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == taskbar_created && taskbar_created) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        update_tray();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        /* NOTIFYICON_VERSION_4: satu klik kiri juga mengirim WM_LBUTTONUP, dan
         * klik kanan juga WM_RBUTTONUP; hanya kabar versi 4 yang dipakai supaya
         * panel dan menu tidak terbuka dua kali. */
        if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_CONTEXTMENU) {
            /* Klik kanan juga membuka panel: semua pilihan ada di sana, tanpa
             * menu putih bawaan Windows yang tidak mengikuti tema gelap. */
            if (panel_hwnd && IsWindowVisible(panel_hwnd))
                ShowWindow(panel_hwnd, SW_HIDE);
            else
                show_panel();
        }
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_CHILD_LINE: {
        char *line = (char *) lp;
        if ((guint) wp == child_id && child)
            on_child_line(line);
        g_free(line);
        return 0;
    }
    case WM_CHILD_EXIT:
        if ((guint) wp == child_id)
            on_child_exit((DWORD) lp);
        return 0;
    case WM_TASK_DONE:
        on_task_done((Task *) lp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_POLL) {
            refresh_service_share();
            static int ticks;
            ticks++;
            if (caps_service && ticks % 3 == 0)
                task_run(TASK_SERVICE_STATUS, NULL, NULL, NULL);
            /* Saat hotspot mati cukup tiap menit: untuk tahu jaringan yang
             * dipakai laptop (mis. eduroam) dan petunjuknya di panel. */
            if (!hotspot_busy && g_strcmp0(caps_hotspot, "no") != 0 && ticks % (hotspot_active ? 3 : 12) == 0)
                task_run(TASK_HOTSPOT_STATUS, NULL, NULL, NULL);
        } else if (wp == TIMER_RECEIVER_RESTART) {
            KillTimer(h, TIMER_RECEIVER_RESTART);
            if (!child && !share_mode())
                start();
        } else if (wp == TIMER_COPY) {
            KillTimer(h, TIMER_COPY);
            copied = FALSE;
            if (panel_hwnd)
                InvalidateRect(panel_hwnd, NULL, FALSE);
        } else if (wp == TIMER_DISPLAY) {
            KillTimer(h, TIMER_DISPLAY);
            task_run(TASK_CAPS, NULL, NULL, NULL);
            task_run(TASK_MONITORS, NULL, NULL, NULL);
        } else if (wp == TIMER_PROMOTE) {
            /* Explorer baru mencatat ikon beberapa detik setelah muncul. */
            static int tries;
            if (promote_tray_icon() || ++tries >= 12) {
                KillTimer(h, TIMER_PROMOTE);
                char *marker = g_build_filename(g_get_user_config_dir(), "tsunagupad", "tray-promoted", NULL);
                g_file_set_contents(marker, "", 0, NULL);
                g_free(marker);
            }
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_PANEL) {
            if (panel_hwnd && IsWindowVisible(panel_hwnd))
                ShowWindow(panel_hwnd, SW_HIDE);
            else
                show_panel();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        /* Monitor dicolok atau dicabut: daftar dan kemampuan dibaca ulang
         * sebentar lagi. Proses berbagi layar menyalakan dirinya ulang sendiri. */
        SetTimer(h, TIMER_DISPLAY, 1500, NULL);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp)
            child_stop();
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_PANEL);
        child_stop();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(14);
    font_normal = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(12);
    font_small = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(18);
    lf.lfWeight = FW_SEMIBOLD;
    font_bold = CreateFontIndirectW(&lf);
    font_icon = icon_font(S(16));
    font_icon_small = icon_font(S(12));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    instance = inst;
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    SetCtx set_ctx = (SetCtx) (void *) GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!set_ctx || !set_ctx((HANDLE) -4))
        SetProcessDPIAware();
    HDC sdc = GetDC(NULL);
    dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);

    gboolean autostarted = cmdline && wcsstr(cmdline, L"--autostart") != NULL;
    /* --toggle: shortcut Start menu "Nyalakan atau matikan" (bisa di-pin ke taskbar). */
    gboolean toggle_arg = cmdline && wcsstr(cmdline, L"--toggle") != NULL;

    /* Satu panel saja per pengguna; yang kedua hanya meneruskan perintahnya. */
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\TsunaguPadTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TsunaguPadTray", NULL);
        if (other && toggle_arg)
            PostMessageW(other, WM_COMMAND, ID_TOGGLE, 0);
        else if (other && !autostarted)
            PostMessageW(other, WM_COMMAND, ID_SHOW_PANEL, 0);
        return 0;
    }

    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(NULL, exe, G_N_ELEMENTS(exe));
    char *self = U(exe);
    bin_dir = g_path_get_dirname(self);
    daemon_exe = g_build_filename(bin_dir, "tsunagupad.exe", NULL);
    g_free(self);

    child_job = CreateJobObjectW(NULL, NULL);
    if (child_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
        memset(&limit, 0, sizeof limit);
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(child_job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    }

    config_load();
    config_save();
    make_fonts();
    int icon_size = GetSystemMetrics(SM_CXSMICON);
    icon_idle = make_icon(FALSE, icon_size);
    icon_active = make_icon(TRUE, icon_size);

    WNDCLASSEXW wc = { .cbSize = sizeof wc };
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TsunaguPadTray";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = panel_proc;
    wc.lpszClassName = L"TsunaguPadPanel";
    wc.style = CS_DROPSHADOW;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    RegisterClassExW(&wc);

    main_hwnd = CreateWindowExW(0, L"TsunaguPadTray", L"Tsunagu-Pad", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = main_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon_idle;
    wcscpy(nid.szTip, L"Tsunagu-Pad");
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_ADD, &nid);
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    /* Panel pertama kali dijalankan: daftarkan diri untuk mulai saat masuk Windows. */
    char *marker = g_build_filename(g_get_user_config_dir(), "tsunagupad", "autostart-asked", NULL);
    if (!g_file_test(marker, G_FILE_TEST_EXISTS)) {
        autostart_set(TRUE);
        g_file_set_contents(marker, "", 0, NULL);
    }
    g_free(marker);

    /* Sisa monitor virtual dari sesi yang berhenti mendadak dilepas dulu,
     * kecuali ada sesi berbagi layar yang masih berjalan (mis. `tsunagupad on`). */
    if (!port_owner(SHARE_PORT)) {
        const char *off[] = { "second-off", NULL };
        g_free(run_tool(off, NULL, NULL));
    }

    /* Pintasan keyboard untuk membuka panel dari mana saja. */
    RegisterHotKey(main_hwnd, HOTKEY_PANEL, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I');
    char *promoted = g_build_filename(g_get_user_config_dir(), "tsunagupad", "tray-promoted", NULL);
    if (!g_file_test(promoted, G_FILE_TEST_EXISTS)) {
        SetTimer(main_hwnd, TIMER_PROMOTE, 5000, NULL);
        notify("Tsunagu-Pad ada di samping jam. Klik ikonnya atau tekan Ctrl+Alt+I untuk membuka panel.");
    }
    g_free(promoted);

    task_run(TASK_CAPS, NULL, NULL, NULL);
    SetTimer(main_hwnd, TIMER_POLL, 5000, NULL);
    task_run(TASK_MONITORS, NULL, NULL, NULL);
    refresh_service_share();
    update_tray();
    if (toggle_arg)
        PostMessageW(main_hwnd, WM_COMMAND, ID_TOGGLE, 0);
    else if (!autostarted)
        show_panel();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (child_job)
        CloseHandle(child_job);
    CloseHandle(single);
    return 0;
}
