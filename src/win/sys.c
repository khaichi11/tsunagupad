/* Tsunagu-Pad untuk Windows: proses, jaringan, dan perintah on/off/status.
 * SPDX-License-Identifier: MIT
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <iphlpapi.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include "win.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE) -4)
#endif

/* Siapa saja yang masuk ke komputer ini boleh meminta proses berhenti;
 * SYSTEM dan Administrators memegang hak penuh. Dipakai juga oleh proses
 * yang dijalankan layanan sebagai SYSTEM, supaya panel pengguna tetap bisa
 * menghentikannya. */
#define QUIT_EVENT_SDDL L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100002;;;AU)"

/* ---------- teks ---------- */

wchar_t *win_utf16(const char *utf8)
{
    return (wchar_t *) g_utf8_to_utf16(utf8 ? utf8 : "", -1, NULL, NULL, NULL);
}

char *win_utf8(const wchar_t *utf16)
{
    return g_utf16_to_utf8((const gunichar2 *) (utf16 ? utf16 : L""), -1, NULL, NULL, NULL);
}

/* Aturan kutip baris perintah Windows (dibaca CommandLineToArgvW dan CRT). */
static void quote_arg(GString *out, const char *arg)
{
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        g_string_append(out, arg);
        return;
    }
    g_string_append_c(out, '"');
    for (const char *p = arg;; p++) {
        guint slashes = 0;
        while (*p == '\\') {
            p++;
            slashes++;
        }
        if (!*p) {
            for (guint i = 0; i < slashes * 2; i++)
                g_string_append_c(out, '\\');
            break;
        }
        if (*p == '"') {
            for (guint i = 0; i < slashes * 2 + 1; i++)
                g_string_append_c(out, '\\');
            g_string_append_c(out, '"');
        } else {
            for (guint i = 0; i < slashes; i++)
                g_string_append_c(out, '\\');
            g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
}

char *win_quote_args(const char *const *argv)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; argv[i]; i++) {
        if (i)
            g_string_append_c(out, ' ');
        quote_arg(out, argv[i]);
    }
    return g_string_free(out, FALSE);
}

/* ---------- lokasi ---------- */

char *win_exe_path(void)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, G_N_ELEMENTS(buf));
    if (n == 0 || n >= G_N_ELEMENTS(buf))
        return NULL;
    return win_utf8(buf);
}

char *win_install_dir(void)
{
    char *exe = win_exe_path();
    if (!exe)
        return g_strdup(".");
    char *bindir = g_path_get_dirname(exe);
    char *base = g_path_get_basename(bindir);
    char *root = g_ascii_strcasecmp(base, "bin") == 0 ? g_path_get_dirname(bindir) : g_strdup(bindir);
    g_free(base);
    g_free(bindir);
    g_free(exe);
    return root;
}

