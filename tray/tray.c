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

#define SHARE_PORT 8765
#define RECEIVER_NAME "Tsunagu-Pad Windows"
#define QR_MAX 196

enum {
    ID_TOGGLE = 100, ID_MODE_SHARE, ID_MODE_RECEIVE, ID_SECOND, ID_FULLSCREEN, ID_SHOW_PANEL,
    ID_COPY, ID_RESET_TOKEN, ID_HOTSPOT, ID_HOTSPOT_CONFIG, ID_BOOT, ID_AUTOSTART, ID_EXIT,
    ID_SETTINGS, ID_MONITOR_PRIMARY = 300, ID_MONITOR_BASE = 301,
    ID_SIZE_BASE = 400, ID_SCALE_BASE = 420, ID_QUALITY_BASE = 440,
    ID_DLG_SAVE = 500, ID_DLG_CANCEL,
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
static HWND main_hwnd, panel_hwnd, dialog_hwnd;
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
               TASK_SERVICE_STATUS, TASK_SERVICE_TOGGLE } TaskKind;

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
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = running() ? icon_active : icon_idle;
    char *tip = g_strdup_printf("Tsunagu-Pad — %s", status_text ? status_text : "");
    wchar_t *w = W(tip);
    wcsncpy(nid.szTip, w, G_N_ELEMENTS(nid.szTip) - 1);
    nid.szTip[G_N_ELEMENTS(nid.szTip) - 1] = 0;
    g_free(w);
    g_free(tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void sync_ui(void);

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

static void load_monitors(void)
{
    if (!monitors)
        monitors = g_array_new(FALSE, TRUE, sizeof(Monitor));
    g_array_set_size(monitors, 0);
    const char *a[] = { "monitors", NULL };
    char *out = run_tool(a, NULL, NULL);
    JsonParser *p = NULL;
    JsonObject *o = last_json(out, &p);
    JsonArray *list = o && json_object_has_member(o, "monitors") ? json_object_get_array_member(o, "monitors") : NULL;
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
            g_array_append_val(monitors, mon);
    }
    if (p)
        g_object_unref(p);
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
        if (g_str_equal(ev, "ready")) {
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
            notify("Penerima AirPlay berhenti berulang kali. Pastikan tidak ada UxPlay lain yang berjalan, lalu nyalakan lagi.");
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
    hotspot_active = g_strcmp0(json_object_get_string_member_with_default(o, "state", ""), "active") == 0;
    set_str(&hotspot_ssid, json_object_get_string_member_with_default(o, "ssid", ""));
    set_str(&hotspot_password, json_object_get_string_member_with_default(o, "password", ""));
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

static void add_item(HMENU m, UINT id, const char *label, gboolean checked, gboolean enabled, gboolean radio)
{
    wchar_t *w = W(label);
    MENUITEMINFOW mi = { .cbSize = sizeof mi };
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
    mi.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_DISABLED);
    mi.wID = id;
    mi.dwTypeData = w;
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
    g_free(w);
}

static void add_submenu(HMENU m, HMENU sub, const char *label)
{
    wchar_t *w = W(label);
    AppendMenuW(m, MF_POPUP, (UINT_PTR) sub, w);
    g_free(w);
}

/* drop_down: menu turun dari titik (dari panel); selain itu naik dari kursor
 * di taskbar. Perintahnya dijalankan setelah menu tertutup, dan pemiliknya
 * tetap aktif, supaya panel tidak ikut tertutup. */
static void show_menu_at(HWND owner, POINT pt, gboolean drop_down)
{
    load_monitors();
    HMENU m = CreatePopupMenu();
    gboolean share = share_mode();
    gboolean on = running();

    add_item(m, ID_TOGGLE, on ? "Matikan Tsunagu-Pad" : "Nyalakan Tsunagu-Pad", FALSE, !starting, FALSE);
    add_item(m, ID_SHOW_PANEL, "Tampilkan panel…", FALSE, TRUE, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add_item(m, ID_MODE_SHARE, "Windows ke iPad", share, TRUE, TRUE);
    add_item(m, ID_MODE_RECEIVE, "iPad ke Windows (AirPlay)", !share, TRUE, TRUE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    if (share) {
        gboolean second = config.second && caps_second;
        if (caps_second)
            add_item(m, ID_SECOND, "Jadikan iPad layar kedua", second, TRUE, FALSE);
        if (second) {
            HMENU size = CreatePopupMenu();
            for (guint i = 0; i < G_N_ELEMENTS(second_sizes); i++)
                add_item(size, ID_SIZE_BASE + i, second_sizes[i].label,
                         g_strcmp0(config.second_size, second_sizes[i].id) == 0, TRUE, TRUE);
            add_submenu(m, size, "Ukuran layar kedua");
            HMENU scale = CreatePopupMenu();
            for (guint i = 0; i < G_N_ELEMENTS(second_scales); i++)
                add_item(scale, ID_SCALE_BASE + i, second_scales[i].label,
                         g_strcmp0(config.second_scale, second_scales[i].id) == 0, TRUE, TRUE);
            add_submenu(m, scale, "Skala layar kedua");
        } else {
            HMENU mon = CreatePopupMenu();
            const Monitor *sel = selected_monitor();
            for (guint i = 0; monitors && i < monitors->len && i < 32; i++) {
                const Monitor *x = &g_array_index(monitors, Monitor, i);
                char *label = g_strdup_printf("%s · %d×%d%s", x->name, x->width, x->height, x->primary ? " (utama)" : "");
                add_item(mon, ID_MONITOR_BASE + i, label, sel == x, TRUE, TRUE);
                g_free(label);
            }
            char *title = g_strdup_printf("Monitor: %s", sel ? sel->name : "Utama");
            add_submenu(m, mon, title);
            g_free(title);
        }
        HMENU q = CreatePopupMenu();
        const char *qid = service_share ? "monitor" : config.quality;
        for (guint i = 0; i < G_N_ELEMENTS(qualities); i++) {
            char *label = g_strdup_printf("%s — %s", qualities[i].label, qualities[i].caption);
            add_item(q, ID_QUALITY_BASE + i, label, g_strcmp0(qid, qualities[i].id) == 0, TRUE, TRUE);
            g_free(label);
        }
        char *qtitle = NULL;
        for (guint i = 0; i < G_N_ELEMENTS(qualities); i++)
            if (g_strcmp0(qid, qualities[i].id) == 0)
                qtitle = g_strdup_printf("Kualitas: %s", qualities[i].label);
        add_submenu(m, q, qtitle ? qtitle : "Kualitas");
        g_free(qtitle);
    } else {
        add_item(m, ID_FULLSCREEN, "Layar penuh", config.fullscreen, TRUE, FALSE);
    }

    if (share) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        add_item(m, ID_COPY, "Salin alamat", FALSE, urls && urls[0] && *urls[0], FALSE);
        add_item(m, ID_RESET_TOKEN, "Buat kode akses baru", FALSE, TRUE, FALSE);
    }

    if (g_strcmp0(caps_hotspot, "no") != 0 || hotspot_active) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        add_item(m, ID_HOTSPOT, hotspot_busy ? "Hotspot langsung (sedang diproses…)" : "Hotspot langsung",
                 hotspot_active, !hotspot_busy, FALSE);
        add_item(m, ID_HOTSPOT_CONFIG, "Atur nama dan kata sandi hotspot…", FALSE, TRUE, FALSE);
    }

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (caps_service)
        add_item(m, ID_BOOT, "iPad aktif di sesi berikutnya (juga layar login)", service_enabled, TRUE, FALSE);
    add_item(m, ID_AUTOSTART, "Mulai otomatis saat masuk Windows", autostart_enabled(), TRUE, FALSE);
    add_item(m, ID_EXIT, "Keluar", FALSE, TRUE, FALSE);

    if (GetForegroundWindow() != owner)
        SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | (drop_down ? TPM_TOPALIGN : TPM_BOTTOMALIGN),
                              pt.x, pt.y, 0, owner, NULL);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd)
        on_command(cmd);
}

