/* Layanan Windows "TsunaguPad": iPad aktif di layar login dan sesi berikutnya.
 *
 * Padanan dari `make login-setup` dan service systemd di Linux. Layanan ini
 * berjalan sebagai LocalSystem dan, bila sakelar "iPad aktif di sesi
 * berikutnya" dinyalakan, menjalankan `tsunagupad share --quality monitor`
 * di sesi konsol yang sedang aktif. Proses itu berjalan sebagai SYSTEM,
 * sehingga bisa menangkap dan mengendalikan layar login, layar kunci, dan
 * jendela UAC, lalu tetap jalan setelah pengguna masuk.
 *
 * Keamanan:
 *   - kode akses salinannya disimpan di %ProgramData%\TsunaguPad\session,
 *     yang hanya bisa dibaca SYSTEM dan Administrators;
 *   - menyalakan atau mematikan sakelarnya butuh hak administrator;
 *   - pipa kendali hanya menerima klien lokal, dan hanya perintah tertentu
 *     dengan opsi yang sudah disaring (misalnya --web-dir ditolak, supaya
 *     proses SYSTEM tidak bisa disuruh menyajikan berkas sembarangan).
 *
 *   tsunagupad service install | uninstall | start | stop
 *   tsunagupad service enable [--token-file BERKAS] | disable | status
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wtsapi32.h>

#include <glib/gstdio.h>

#include "win.h"

#define PIPE_NAME L"\\\\.\\pipe\\tsunagupad-service"
/* SYSTEM dan Administrators penuh; pengguna yang masuk ke komputer ini boleh
 * mengirim perintah. Klien dari jaringan (SMB) ditolak lewat bendera pipa. */
#define PIPE_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)"
/* Folder data: pengguna boleh membaca, hanya SYSTEM/Administrators yang menulis. */
#define DATA_SDDL L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)"
/* Folder sesi (kode akses dan log berisi alamat): pengguna biasa tidak boleh masuk. */
#define SESSION_SDDL L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)"

/* ---------- lokasi ---------- */

static char *data_path(const char *name)
{
    char *dir = win_program_data_dir();
    char *path = name ? g_build_filename(dir, name, NULL) : g_strdup(dir);
    g_free(dir);
    return path;
}

/* XDG_CONFIG_HOME proses SYSTEM: GLib mencari tsunagupad\token di bawahnya. */
static char *session_config_home(void)
{
    return data_path("session");
}

static char *session_file(const char *name)
{
    char *home = session_config_home();
    char *path = g_build_filename(home, "tsunagupad", name, NULL);
    g_free(home);
    return path;
}

static gboolean make_dir_with_acl(const char *path, const wchar_t *sddl)
{
    g_mkdir_with_parents(path, 0700);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL))
        return FALSE;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = NULL;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    wchar_t *wpath = win_utf16(path);
    DWORD r = SetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL);
    g_free(wpath);
    LocalFree(sd);
    return r == ERROR_SUCCESS;
}

static gboolean secure_dirs(void)
{
    char *root = data_path(NULL);
    char *home = session_config_home();
    char *inner = g_build_filename(home, "tsunagupad", NULL);
    gboolean ok = make_dir_with_acl(root, DATA_SDDL) && make_dir_with_acl(home, SESSION_SDDL);
    g_mkdir_with_parents(inner, 0700);
    g_free(root);
    g_free(home);
    g_free(inner);
    return ok;
}

static gboolean is_enabled(void)
{
    char *flag = data_path("enabled");
    char *token = session_file("token");
    gboolean on = g_file_test(flag, G_FILE_TEST_EXISTS) && g_file_test(token, G_FILE_TEST_EXISTS);
    g_free(flag);
    g_free(token);
    return on;
}

/* ---------- opsi yang boleh diteruskan ke proses SYSTEM ---------- */