char *win_state_dir(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "tsunagupad", NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

char *win_program_data_dir(void)
{
    const char *base = g_getenv("ProgramData");
    return g_build_filename(base && *base ? base : "C:\\ProgramData", "TsunaguPad", NULL);
}

char *win_find_web_dir(const char *given)
{
    if (given)
        return g_strdup(given);
    char *root = win_install_dir();
    char *exe = win_exe_path();
    char *bindir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *candidates[] = {
        g_build_filename(root, "share", "tsunagupad", "web", NULL),
        g_build_filename(bindir, "..", "web", NULL),     /* dijalankan dari folder build\ */
        g_build_filename(bindir, "web", NULL),
#ifdef TSUNAGUPAD_WEB_DIR
        g_strdup(TSUNAGUPAD_WEB_DIR),
#endif
        NULL,
    };
    char *found = NULL;
    for (int i = 0; candidates[i]; i++) {
        char *index = g_build_filename(candidates[i], "index.html", NULL);
        if (!found && g_file_test(index, G_FILE_TEST_EXISTS))
            found = g_canonicalize_filename(candidates[i], NULL);
        g_free(index);
        g_free(candidates[i]);
    }
    g_free(root);
    g_free(exe);
    g_free(bindir);
    return found;
}

char *win_find_uxplay(void)
{
    char *exe = win_exe_path();
    char *dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *local = g_build_filename(dir, "uxplay.exe", NULL);
    g_free(exe);
    g_free(dir);
    if (g_file_test(local, G_FILE_TEST_IS_REGULAR))
        return local;
    g_free(local);
    return g_find_program_in_path("uxplay");
}

/* ---------- awal proses ---------- */

/* Plugin GStreamer ikut dipasang di <folder>\lib\gstreamer-1.0. Lokasinya
 * ditetapkan di sini supaya GStreamer lain yang mungkin terpasang (MSYS2 atau
 * pemasang resmi) tidak ikut termuat dan bentrok versinya. */
static void setup_gstreamer_env(void)
{
    char *root = win_install_dir();
    char *plugins = g_build_filename(root, "lib", "gstreamer-1.0", NULL);
    if (g_file_test(plugins, G_FILE_TEST_IS_DIR) && !g_getenv("TSUNAGUPAD_KEEP_GST_ENV")) {
        g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", plugins, TRUE);
        g_unsetenv("GST_PLUGIN_PATH");
        g_unsetenv("GST_PLUGIN_PATH_1_0");
        g_unsetenv("GST_PLUGIN_SYSTEM_PATH");
        char *scanner = g_build_filename(root, "libexec", "gstreamer-1.0", "gst-plugin-scanner.exe", NULL);
        if (g_file_test(scanner, G_FILE_TEST_IS_REGULAR))
            g_setenv("GST_PLUGIN_SCANNER_1_0", scanner, TRUE);
        g_free(scanner);
        char *state = win_state_dir();
        char *name = g_strdup_printf("gstreamer-registry-%s.bin", TSUNAGUPAD_VERSION);
        char *registry = g_build_filename(state, name, NULL);
        g_setenv("GST_REGISTRY_1_0", registry, TRUE);
        g_free(registry);
        g_free(name);
        g_free(state);
    }
    g_free(plugins);
    g_free(root);
}

char **win_init_process(int *argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /* Koordinat dari iPad dipetakan ke piksel fisik monitor; tanpa ini Windows
     * memberi koordinat yang sudah diskala DPI dan sentuhan meleset. */
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    SetCtx set_ctx = (SetCtx) (void *) GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                      "SetProcessDpiAwarenessContext");
    if (!set_ctx || !set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    SetConsoleOutputCP(CP_UTF8);
    /* Baris JSON untuk panel dibaca apa adanya; tanpa \r tambahan. */
    _setmode(_fileno(stdout), _O_BINARY);

    setup_gstreamer_env();

    char **args = g_win32_get_command_line();
    if (!args)
        return argv;
    *argc = (int) g_strv_length(args);
    return args;
}

gboolean win_random(guint8 *buf, gsize len)
{
    return BCryptGenRandom(NULL, buf, (ULONG) len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

/* ---------- alamat untuk iPad ---------- */

typedef struct {
    int rank;
    guint order;
    char *addr;
} Address;

static gboolean contains_ci(const wchar_t *hay, const wchar_t *needle)
{
    if (!hay)
        return FALSE;
    char *h = win_utf8(hay), *n = win_utf8(needle);
    char *hl = g_utf8_strdown(h, -1), *nl = g_utf8_strdown(n, -1);
    gboolean found = strstr(hl, nl) != NULL;
    g_free(h);
    g_free(n);
    g_free(hl);
    g_free(nl);
    return found;
}

static gint compare_address(gconstpointer a, gconstpointer b)
{
    const Address *x = a, *y = b;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* Urutan sama seperti di Linux: hotspot Tsunagu-Pad (Mobile Hotspot Windows),
 * Tailscale, Wi-Fi, lalu sisanya. Adaptor jaringan virtual (WSL, Hyper-V,
 * VirtualBox, VMware, Docker) ditaruh paling akhir, bukan dibuang: di
 * komputer yang tersambung lewat switch eksternal Hyper-V, "vEthernet"
 * justru satu-satunya jalan ke jaringan. */
static GArray *collect_addresses(void)
{
    static const wchar_t *const virtual_nets[] = {
        L"vEthernet", L"Hyper-V Virtual", L"VirtualBox", L"VMware", L"WSL", L"Docker",
    };
    GArray *found = g_array_new(FALSE, FALSE, sizeof(Address));
    ULONG size = 32 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && ret == ERROR_BUFFER_OVERFLOW; tries++) {
        g_free(list);
        list = g_malloc(size);
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    if (ret == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            /* Jaringan host-only (VirtualBox, VMware) hanya menuju mesin
             * virtual di laptop ini; iPad tidak pernah bisa sampai ke sana. */
            if (contains_ci(a->Description, L"Host-Only") || contains_ci(a->FriendlyName, L"Host-Only"))
                continue;
            gboolean virtual_net = FALSE;
            for (guint v = 0; v < G_N_ELEMENTS(virtual_nets); v++)
                virtual_net = virtual_net || contains_ci(a->FriendlyName, virtual_nets[v]) ||
                              contains_ci(a->Description, virtual_nets[v]);
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                struct sockaddr_in *in = (struct sockaddr_in *) u->Address.lpSockaddr;
                guint8 *b = (guint8 *) &in->sin_addr;
                if (b[0] == 169 && b[1] == 254)
                    continue;   /* alamat otomatis tanpa DHCP, tidak terjangkau iPad */
                char buf[INET_ADDRSTRLEN];
                g_snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                int rank = 3;
                if (contains_ci(a->Description, L"Wi-Fi Direct Virtual") ||
                    (b[0] == 192 && b[1] == 168 && b[2] == 137))
                    rank = 0;   /* Mobile Hotspot: iPad tersambung langsung ke laptop */
                else if (contains_ci(a->FriendlyName, L"Tailscale") || contains_ci(a->Description, L"Tailscale"))
                    rank = 1;   /* Tailscale menembus client isolation jaringan kampus */
                else if (a->IfType == IF_TYPE_IEEE80211)
                    rank = 2;
                else if (virtual_net)
                    rank = 4;
                Address addr = { rank, found->len, g_strdup(buf) };
                g_array_append_val(found, addr);
            }
        }
    }
    g_free(list);
    g_array_sort(found, compare_address);
    return found;
}

GPtrArray *win_local_addresses(void)
{
    GArray *found = collect_addresses();
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < found->len; i++)
        g_ptr_array_add(out, g_array_index(found, Address, i).addr);
    g_array_free(found, TRUE);
    return out;
}

/* Alamat laptop di jaringan Mobile Hotspot, atau NULL bila hotspot mati. */
char *win_hotspot_address(void)
{
    GArray *found = collect_addresses();
    char *out = NULL;
    for (guint i = 0; i < found->len; i++) {
        Address *a = &g_array_index(found, Address, i);
        if (a->rank == 0 && !out)
            out = g_strdup(a->addr);
        g_free(a->addr);
    }
    g_array_free(found, TRUE);
    return out;
}

/* ---------- berhenti dengan rapi ---------- */

static GMainLoop *quit_loop;

static gboolean quit_idle(gpointer data)
{
    if (quit_loop)
        g_main_loop_quit(quit_loop);
    return G_SOURCE_REMOVE;
}

static BOOL WINAPI on_console_ctrl(DWORD type)
{
    g_idle_add(quit_idle, NULL);
    /* Saat jendela ditutup atau Windows logoff, proses hanya diberi beberapa
     * detik; ditunggu sebentar supaya monitor virtual sempat dilepas. */
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        Sleep(3000);
    return TRUE;
}

/* Proses milik pengguna memakai ruang nama sesinya sendiri ("Local\\").
 * Proses yang dijalankan layanan sebagai SYSTEM memakai "Global\\", supaya
 * layanan (sesi 0) dan panel pengguna sama-sama bisa menemukannya. */
static wchar_t *quit_event_name(DWORD pid, gboolean global)
{
    wchar_t *name = g_new(wchar_t, 64);
    _snwprintf(name, 64, global ? L"Global\\tsunagupad-quit-%lu" : L"Local\\tsunagupad-quit-%lu",
               (unsigned long) pid);
    name[63] = 0;
    return name;
}

HANDLE win_create_quit_event(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(QUIT_EVENT_SDDL, SDDL_REVISION_1, &sd, NULL))
        sa.lpSecurityDescriptor = sd;
    HANDLE event = NULL;
    if (g_getenv("TSUNAGUPAD_SERVICE")) {
        wchar_t *name = quit_event_name(GetCurrentProcessId(), TRUE);
        event = CreateEventW(&sa, TRUE, FALSE, name);
        g_free(name);
    }
    if (!event) {
        wchar_t *name = quit_event_name(GetCurrentProcessId(), FALSE);
        event = CreateEventW(&sa, TRUE, FALSE, name);
        g_free(name);
    }
    if (sd)
        LocalFree(sd);
    return event;
}