static void show_menu(HWND owner, POINT pt)
{
    show_menu_at(owner, pt, FALSE);
}

/* ---------- dialog hotspot ---------- */

static HWND dlg_ssid, dlg_password, dlg_status;

static void dialog_save(void)
{
    wchar_t ws[128], wp[128];
    GetWindowTextW(dlg_ssid, ws, G_N_ELEMENTS(ws));
    GetWindowTextW(dlg_password, wp, G_N_ELEMENTS(wp));
    char *ssid = U(ws), *password = U(wp);
    g_strstrip(ssid);
    gboolean ssid_ok = strlen(ssid) >= 1 && strlen(ssid) <= 32;
    for (const char *p = ssid; ssid_ok && *p; p++)
        ssid_ok = (guchar) *p >= 0x20 && *p != 0x7f;
    gboolean pass_ok = strlen(password) >= 8 && strlen(password) <= 63;
    for (const char *p = password; pass_ok && *p; p++)
        pass_ok = *p >= 0x20 && *p <= 0x7e;
    if (!ssid_ok)
        SetWindowTextW(dlg_status, L"Nama Wi-Fi harus 1–32 byte.");
    else if (!pass_ok)
        SetWindowTextW(dlg_status, L"Kata sandi harus 8–63 karakter: huruf, angka, simbol, atau spasi "
                                   L"(tanpa emoji/huruf beraksen).");
    else {
        char *input = g_strdup_printf("%s\n", password);
        task_run(TASK_HOTSPOT_CONFIG, ssid, NULL, input);
        memset(input, 0, strlen(input));
        g_free(input);
        DestroyWindow(dialog_hwnd);
    }
    SecureZeroMemory(wp, sizeof wp);
    memset(password, 0, strlen(password));
    g_free(password);
    g_free(ssid);
}