static gboolean value_ok(const char *opt, const char *v)
{
    static const char *const qualities[] = { "hemat", "lancar", "seimbang", "tajam", "monitor", NULL };
    static const char *const encoders[] = { "auto", "nvidia", "intel", "amd", "mf", "cpu", "cpu-sw", NULL };
    static const char *const pens[] = { "auto", "tablet", "mouse", NULL };
    static const char *const cursors[] = { "sistem", "lokal", NULL };
    if (!v || !*v || strlen(v) > 64)
        return FALSE;
    for (const char *p = v; *p; p++)
        if ((guchar) *p < 0x20 || *p == '"')
            return FALSE;
    if (g_str_equal(opt, "--quality"))
        return g_strv_contains(qualities, v);
    if (g_str_equal(opt, "--encoder"))
        return g_strv_contains(encoders, v);
    if (g_str_equal(opt, "--pen"))
        return g_strv_contains(pens, v);
    if (g_str_equal(opt, "--cursor"))
        return g_strv_contains(cursors, v);
    if (g_str_equal(opt, "--second")) {
        int w, h;
        char tail;
        return sscanf(v, "%dx%d%c", &w, &h, &tail) == 2 && w >= 320 && h >= 240 && w <= 7680 && h <= 4320;
    }
    if (g_str_equal(opt, "--scale")) {
        if (g_str_equal(v, "ikut"))
            return TRUE;
        char *end = NULL;
        double s = g_ascii_strtod(v, &end);
        return end && !*end && s >= 0.5 && s <= 4;
    }
    if (g_str_equal(opt, "--monitor"))
        return TRUE;    /* hanya dipakai untuk mencari monitor */
    return FALSE;
}

/* Mengubah argumen kiriman klien menjadi daftar yang aman, atau NULL. */
static GPtrArray *filter_args(const char *line, char **why)
{
    static const struct { const char *name, *alias; gboolean value; } allowed[] = {
        { "--quality", "-q", TRUE }, { "--encoder", "-e", TRUE }, { "--pen", NULL, TRUE },
        { "--cursor", NULL, TRUE }, { "--second", NULL, TRUE }, { "--scale", NULL, TRUE },
        { "--monitor", "-m", TRUE }, { "--no-input", NULL, FALSE },
    };
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    if (!line || !*line)
        return out;
    int n = 0;
    wchar_t *wline = win_utf16(line);
    wchar_t **wargv = CommandLineToArgvW(wline, &n);
    g_free(wline);
    if (!wargv) {
        *why = g_strdup("argumen tidak bisa dibaca");
        g_ptr_array_unref(out);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        char *arg = win_utf8(wargv[i]);
        char *value = NULL;
        char *eq = g_str_has_prefix(arg, "--") ? strchr(arg, '=') : NULL;
        if (eq) {
            value = g_strdup(eq + 1);
            *eq = '\0';
        }
        int match = -1;
        for (guint k = 0; k < G_N_ELEMENTS(allowed) && match < 0; k++)
            if (g_str_equal(arg, allowed[k].name) || g_strcmp0(arg, allowed[k].alias) == 0)
                match = (int) k;
        if (match < 0) {
            *why = g_strdup_printf("opsi %s tidak diizinkan lewat layanan", arg);
        } else if (allowed[match].value) {
            if (!value && i + 1 < n)
                value = win_utf8(wargv[++i]);
            if (!value_ok(allowed[match].name, value))
                *why = g_strdup_printf("nilai untuk %s tidak diizinkan", allowed[match].name);
            else {
                g_ptr_array_add(out, g_strdup(allowed[match].name));
                g_ptr_array_add(out, g_strdup(value));
            }
        } else {
            g_ptr_array_add(out, g_strdup(allowed[match].name));
        }
        g_free(arg);
        g_free(value);
        if (*why)
            break;
    }
    LocalFree(wargv);
    if (*why) {
        g_ptr_array_unref(out);
        return NULL;
    }
    return out;
}