static DWORD WINAPI quit_event_thread(LPVOID event)
{
    WaitForSingleObject(event, INFINITE);
    g_idle_add(quit_idle, NULL);
    return 0;
}

/* Pengganti SIGTERM: panel memberi tanda lewat event bernama, lalu proses
 * keluar dari main loop dan membereskan monitor virtual serta tombol yang
 * masih tertekan sebelum selesai. */
void win_quit_watch(GMainLoop *loop)
{
    quit_loop = loop;
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    HANDLE event = win_create_quit_event();
    if (event) {
        HANDLE thread = CreateThread(NULL, 0, quit_event_thread, event, 0, NULL);
        if (thread)
            CloseHandle(thread);
    }
}

gboolean win_signal_quit(guint32 pid, guint timeout_ms)
{
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!proc)
        proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    HANDLE event = NULL;
    for (int global = 0; global < 2 && !event; global++) {
        wchar_t *name = quit_event_name(pid, global);
        event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        g_free(name);
    }
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
    gboolean gone = FALSE;
    if (proc) {
        gone = WaitForSingleObject(proc, event ? timeout_ms : 0) == WAIT_OBJECT_0;
        if (!gone && TerminateProcess(proc, 1))
            gone = WaitForSingleObject(proc, 2000) == WAIT_OBJECT_0;
        CloseHandle(proc);
    }
    return gone;
}