static LRESULT CALLBACK dialog_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_SAVE || LOWORD(wp) == IDOK)
            dialog_save();
        else if (LOWORD(wp) == ID_DLG_CANCEL || LOWORD(wp) == IDCANCEL)
            DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        dialog_hwnd = NULL;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC) wp, TRANSPARENT);
        return (LRESULT) GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND child_ctl(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w,
                      int hgt, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hgt, parent,
                             (HMENU) (INT_PTR) id, instance, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM) font, TRUE);
    return c;
}

static void show_hotspot_dialog(void)
{
    if (dialog_hwnd) {
        SetForegroundWindow(dialog_hwnd);
        return;
    }
    int w = S(380), h = S(260);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    dialog_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"TsunaguPadDialog", L"Hotspot Tsunagu-Pad",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU, (wa.left + wa.right - w) / 2,
                                  (wa.top + wa.bottom - h) / 2, w, h, NULL, NULL, instance, NULL);
    int x = S(16), cw = w - S(48), y = S(14);
    child_ctl(dialog_hwnd, L"STATIC", L"Nama Wi-Fi", 0, x, y, cw, S(20), 0, font_normal);
    y += S(22);
    wchar_t *ssid = W(hotspot_ssid);
    dlg_ssid = child_ctl(dialog_hwnd, L"EDIT", ssid, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, x, y, cw, S(26), 0, font_normal);
    g_free(ssid);
    y += S(36);
    child_ctl(dialog_hwnd, L"STATIC", L"Kata sandi (8–63 karakter)", 0, x, y, cw, S(20), 0, font_normal);
    y += S(22);
    wchar_t *pass = W(hotspot_password);
    dlg_password = child_ctl(dialog_hwnd, L"EDIT", pass, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                             x, y, cw, S(26), 0, font_normal);
    g_free(pass);
    y += S(32);
    dlg_status = child_ctl(dialog_hwnd, L"STATIC", L"", 0, x, y, cw, S(36), 0, font_small);
    y += S(40);
    child_ctl(dialog_hwnd, L"BUTTON", L"Batal", WS_TABSTOP, w - S(32) - S(180), y, S(86), S(28), ID_DLG_CANCEL, font_normal);
    child_ctl(dialog_hwnd, L"BUTTON", L"Simpan", WS_TABSTOP | BS_DEFPUSHBUTTON, w - S(32) - S(86), y, S(86), S(28),
              ID_DLG_SAVE, font_normal);
    ShowWindow(dialog_hwnd, SW_SHOW);
    SetForegroundWindow(dialog_hwnd);
    SetFocus(dlg_ssid);
}