/* ---------- bagian layanan ---------- */

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS svc_status;
static HANDLE stop_event, wake_event;
static CRITICAL_SECTION lock;
static HANDLE child;
static DWORD child_pid, child_session = (DWORD) -1;
static gboolean suppressed;         /* dimatikan pengguna; tunggu sesi berikutnya */
static gboolean manual;             /* dinyalakan lewat `tsunagupad on` */
static GPtrArray *manual_args;
static gint64 restart_at;

static void report(DWORD state)
{
    svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    svc_status.dwCurrentState = state;
    svc_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    svc_status.dwWin32ExitCode = NO_ERROR;
    svc_status.dwWaitHint = state == SERVICE_RUNNING ? 0 : 5000;
    SetServiceStatus(status_handle, &svc_status);
}

static wchar_t *child_environment(void)
{
    /* Lingkungan layanan (milik sistem, bukan pengguna) ditambah lokasi
     * konfigurasi sesi dan penanda bahwa proses ini milik layanan. */
    char *home = session_config_home();
    char *xdg = g_strdup_printf("XDG_CONFIG_HOME=%s", home);
    g_free(home);
    GString *block = g_string_new(NULL);
    wchar_t *env = GetEnvironmentStringsW();
    for (wchar_t *p = env; p && *p; p += wcslen(p) + 1) {
        char *entry = win_utf8(p);
        gboolean drop = g_ascii_strncasecmp(entry, "XDG_CONFIG_HOME=", 16) == 0 ||
                        g_ascii_strncasecmp(entry, "GST_", 4) == 0 ||
                        g_ascii_strncasecmp(entry, "TSUNAGUPAD_", 11) == 0;
        if (!drop) {
            g_string_append(block, entry);
            g_string_append_c(block, '\x01');
        }
        g_free(entry);
    }
    if (env)
        FreeEnvironmentStringsW(env);
    g_string_append(block, xdg);
    g_string_append_c(block, '\x01');
    g_string_append(block, "TSUNAGUPAD_SERVICE=1");
    g_string_append_c(block, '\x01');
    g_free(xdg);
    /* Pemisah sementara \x01 diganti NUL setelah diubah ke UTF-16. */
    wchar_t *wide = win_utf16(block->str);
    gsize len = wcslen(wide);
    for (gsize i = 0; i < len; i++)
        if (wide[i] == 1)
            wide[i] = 0;
    wchar_t *out = g_new0(wchar_t, len + 2);
    memcpy(out, wide, len * sizeof(wchar_t));
    g_free(wide);
    g_string_free(block, TRUE);
    return out;
}

static gboolean start_child(DWORD session)
{
    HANDLE self = NULL, primary = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &self))
        return FALSE;
    gboolean ok = DuplicateTokenEx(self, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &primary) &&
                  SetTokenInformation(primary, TokenSessionId, &session, sizeof session);
    CloseHandle(self);
    if (!ok) {
        if (primary)
            CloseHandle(primary);
        return FALSE;
    }

    char *exe = win_exe_path();
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, exe);
    g_ptr_array_add(args, "share");
    gboolean has_quality = FALSE;
    if (manual && manual_args)
        for (guint i = 0; i < manual_args->len; i++) {
            has_quality = has_quality || g_str_equal(manual_args->pdata[i], "--quality");
            g_ptr_array_add(args, manual_args->pdata[i]);
        }
    if (!has_quality) {
        /* Sama seperti service systemd: 1080p tanpa skala agar tulisan tajam. */
        g_ptr_array_add(args, "--quality");
        g_ptr_array_add(args, "monitor");
    }
    g_ptr_array_add(args, NULL);
    char *cmdline = win_quote_args((const char *const *) args->pdata);
    g_ptr_array_free(args, TRUE);

    char *log = session_file("session.log");
    wchar_t *wlog = win_utf16(log);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE fh = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wlog);
    g_free(log);

    STARTUPINFOW si = { .cb = sizeof si };
    si.lpDesktop = L"winsta0\\default";
    if (fh != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = fh;
        si.hStdError = fh;
    }
    PROCESS_INFORMATION pi = { 0 };
    wchar_t *wcmd = win_utf16(cmdline);
    wchar_t *env = child_environment();
    ok = CreateProcessAsUserW(primary, NULL, wcmd, NULL, NULL, fh != INVALID_HANDLE_VALUE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, NULL, &si, &pi);
    g_free(env);
    g_free(wcmd);
    g_free(cmdline);
    g_free(exe);
    if (fh != INVALID_HANDLE_VALUE)
        CloseHandle(fh);
    CloseHandle(primary);
    if (!ok)
        return FALSE;
    CloseHandle(pi.hThread);
    child = pi.hProcess;
    child_pid = pi.dwProcessId;
    child_session = session;
    return TRUE;
}