/* ---------- proses ---------- */

static gboolean listener_pid(ULONG family, guint port, DWORD *pid)
{
    DWORD size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    void *table = g_malloc(size + 1024);
    size += 1024;
    gboolean found = FALSE;
    if (GetExtendedTcpTable(table, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        if (family == AF_INET) {
            MIB_TCPTABLE_OWNER_PID *t = table;
            for (DWORD i = 0; i < t->dwNumEntries && !found; i++)
                if (ntohs((u_short) t->table[i].dwLocalPort) == port) {
                    *pid = t->table[i].dwOwningPid;
                    found = TRUE;
                }
        } else {
            MIB_TCP6TABLE_OWNER_PID *t = table;
            for (DWORD i = 0; i < t->dwNumEntries && !found; i++)
                if (ntohs((u_short) t->table[i].dwLocalPort) == port) {
                    *pid = t->table[i].dwOwningPid;
                    found = TRUE;
                }
        }
    }
    g_free(table);
    return found;
}

guint32 win_port_owner(guint port)
{
    DWORD pid = 0;
    if (listener_pid(AF_INET, port, &pid) || listener_pid(AF_INET6, port, &pid))
        return pid;
    return 0;
}

gboolean win_process_is(guint32 pid, const char *exe_name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    PROCESSENTRY32W e = { .dwSize = sizeof e };
    gboolean match = FALSE;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID != pid)
            continue;
        char *name = win_utf8(e.szExeFile);
        match = g_ascii_strcasecmp(name, exe_name) == 0;
        g_free(name);
        break;
    }
    CloseHandle(snap);
    return match;
}

/* Setelah panel dimuat ulang atau proses lama tertinggal, port 8765 masih
 * dipegang tsunagupad.exe yang lama. Hentikan dulu sebelum mulai. */
void win_stop_stale_share(guint port)
{
    guint32 pid = win_port_owner(port);
    if (!pid || pid == GetCurrentProcessId() || !win_process_is(pid, "tsunagupad.exe"))
        return;
    win_signal_quit(pid, 4000);
    /* Proses milik layanan (SYSTEM) tidak bisa ditunggu lewat handle-nya oleh
     * pengguna biasa; yang ditunggu adalah port-nya sampai dilepas. */
    for (int i = 0; i < 50 && win_port_owner(port) == pid; i++)
        Sleep(100);
}

gboolean win_console_session(void)
{
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session) &&
           session == WTSGetActiveConsoleSessionId();
}

gboolean win_is_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation = { 0 };
    DWORD size = sizeof elevation;
    gboolean elevated = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, size, &size))
            elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

/* ---------- menjalankan program lain ---------- */

typedef struct {
    HANDLE pipe;
    char *data;
} Feed;

static DWORD WINAPI feed_stdin(LPVOID p)
{
    Feed *f = p;
    DWORD written;
    if (f->data)
        WriteFile(f->pipe, f->data, (DWORD) strlen(f->data), &written, NULL);
    CloseHandle(f->pipe);
    return 0;
}