/* ---------- panel ----------
 * Windows tidak mengizinkan aplikasi lain menambah tombol ke Quick Settings,
 * jadi panel ini meniru tampilannya: kotak-kotak tombol dengan bagian ">"
 * untuk pilihan, sudut membulat, tema gelap/terang mengikuti Windows, dan
 * muncul di pojok kanan bawah tepat di atas ikon jam. */

enum { TILE_MAIN, TILE_RECEIVE, TILE_SECOND, TILE_HOTSPOT, TILE_BOOT, TILE_QUALITY };

typedef struct {
    int id;
    RECT box, chevron;
    gboolean split, on, enabled;
    const wchar_t *glyph;
    char *label;
} Tile;

static Tile tiles[8];
static int ntiles;
static RECT copy_rect, settings_rect, qr_rect, hqr_rect;
static int panel_height;
static int qr_dip = QR_MAX, hqr_dip = 150;     /* diperkecil bila layar pendek */
static int hover_tile = -1, hover_part;     /* 1 bagian utama, 2 bagian ">" */
static gboolean hover_copy, hover_settings, mouse_tracking;

#define PANEL_W 360
#define TILE_W  96
#define TILE_H  48
#define TILE_GAP 12
#define SS 3        /* bentuk digambar 3x lalu diperkecil supaya sudutnya halus */

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