static void stop_child(void)
{
    if (!child)
        return;
    win_signal_quit(child_pid, 5000);
    WaitForSingleObject(child, 2000);
    CloseHandle(child);
    child = NULL;
    child_pid = 0;
    child_session = (DWORD) -1;
}

/* Satu putaran pengecekan: apakah proses berbagi layar harus berjalan, dan di sesi mana. */
static void reconcile(void)
{
    EnterCriticalSection(&lock);
    if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(child, &code);
        CloseHandle(child);
        child = NULL;
        child_pid = 0;
        child_session = (DWORD) -1;
        if (code == 0) {
            /* Keluar bersih: dihentikan dari panel atau `tsunagupad off`. Sama
             * seperti Restart=on-failure di systemd, tidak dinyalakan lagi
             * sampai sesi berikutnya. */
            suppressed = TRUE;
            manual = FALSE;
        } else {
            restart_at = g_get_monotonic_time() + (code == 3 ? 0 : 3 * G_USEC_PER_SEC);
        }
    }

    DWORD session = WTSGetActiveConsoleSessionId();
    gboolean want = (manual || is_enabled()) && !suppressed && session != 0xFFFFFFFF;
    if (child && (!want || child_session != session))
        stop_child();
    if (want && !child && g_get_monotonic_time() >= restart_at) {
        if (!start_child(session))
            restart_at = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    }
    LeaveCriticalSection(&lock);
}

static char *handle_command(const char *line)
{
    if (g_str_equal(line, "ping"))
        return g_strdup("ok");
    if (g_str_equal(line, "status")) {
        EnterCriticalSection(&lock);
        char *reply = g_strdup_printf("{\"enabled\":%s,\"share\":\"%s\",\"session\":%ld}",
                                      is_enabled() ? "true" : "false", child ? "running" : "stopped",
                                      child ? (long) child_session : -1L);
        LeaveCriticalSection(&lock);
        return reply;
    }
    if (g_str_equal(line, "reload")) {
        EnterCriticalSection(&lock);
        suppressed = FALSE;
        restart_at = 0;
        LeaveCriticalSection(&lock);
        SetEvent(wake_event);
        return g_strdup("ok");
    }
    if (g_str_equal(line, "share-off")) {
        EnterCriticalSection(&lock);
        suppressed = TRUE;
        manual = FALSE;
        stop_child();
        LeaveCriticalSection(&lock);
        return g_strdup("ok");
    }
    if (g_str_equal(line, "share-on") || g_str_has_prefix(line, "share-on ")) {
        char *token_path = session_file("token");
        gboolean have_token = g_file_test(token_path, G_FILE_TEST_EXISTS);
        g_free(token_path);
        if (!have_token)
            return g_strdup("error Nyalakan dulu \"iPad aktif di sesi berikutnya\" dari panel "
                            "Tsunagu-Pad (butuh administrator), supaya layanan punya kode aksesnya.");
        char *why = NULL;
        GPtrArray *args = filter_args(line + 8, &why);
        if (!args) {
            char *reply = g_strdup_printf("error %s", why);
            g_free(why);
            return reply;
        }
        EnterCriticalSection(&lock);
        if (manual_args)
            g_ptr_array_unref(manual_args);
        manual_args = args;
        manual = TRUE;
        suppressed = FALSE;
        restart_at = 0;
        stop_child();
        LeaveCriticalSection(&lock);
        SetEvent(wake_event);
        return g_strdup("ok");
    }
    if (g_str_has_prefix(line, "vdd-mode ")) {
        int w = 0, h = 0;
        char tail;
        if (sscanf(line + 9, "%d %d%c", &w, &h, &tail) != 2 || w < 320 || h < 240 || w > 7680 || h > 4320)
            return g_strdup("error ukuran tidak valid");
        GError *err = NULL;
        if (!vdd_add_mode(w & ~1, h & ~1, &err)) {
            char *reply = g_strdup_printf("error %s", err ? err->message : "gagal");
            g_clear_error(&err);
            return reply;
        }
        return g_strdup("ok");
    }
    return g_strdup("error perintah tidak dikenal");
}