gboolean win_run(const char *const *argv, const char *input, char **output, int *exit_code,
                 GError **error)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Pipa proses gagal dibuat");
        return FALSE;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi = { 0 };
    char *cmdline = win_quote_args(argv);
    wchar_t *wcmd = win_utf16(cmdline);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    g_free(wcmd);
    g_free(cmdline);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        char *msg = g_win32_error_message(GetLastError());
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s tidak bisa dijalankan: %s", argv[0], msg);
        g_free(msg);
        CloseHandle(out_r);
        CloseHandle(in_w);
        return FALSE;
    }

    Feed feed = { in_w, (char *) input };
    HANDLE writer = CreateThread(NULL, 0, feed_stdin, &feed, 0, NULL);
    if (!writer)
        CloseHandle(in_w);

    GString *buf = g_string_new(NULL);
    char chunk[4096];
    DWORD got;
    while (ReadFile(out_r, chunk, sizeof chunk, &got, NULL) && got > 0)
        g_string_append_len(buf, chunk, got);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (writer) {
        WaitForSingleObject(writer, INFINITE);
        CloseHandle(writer);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code)
        *exit_code = (int) code;
    if (output)
        *output = g_string_free(buf, FALSE);
    else
        g_string_free(buf, TRUE);
    return TRUE;
}

/* ---------- kendali dari terminal atau SSH ---------- */

#define SHARE_PORT 8765

static char *session_log_path(void)
{
    char *dir = win_state_dir();
    char *path = g_build_filename(dir, "session.log", NULL);
    g_free(dir);
    return path;
}

/* Baris "ready" terakhir dari log sesi; berisi alamat untuk iPad. */
static char *last_ready_urls(const char *log)
{
    char *data = NULL;
    if (!g_file_get_contents(log, &data, NULL, NULL))
        return NULL;
    char *urls = NULL;
    char **lines = g_strsplit(data, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *key = strstr(lines[i], "\"urls\":\"");
        if (!key)
            continue;
        key += 8;
        const char *end = strchr(key, '"');
        if (end) {
            g_free(urls);
            urls = g_strndup(key, end - key);
        }
    }
    g_strfreev(lines);
    g_free(data);
    return urls;
}

/* Log sesi yang dijalankan layanan (layar login, sesi berikutnya). */
static char *service_log_path(void)
{
    char *dir = win_program_data_dir();
    char *path = g_build_filename(dir, "session", "tsunagupad", "session.log", NULL);
    g_free(dir);
    return path;
}

static char *running_urls(void)
{
    char *log = session_log_path();
    char *urls = last_ready_urls(log);
    g_free(log);
    if (!urls) {
        log = service_log_path();
        urls = last_ready_urls(log);
        g_free(log);
    }
    return urls;
}

static void print_urls(const char *prefix, const char *urls)
{
    char **list = g_strsplit(urls, " ", -1);
    for (int u = 0; list[u]; u++)
        if (*list[u])
            g_print("%s%s\n", prefix, list[u]);
    g_strfreev(list);
}

static guint32 running_share(void)
{
    guint32 pid = win_port_owner(SHARE_PORT);
    return pid && win_process_is(pid, "tsunagupad.exe") ? pid : 0;
}

static int wait_for_ready(const char *log, gboolean (*alive)(void))
{
    g_print("Menunggu Tsunagu-Pad siap…\n");
    for (int i = 0; i < 120; i++) {
        g_usleep(500 * 1000);
        char *urls = last_ready_urls(log);
        if (urls && running_share()) {
            g_print("Aktif. Buka di iPad:\n");
            print_urls("  ", urls);
            g_free(urls);
            return 0;
        }
        g_free(urls);
        if (i > 10 && alive && !alive())
            break;
    }
    g_printerr("Gagal. Lihat %s\n", log);
    return 1;
}

static HANDLE detached;

static gboolean detached_alive(void)
{
    return detached && WaitForSingleObject(detached, 0) == WAIT_TIMEOUT;
}