static char *info_text(void)
{
    if (g_strcmp0(child_kind, "receive") == 0)
        return g_strdup_printf("Di iPad: buka Pusat Kontrol, lalu Pencerminan Layar (Screen Mirroring), "
                               "lalu pilih “%s”", RECEIVER_NAME);
    if (!urls || !urls[0])
        return NULL;
    GString *s = g_string_new("Pindai dengan kamera iPad atau buka di Safari:\n");
    g_string_append(s, urls[0]);
    for (int i = 1; urls[i] && i < 3; i++)
        if (*urls[i])
            g_string_append_printf(s, "\natau %s", urls[i]);
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

static const char *quality_label(void)
{
    const char *qid = service_share ? "monitor" : config.quality;
    for (guint i = 0; i < G_N_ELEMENTS(qualities); i++)
        if (g_strcmp0(qid, qualities[i].id) == 0)
            return qualities[i].label;
    return "Kualitas";
}

static void add_tile(int id, const wchar_t *glyph, const char *label, gboolean on, gboolean split, gboolean enabled)
{
    Tile *t = &tiles[ntiles++];
    g_free(t->label);
    memset(t, 0, sizeof *t);
    t->id = id;
    t->glyph = glyph;
    t->label = g_strdup(label);
    t->on = on;
    t->split = split;
    t->enabled = enabled;
}

static void build_layout(HDC dc)
{
    for (int i = 0; i < ntiles; i++)
        g_clear_pointer(&tiles[i].label, g_free);
    ntiles = 0;
    gboolean share = share_mode();
    char *main_label;
    if (starting)
        main_label = g_strdup("Menyiapkan…");
    else if (running())
        main_label = g_strdup(status_text ? status_text : service_share ? "Aktif · layanan" : "Aktif");
    else
        main_label = g_strdup("Tsunagu-Pad");
    add_tile(TILE_MAIN, L"", main_label, running(), TRUE, !starting);
    g_free(main_label);
    add_tile(TILE_RECEIVE, L"", "iPad ke Windows", !share, FALSE, TRUE);
    if (caps_second)
        add_tile(TILE_SECOND, L"", "Layar kedua", share && config.second, TRUE, share);
    if (g_strcmp0(caps_hotspot, "no") != 0 || hotspot_active)
        add_tile(TILE_HOTSPOT, L"", hotspot_busy ? "Hotspot…" : "Hotspot", hotspot_active, TRUE, !hotspot_busy);
    if (caps_service)
        add_tile(TILE_BOOT, L"", "Sesi berikutnya", service_enabled, FALSE, TRUE);
    char *q = g_strdup_printf("Kualitas: %s", quality_label());
    add_tile(TILE_QUALITY, L"", q, FALSE, FALSE, share);
    g_free(q);

    int left = (S(PANEL_W) - 3 * S(TILE_W) - 2 * S(TILE_GAP)) / 2;
    int y = S(76);
    for (int i = 0; i < ntiles; i++) {
        int col = i % 3, row = i / 3;
        int x = left + col * (S(TILE_W) + S(TILE_GAP));
        int ty = y + row * (S(TILE_H) + S(34));
        SetRect(&tiles[i].box, x, ty, x + S(TILE_W), ty + S(TILE_H));
        if (tiles[i].split)
            SetRect(&tiles[i].chevron, x + S(TILE_W) - S(34), ty, x + S(TILE_W), ty + S(TILE_H));
        else
            SetRectEmpty(&tiles[i].chevron);
    }
    y += ((ntiles + 2) / 3) * (S(TILE_H) + S(34)) + S(4);

    int cw = S(PANEL_W) - 2 * left;
    SetRectEmpty(&qr_rect);
    SetRectEmpty(&hqr_rect);
    SetRectEmpty(&copy_rect);
    gboolean show_qr = g_strcmp0(child_kind, "share") == 0 && qr_rows && *qr_rows;
    char *info = info_text();
    if (show_qr || info || (hotspot_active && hotspot_qr && *hotspot_qr))
        y += S(14);
    if (show_qr) {
        int size = S(qr_dip);
        SetRect(&qr_rect, (S(PANEL_W) - size) / 2, y, (S(PANEL_W) + size) / 2, y + size);
        y += size + S(10);
    }
    if (info) {
        y += text_height(dc, font_small, info, cw) + S(10);
        g_free(info);
    }
    if (show_qr) {
        SetRect(&copy_rect, (S(PANEL_W) - S(150)) / 2, y, (S(PANEL_W) + S(150)) / 2, y + S(32));
        y += S(44);
    }
    if (hotspot_active && hotspot_qr && *hotspot_qr) {
        int size = S(hqr_dip);
        SetRect(&hqr_rect, (S(PANEL_W) - size) / 2, y, (S(PANEL_W) + size) / 2, y + size);
        y += size + S(8);
        char *t = hotspot_text();
        y += text_height(dc, font_small, t, cw) + S(10);
        g_free(t);
    }
    y += S(8);
    /* Kaki panel: petunjuk pintasan di kiri, tombol pengaturan di kanan. */
    SetRect(&settings_rect, S(PANEL_W) - left - S(36), y + S(8), S(PANEL_W) - left, y + S(44));
    panel_height = y + S(52);
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
    build_layout(dc);

    /* Lapisan bentuk: digambar 3x lebih besar lalu diperkecil dengan HALFTONE. */
    HDC sdc = CreateCompatibleDC(wdc);
    HBITMAP sbmp = CreateCompatibleBitmap(wdc, w * SS, ht * SS);
    HGDIOBJ old_sbmp = SelectObject(sdc, sbmp);
    RECT all = { 0, 0, w * SS, ht * SS };
    HBRUSH bg = CreateSolidBrush(c.bg);
    FillRect(sdc, &all, bg);
    DeleteObject(bg);
    for (int i = 0; i < ntiles; i++) {
        Tile *t = &tiles[i];
        gboolean hot = hover_tile == i && t->enabled;
        COLORREF fill = t->on ? (hot ? c.accent_hover : c.accent) : (hot ? c.tile_hover : c.tile);
        shape(sdc, t->box, S(6), fill, t->on ? CLR_INVALID : c.border);
    }
    if (!IsRectEmpty(&qr_rect)) {
        RECT q = qr_rect;
        InflateRect(&q, S(6), S(6));
        shape(sdc, q, S(10), RGB(255, 255, 255), CLR_INVALID);
    }
    if (!IsRectEmpty(&hqr_rect)) {
        RECT q = hqr_rect;
        InflateRect(&q, S(6), S(6));
        shape(sdc, q, S(10), RGB(255, 255, 255), CLR_INVALID);
    }
    if (!IsRectEmpty(&copy_rect))
        shape(sdc, copy_rect, S(6), hover_copy ? c.tile_hover : c.tile, c.border);
    if (hover_settings)
        shape(sdc, settings_rect, S(6), c.tile_hover, CLR_INVALID);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, 0, 0, w, ht, sdc, 0, 0, w * SS, ht * SS, SRCCOPY);
    SelectObject(sdc, old_sbmp);
    DeleteObject(sbmp);
    DeleteDC(sdc);

    SetBkMode(dc, TRANSPARENT);
    int left = (S(PANEL_W) - 3 * S(TILE_W) - 2 * S(TILE_GAP)) / 2;
    int cw = w - 2 * left;
    RECT title = { left, S(18), w - left, S(18) + S(24) };
    draw_text(dc, "Tsunagu-Pad", title, font_bold, c.fg, DT_SINGLELINE | DT_LEFT);
    const char *sub = share_mode() ? "Layar Windows ke iPad" : "Layar iPad ke Windows (AirPlay)";
    RECT subr = { left, S(44), w - left, S(44) + S(18) };
    draw_text(dc, sub, subr, font_small, c.muted, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);

    for (int i = 0; i < ntiles; i++) {
        Tile *t = &tiles[i];
        COLORREF fg = !t->enabled ? c.muted : t->on ? c.on_fg : c.fg;
        RECT main = t->box;
        if (t->split) {
            main.right = t->chevron.left;
            HPEN p = CreatePen(PS_SOLID, 1, t->on ? c.accent_hover : c.border);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, t->chevron.left, t->box.top + S(1), NULL);
            LineTo(dc, t->chevron.left, t->box.bottom - S(1));
            SelectObject(dc, op);
            DeleteObject(p);
            draw_glyph(dc, L"", t->chevron, font_icon_small, fg);
        }
        draw_glyph(dc, t->glyph, main, font_icon, fg);
        RECT label = { t->box.left - S(4), t->box.bottom + S(6), t->box.right + S(4), t->box.bottom + S(26) };
        draw_text(dc, t->label, label, font_small, t->enabled ? c.fg : c.muted,
                  DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);
    }

    int y = tiles[ntiles - 1].box.bottom + S(34) + S(4);
    if (!IsRectEmpty(&qr_rect) || !IsRectEmpty(&hqr_rect) || g_strcmp0(child_kind, "receive") == 0) {
        HPEN p = CreatePen(PS_SOLID, 1, c.line);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, 0, y + S(4), NULL);
        LineTo(dc, w, y + S(4));
        SelectObject(dc, op);
        DeleteObject(p);
    }
    if (!IsRectEmpty(&qr_rect)) {
        paint_qr(dc, qr_rect, qr_rows);
        y = qr_rect.bottom + S(10);
    } else if (g_strcmp0(child_kind, "receive") == 0) {
        y += S(14);
    }
    char *info = info_text();
    if (info) {
        RECT ir = { left, y, left + cw, y + S(200) };
        draw_text(dc, info, ir, font_small, c.fg, DT_WORDBREAK | DT_CENTER);
        g_free(info);
    }
    if (!IsRectEmpty(&copy_rect))
        draw_text(dc, copied ? "Tersalin" : "Salin alamat", copy_rect, font_normal, c.fg,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    if (!IsRectEmpty(&hqr_rect)) {
        paint_qr(dc, hqr_rect, hotspot_qr);
        char *t = hotspot_text();
        RECT hr = { left, hqr_rect.bottom + S(8), left + cw, hqr_rect.bottom + S(8) + S(120) };
        draw_text(dc, t, hr, font_small, c.fg, DT_WORDBREAK | DT_CENTER);
        g_free(t);
    }

    /* Kaki panel, dengan latar sedikit berbeda seperti Quick Settings. */
    RECT foot = { 0, settings_rect.top - S(8), w, ht };
    HPEN p = CreatePen(PS_SOLID, 1, c.line);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, 0, foot.top, NULL);
    LineTo(dc, w, foot.top);
    SelectObject(dc, op);
    DeleteObject(p);
    RECT hint = { left, settings_rect.top, settings_rect.left - S(8), settings_rect.bottom };
    draw_text(dc, "Ctrl+Alt+I membuka panel ini", hint, font_small, c.muted,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    draw_glyph(dc, L"", settings_rect, font_icon_small, c.fg);

    BitBlt(wdc, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void position_panel(void)
{
    if (!panel_hwnd)
        return;
    /* Di atas area notifikasi pada monitor tempat taskbar berada. */
    APPBARDATA abd = { .cbSize = sizeof abd };
    RECT wa;
    HMONITOR mon = SHAppBarMessage(ABM_GETTASKBARPOS, &abd)
        ? MonitorFromRect(&abd.rc, MONITOR_DEFAULTTOPRIMARY) : MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(mon, &mi);
    wa = mi.rcWork;
    /* Layar laptop yang pendek: QR diperkecil sampai panelnya muat. */
    HDC dc = GetDC(panel_hwnd);
    qr_dip = QR_MAX;
    hqr_dip = 150;
    build_layout(dc);
    while (panel_height > (wa.bottom - wa.top) - S(24) && qr_dip > 120) {
        qr_dip -= 12;
        hqr_dip = MAX(100, hqr_dip - 10);
        build_layout(dc);
    }
    ReleaseDC(panel_hwnd, dc);
    int width = S(PANEL_W), x = wa.right - width - S(12), y = wa.bottom - panel_height - S(12);
    SetWindowPos(panel_hwnd, HWND_TOPMOST, x, MAX(wa.top + S(12), y), width, panel_height, SWP_NOACTIVATE);
    InvalidateRect(panel_hwnd, NULL, FALSE);
}

static void popup_menu(HMENU m, RECT anchor)
{
    UINT cmd = TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                              anchor.left, anchor.bottom, 0, panel_hwnd, NULL);
    PostMessageW(panel_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd)
        on_command(cmd);
}

static void second_menu(RECT anchor)
{
    HMENU m = CreatePopupMenu();
    for (guint i = 0; i < G_N_ELEMENTS(second_sizes); i++)
        add_item(m, ID_SIZE_BASE + i, second_sizes[i].label, g_strcmp0(config.second_size, second_sizes[i].id) == 0,
                 TRUE, TRUE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(second_scales); i++) {
        char *label = g_strdup_printf("Skala %s", second_scales[i].label);
        add_item(m, ID_SCALE_BASE + i, label, g_strcmp0(config.second_scale, second_scales[i].id) == 0, TRUE, TRUE);
        g_free(label);
    }
    popup_menu(m, anchor);
}

static void quality_menu(RECT anchor)
{
    HMENU m = CreatePopupMenu();
    const char *qid = service_share ? "monitor" : config.quality;
    for (guint i = 0; i < G_N_ELEMENTS(qualities); i++) {
        char *label = g_strdup_printf("%s — %s", qualities[i].label, qualities[i].caption);
        add_item(m, ID_QUALITY_BASE + i, label, g_strcmp0(qid, qualities[i].id) == 0, TRUE, TRUE);
        g_free(label);
    }
    popup_menu(m, anchor);
}

static RECT screen_rect(RECT r)
{
    MapWindowPoints(panel_hwnd, NULL, (POINT *) &r, 2);
    return r;
}

static void panel_click(POINT pt)
{
    for (int i = 0; i < ntiles; i++) {
        Tile *t = &tiles[i];
        if (!t->enabled || !PtInRect(&t->box, pt))
            continue;
        gboolean chevron = t->split && PtInRect(&t->chevron, pt);
        switch (t->id) {
        case TILE_MAIN:
            if (chevron) {
                RECT r = screen_rect(t->box);
                POINT p = { r.left, r.bottom };
                show_menu_at(panel_hwnd, p, TRUE);
            } else {
                on_command(ID_TOGGLE);
            }
            break;
        case TILE_RECEIVE:
            on_command(share_mode() ? ID_MODE_RECEIVE : ID_MODE_SHARE);
            break;
        case TILE_SECOND:
            if (chevron)
                second_menu(screen_rect(t->box));
            else
                on_command(ID_SECOND);
            break;
        case TILE_HOTSPOT:
            on_command(chevron ? ID_HOTSPOT_CONFIG : ID_HOTSPOT);
            break;
        case TILE_BOOT:
            on_command(ID_BOOT);
            break;
        case TILE_QUALITY:
            quality_menu(screen_rect(t->box));
            break;
        }
        return;
    }
    if (!IsRectEmpty(&copy_rect) && PtInRect(&copy_rect, pt))
        on_command(ID_COPY);
    else if (PtInRect(&settings_rect, pt)) {
        RECT r = screen_rect(settings_rect);
        POINT p = { r.left, r.top };
        show_menu(panel_hwnd, p);
    }
}

static void panel_hover(POINT pt)
{
    int tile = -1, part = 0;
    for (int i = 0; i < ntiles && tile < 0; i++)
        if (PtInRect(&tiles[i].box, pt)) {
            tile = i;
            part = tiles[i].split && PtInRect(&tiles[i].chevron, pt) ? 2 : 1;
        }
    gboolean copy = !IsRectEmpty(&copy_rect) && PtInRect(&copy_rect, pt);
    gboolean settings = PtInRect(&settings_rect, pt);
    if (tile != hover_tile || part != hover_part || copy != hover_copy || settings != hover_settings) {
        hover_tile = tile;
        hover_part = part;
        hover_copy = copy;
        hover_settings = settings;
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
        hover_tile = -1;
        hover_copy = hover_settings = FALSE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT pt = { (short) LOWORD(lp), (short) HIWORD(lp) };
        panel_click(pt);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        show_menu_at(panel_hwnd, pt, TRUE);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !dialog_hwnd)
            ShowWindow(h, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            ShowWindow(h, SW_HIDE);
        return 0;
    case WM_SETTINGCHANGE:
        InvalidateRect(h, NULL, FALSE);   /* tema gelap/terang berganti */
        return 0;
    case WM_COMMAND:
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
                                     WS_POPUP, 0, 0, S(PANEL_W), S(300), NULL, NULL, instance, NULL);
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
    position_panel();
    ShowWindow(panel_hwnd, SW_SHOW);
    SetForegroundWindow(panel_hwnd);
}

static void sync_ui(void)
{
    update_tray();
    if (panel_hwnd && IsWindowVisible(panel_hwnd))
        position_panel();
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
        show_hotspot_dialog();
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
        if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) {
            if (panel_hwnd && IsWindowVisible(panel_hwnd))
                ShowWindow(panel_hwnd, SW_HIDE);
            else
                show_panel();
        } else if (LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            show_menu(h, pt);
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
            if (hotspot_active && !hotspot_busy && ticks % 3 == 0)
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
    wc.style = 0;
    wc.lpfnWndProc = dialog_proc;
    wc.lpszClassName = L"TsunaguPadDialog";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
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
    refresh_service_share();
    update_tray();
    if (toggle_arg)
        PostMessageW(main_hwnd, WM_COMMAND, ID_TOGGLE, 0);
    else if (!autostarted)
        show_panel();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (dialog_hwnd && IsDialogMessageW(dialog_hwnd, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (child_job)
        CloseHandle(child_job);
    CloseHandle(single);
    return 0;
}