static DWORD WINAPI pipe_thread(LPVOID unused)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(PIPE_SDDL, SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                       PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 4096, 4096, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(stop_event, 1000) == WAIT_OBJECT_0)
                break;
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
            CloseHandle(pipe);
            break;
        }
        if (connected) {
            char buf[1025];
            DWORD got = 0;
            if (ReadFile(pipe, buf, sizeof buf - 1, &got, NULL) && got > 0) {
                buf[got] = '\0';
                g_strchomp(buf);
                char *reply = g_utf8_validate(buf, -1, NULL) ? handle_command(buf) : g_strdup("error teks");
                DWORD written;
                WriteFile(pipe, reply, (DWORD) strlen(reply), &written, NULL);
                FlushFileBuffers(pipe);
                g_free(reply);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    if (sd)
        LocalFree(sd);
    return 0;
}

static DWORD WINAPI control_handler(DWORD control, DWORD type, LPVOID data, LPVOID context)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING);
        SetEvent(stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        /* Masuk, keluar, atau ganti pengguna: "sesi berikutnya" dimulai. */
        if (type == WTS_SESSION_LOGON || type == WTS_SESSION_LOGOFF || type == WTS_CONSOLE_CONNECT) {
            EnterCriticalSection(&lock);
            suppressed = FALSE;
            restart_at = 0;
            LeaveCriticalSection(&lock);
        }
        SetEvent(wake_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    status_handle = RegisterServiceCtrlHandlerExW(TSUNAGUPAD_SERVICE_NAME, control_handler, NULL);
    if (!status_handle)
        return;
    report(SERVICE_START_PENDING);
    InitializeCriticalSection(&lock);
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    secure_dirs();
    HANDLE pipe = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    report(SERVICE_RUNNING);

    for (;;) {
        reconcile();
        HANDLE waits[3] = { stop_event, wake_event, child ? child : wake_event };
        DWORD r = WaitForMultipleObjects(child ? 3 : 2, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0)
            break;
    }

    EnterCriticalSection(&lock);
    stop_child();
    LeaveCriticalSection(&lock);
    /* Membangunkan ConnectNamedPipe supaya thread pipa ikut selesai. */
    HANDLE poke = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (poke != INVALID_HANDLE_VALUE)
        CloseHandle(poke);
    if (pipe) {
        WaitForSingleObject(pipe, 3000);
        CloseHandle(pipe);
    }
    report(SERVICE_STOPPED);
}

/* ---------- klien ---------- */

gboolean win_service_request(const char *command, char **reply, GError **error)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 20 && pipe == INVALID_HANDLE_VALUE; tries++) {
        pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY && tries > 3)
                break;
            WaitNamedPipeW(PIPE_NAME, 500);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "layanan Tsunagu-Pad tidak berjalan");
        return FALSE;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);
    DWORD n;
    gboolean ok = WriteFile(pipe, command, (DWORD) strlen(command), &n, NULL);
    char buf[4097];
    DWORD got = 0;
    ok = ok && ReadFile(pipe, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(pipe);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "layanan Tsunagu-Pad tidak menjawab");
        return FALSE;
    }
    buf[got] = '\0';
    if (g_str_has_prefix(buf, "error")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", buf[5] ? buf + 6 : "gagal");
        return FALSE;
    }
    if (reply)
        *reply = g_strdup(buf);
    return TRUE;
}