int win_cmd_on(int argc, char **argv)
{
    if (running_share()) {
        g_print("Tsunagu-Pad sudah berjalan.\n");
        return win_cmd_status();
    }

    /* Lewat SSH proses berada di sesi 0 yang tidak punya layar. Layanan
     * Tsunagu-Pad bisa menjalankannya di sesi konsol, termasuk layar login. */
    if (!win_console_session()) {
        if (!win_service_installed()) {
            g_printerr("Sesi ini tidak punya layar (mis. SSH) dan layanan Tsunagu-Pad belum "
                       "terpasang. Jalankan dari desktop, atau pasang ulang Tsunagu-Pad.\n");
            return 1;
        }
        GString *cmd = g_string_new("share-on");
        for (int i = 1; i < argc; i++) {
            g_string_append_c(cmd, ' ');
            quote_arg(cmd, argv[i]);
        }
        char *reply = NULL;
        GError *err = NULL;
        gboolean ok = win_service_request(cmd->str, &reply, &err);
        g_string_free(cmd, TRUE);
        if (!ok) {
            g_printerr("Layanan Tsunagu-Pad tidak menjawab: %s\n", err ? err->message : "?");
            g_clear_error(&err);
            return 1;
        }
        g_free(reply);
        /* Log layanan hanya bisa dibaca administrator karena memuat kode
         * akses; pengguna biasa cukup melihat bahwa sesinya sudah jalan. */
        for (int i = 0; i < 120 && !running_share(); i++)
            g_usleep(500 * 1000);
        if (!running_share()) {
            g_printerr("Layanan belum berhasil menyalakan Tsunagu-Pad. Lihat %s (butuh administrator).\n",
                       "%ProgramData%\\TsunaguPad\\session\\tsunagupad\\session.log");
            return 1;
        }
        char *urls = running_urls();
        if (urls) {
            g_print("Aktif. Buka di iPad:\n");
            print_urls("  ", urls);
            g_free(urls);
        } else {
            g_print("Aktif. Buka alamat yang tersimpan di Layar Utama iPad.\n");
        }
        return 0;
    }

    char *self = win_exe_path();
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, self);
    g_ptr_array_add(args, "share");
    for (int i = 1; i < argc; i++)
        g_ptr_array_add(args, argv[i]);
    g_ptr_array_add(args, NULL);
    char *cmdline = win_quote_args((const char *const *) args->pdata);
    g_ptr_array_free(args, TRUE);

    char *log = session_log_path();
    wchar_t *wlog = win_utf16(log);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE fh = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wlog);
    if (fh == INVALID_HANDLE_VALUE) {
        g_free(cmdline);
        g_free(log);
        g_printerr("Tidak bisa menulis log sesi\n");
        return 1;
    }

    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = fh;
    si.hStdError = fh;
    PROCESS_INFORMATION pi = { 0 };
    wchar_t *wcmd = win_utf16(cmdline);
    /* Lepas dari konsol dan dari job terminal ini, supaya proses tetap
     * berjalan setelah jendela terminal atau sesi SSH ditutup. */
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB,
                             NULL, NULL, &si, &pi);
    if (!ok)
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                            NULL, NULL, &si, &pi);
    g_free(wcmd);
    g_free(cmdline);
    CloseHandle(fh);
    if (!ok) {
        g_free(log);
        g_printerr("Gagal menjalankan proses berbagi layar\n");
        return 1;
    }
    CloseHandle(pi.hThread);
    detached = pi.hProcess;
    int rc = wait_for_ready(log, detached_alive);
    CloseHandle(detached);
    detached = NULL;
    g_free(log);
    return rc;
}

int win_cmd_off(void)
{
    guint32 pid = running_share();
    if (!pid) {
        g_print("Tsunagu-Pad tidak sedang berjalan.\n");
        return 0;
    }
    if (!win_signal_quit(pid, 5000) && win_service_installed()) {
        /* Proses milik layanan (SYSTEM) hanya bisa dihentikan oleh layanannya. */
        win_service_request("share-off", NULL, NULL);
    }
    g_print("Tsunagu-Pad dihentikan.\n");
    return 0;
}

int win_cmd_status(void)
{
    guint32 pid = running_share();
    g_print("Berbagi layar : %s\n", pid ? "aktif" : "mati");
    if (pid) {
        char *urls = running_urls();
        if (urls)
            print_urls("Alamat        : ", urls);
        g_free(urls);
    }
    const char *cap = win_hotspot_capability();
    if (g_str_equal(cap, "no"))
        g_print("Hotspot       : tidak didukung\n");
    else
        g_print("Hotspot       : %s\n", win_hotspot_active() ? "active" : "inactive");
    return pid ? 0 : 1;
}
