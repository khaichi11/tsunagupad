/* iPad -> Windows: menjalankan UxPlay (penerima AirPlay open source, GPL)
 * yang ikut terpasang bersama Tsunagu-Pad.
 *
 * UxPlay versi ini membawa penyiar mDNS sendiri, jadi Bonjour dari Apple
 * tidak perlu dipasang. Windows tidak punya execv yang menggantikan proses,
 * jadi UxPlay dijalankan sebagai anak proses di dalam job object: bila proses
 * ini dihentikan dengan cara apa pun, UxPlay ikut berhenti.
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <psapi.h>
#include <string.h>
#include <tlhelp32.h>

#include "win.h"

static HANDLE child;

/* UxPlay yang tertinggal (mis. setelah panel ditutup paksa) masih memegang
 * port AirPlay dan nama penerimanya; dihentikan dulu sebelum mulai. Hanya
 * UxPlay milik Tsunagu-Pad sendiri yang disentuh. */
static void stop_stale_uxplay(const char *uxplay)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W e = { .dwSize = sizeof e };
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"uxplay.exe") != 0)
            continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                               FALSE, e.th32ProcessID);
        if (!p)
            continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = G_N_ELEMENTS(path);
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            char *utf8 = win_utf8(path);
            if (g_ascii_strcasecmp(utf8, uxplay) == 0 && TerminateProcess(p, 1))
                WaitForSingleObject(p, 3000);
            g_free(utf8);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    /* Ctrl+C, jendela ditutup, atau logoff: UxPlay menerima sinyal yang sama
     * dari konsol bersama dan berhenti sendiri. Proses ini menunggunya. */
    return TRUE;
}

static DWORD WINAPI quit_thread(LPVOID event)
{
    WaitForSingleObject(event, INFINITE);
    /* Berhenti dengan rapi: UxPlay mengirim kabar "pamit" mDNS supaya
     * namanya langsung hilang dari daftar Screen Mirroring di iPad. */
    if (GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) && WaitForSingleObject(child, 3000) == WAIT_OBJECT_0)
        return 0;
    TerminateProcess(child, 0);
    return 0;
}

int win_cmd_receive(const char *name, int fps, gboolean fullscreen, gboolean pin, gboolean no_audio)
{
    char *uxplay = win_find_uxplay();
    if (!uxplay) {
        tsunagu_event("error", "message", "UxPlay tidak ditemukan. Pasang ulang Tsunagu-Pad.", NULL);
        g_printerr("tsunagupad: UxPlay tidak ditemukan. Pasang ulang Tsunagu-Pad.\n");
        return 1;
    }
    stop_stale_uxplay(uxplay);

    char *fps_str = g_strdup_printf("%d", CLAMP(fps, 10, 60));
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, uxplay);
    g_ptr_array_add(args, "-n");
    g_ptr_array_add(args, (char *) name);
    g_ptr_array_add(args, "-nh");
    g_ptr_array_add(args, "-p");
    g_ptr_array_add(args, "-fps");
    g_ptr_array_add(args, fps_str);
    g_ptr_array_add(args, "-vsync");
    g_ptr_array_add(args, "no");
    g_ptr_array_add(args, "-nohold");
    /* Sama seperti di Linux: jeda dari iPad (layar dikunci, pindah aplikasi)
     * bukan tanda sesi mati, dan jendelanya tetap ada untuk sambungan berikutnya. */
    g_ptr_array_add(args, "-reset");
    g_ptr_array_add(args, "0");
    g_ptr_array_add(args, "-nc");
    /* Direct3D 11: dekoder perangkat keras dan jendela biasa yang bisa
     * dijadikan layar penuh (juga dengan Alt+Enter). */
    g_ptr_array_add(args, "-vs");
    g_ptr_array_add(args, "d3d11videosink");
    if (fullscreen)
        g_ptr_array_add(args, "-fs");
    if (pin)
        g_ptr_array_add(args, "-pin");
    if (no_audio)
        g_ptr_array_add(args, "-a");
    g_ptr_array_add(args, NULL);
    char *cmdline = win_quote_args((const char *const *) args->pdata);
    g_ptr_array_free(args, TRUE);
    g_free(fps_str);

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
        memset(&limit, 0, sizeof limit);
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    }

    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    wchar_t *wcmd = win_utf16(cmdline);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    g_free(wcmd);
    g_free(cmdline);
    if (!ok) {
        char *msg = g_win32_error_message(GetLastError());
        tsunagu_event("error", "message", msg, NULL);
        g_printerr("tsunagupad: UxPlay gagal dijalankan: %s\n", msg);
        g_free(msg);
        g_free(uxplay);
        return 1;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    child = pi.hProcess;
    tsunagu_event("receiver", "state", "starting", "name", name, NULL);

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    HANDLE quit = win_create_quit_event();
    if (quit) {
        HANDLE t = CreateThread(NULL, 0, quit_thread, quit, 0, NULL);
        if (t)
            CloseHandle(t);
    }

    WaitForSingleObject(child, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    if (job)
        CloseHandle(job);
    g_free(uxplay);
    return (int) code;
}