gboolean win_service_installed(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return FALSE;
    SC_HANDLE svc = OpenServiceW(scm, TSUNAGUPAD_SERVICE_NAME, SERVICE_QUERY_STATUS);
    gboolean found = svc != NULL;
    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return found;
}

/* ---------- perintah ---------- */

static int need_admin(void)
{
    g_printerr("Perintah ini butuh hak administrator. Jalankan dari terminal \"Run as administrator\".\n");
    return 1;
}

static gboolean wait_state(SC_HANDLE svc, DWORD want)
{
    SERVICE_STATUS st;
    for (int i = 0; i < 100; i++) {
        if (!QueryServiceStatus(svc, &st))
            return FALSE;
        if (st.dwCurrentState == want)
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static int service_install(void)
{
    if (!win_is_elevated())
        return need_admin();
    secure_dirs();
    char *exe = win_exe_path();
    const char *argv[] = { exe, "service", "run", NULL };
    char *cmd = win_quote_args(argv);
    wchar_t *wcmd = win_utf16(cmd);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        g_free(exe);
        g_free(cmd);
        g_free(wcmd);
        return need_admin();
    }
    SC_HANDLE svc = OpenServiceW(scm, TSUNAGUPAD_SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             wcmd, NULL, NULL, NULL, NULL, NULL, L"Tsunagu-Pad");
    } else {
        svc = CreateServiceW(scm, TSUNAGUPAD_SERVICE_NAME, L"Tsunagu-Pad", SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             wcmd, NULL, NULL, NULL, NULL, NULL);
    }
    g_free(exe);
    g_free(cmd);
    g_free(wcmd);
    if (!svc) {
        char *msg = g_win32_error_message(GetLastError());
        g_printerr("Layanan tidak bisa dibuat: %s\n", msg);
        g_free(msg);
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_DESCRIPTIONW desc = {
        L"Menampilkan layar login dan sesi berikutnya di iPad bila sakelar \"iPad aktif di sesi "
        L"berikutnya\" dinyalakan di panel Tsunagu-Pad. Tidak melakukan apa pun bila sakelarnya mati."
    };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION actions[] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_NONE, 0 } };
    SERVICE_FAILURE_ACTIONSW failure = { 86400, NULL, NULL, G_N_ELEMENTS(actions), actions };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    StartServiceW(svc, 0, NULL);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    g_print("Layanan Tsunagu-Pad terpasang.\n");
    return 0;
}

static int service_control(gboolean start)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, TSUNAGUPAD_SERVICE_NAME,
                                       SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS) : NULL;
    if (!svc) {
        if (scm)
            CloseServiceHandle(scm);
        return need_admin();
    }
    SERVICE_STATUS st;
    BOOL ok = start ? StartServiceW(svc, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING
                    : ControlService(svc, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    if (ok)
        wait_state(svc, start ? SERVICE_RUNNING : SERVICE_STOPPED);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}

static int service_uninstall(void)
{
    if (!win_is_elevated())
        return need_admin();
    service_control(FALSE);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE svc = scm ? OpenServiceW(scm, TSUNAGUPAD_SERVICE_NAME, DELETE) : NULL;
    if (svc) {
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    if (scm)
        CloseServiceHandle(scm);
    char *flag = data_path("enabled");
    char *token = session_file("token");
    g_remove(flag);
    g_remove(token);
    g_free(flag);
    g_free(token);
    g_print("Layanan Tsunagu-Pad dihapus.\n");
    return 0;
}

static gboolean token_valid(const char *t)
{
    if (!t || strlen(t) < 8 || strlen(t) > 64)
        return FALSE;
    for (const char *p = t; *p; p++)
        if (!g_ascii_isalnum(*p))
            return FALSE;
    return TRUE;
}

static int service_enable(int argc, char **argv)
{
    if (!win_is_elevated())
        return need_admin();
    char *source = NULL;
    for (int i = 2; i < argc; i++)
        if (g_str_equal(argv[i], "--token-file") && i + 1 < argc)
            source = g_strdup(argv[++i]);
    if (!source)
        source = g_build_filename(g_get_user_config_dir(), "tsunagupad", "token", NULL);
    char *token = NULL;
    if (!g_file_get_contents(source, &token, NULL, NULL) || !token_valid(g_strstrip(token))) {
        g_printerr("Kode akses belum ada di %s; nyalakan Tsunagu-Pad sekali dulu.\n", source);
        g_free(source);
        g_free(token);
        return 1;
    }
    g_free(source);
    if (!secure_dirs()) {
        g_printerr("Folder data layanan tidak bisa disiapkan.\n");
        g_free(token);
        return 1;
    }
    /* Salinan kode akses yang sama dengan milik pengguna, supaya shortcut di
     * iPad tetap berlaku di layar login. */
    char *dest = session_file("token");
    char *flag = data_path("enabled");
    gboolean ok = g_file_set_contents(dest, token, -1, NULL) && g_file_set_contents(flag, "", 0, NULL);
    memset(token, 0, strlen(token));
    g_free(token);
    g_free(dest);
    g_free(flag);
    if (!ok) {
        g_printerr("Sakelar layanan tidak bisa disimpan.\n");
        return 1;
    }
    if (!win_service_installed())
        service_install();
    win_service_request("reload", NULL, NULL);
    g_print("iPad aktif di layar login dan sesi berikutnya.\n");
    return 0;
}

static int service_disable(void)
{
    if (!win_is_elevated())
        return need_admin();
    char *flag = data_path("enabled");
    char *token = session_file("token");
    g_remove(flag);
    g_remove(token);
    g_free(flag);
    g_free(token);
    win_service_request("share-off", NULL, NULL);
    g_print("iPad tidak lagi aktif di sesi berikutnya.\n");
    return 0;
}

static int service_status(void)
{
    gboolean installed = win_service_installed();
    char *reply = NULL;
    gboolean running = installed && win_service_request("status", &reply, NULL);
    char *flag = data_path("enabled");
    gboolean enabled = g_file_test(flag, G_FILE_TEST_EXISTS);
    g_free(flag);
    gboolean share = reply && strstr(reply, "\"running\"");
    tsunagu_event("service", "installed", installed ? "yes" : "no", "running", running ? "yes" : "no",
                  "enabled", enabled ? "yes" : "no", "share", share ? "running" : "stopped", NULL);
    g_free(reply);
    return 0;
}

int win_cmd_service(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (g_str_equal(what, "run")) {
        SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR) TSUNAGUPAD_SERVICE_NAME, service_main }, { NULL, NULL } };
        if (!StartServiceCtrlDispatcherW(table)) {
            g_printerr("Perintah ini dijalankan oleh Windows sebagai layanan, bukan dari terminal.\n");
            return 1;
        }
        return 0;
    }
    if (g_str_equal(what, "install"))
        return service_install();
    if (g_str_equal(what, "uninstall"))
        return service_uninstall();
    if (g_str_equal(what, "start"))
        return service_control(TRUE);
    if (g_str_equal(what, "stop"))
        return service_control(FALSE);
    if (g_str_equal(what, "enable"))
        return service_enable(argc, argv);
    if (g_str_equal(what, "disable"))
        return service_disable();
    if (g_str_equal(what, "status"))
        return service_status();
    g_printerr("Pemakaian: tsunagupad service install|uninstall|start|stop|enable [--token-file F]|disable|status\n");
    return 1;
}
