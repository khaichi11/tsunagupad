/* Pembantu pemasang Windows. Dijalankan pemasang (NSIS) dengan hak
 * administrator; tidak dipakai saat Tsunagu-Pad berjalan biasa.
 *
 *   tsunagupad setup display-install FOLDER  pasang driver layar kedua Tsunagu-Pad
 *                                           (katalognya sudah ditandatangani
 *                                           windows/display-install.ps1)
 *   tsunagupad setup display-remove          lepas driver itu beserta sertifikatnya
 *   tsunagupad setup vdd-remove              lepas Virtual Display Driver yang dulu
 *                                           dipasang Tsunagu-Pad 0.2.0
 *   tsunagupad setup path-add FOLDER | path-remove FOLDER
 *   tsunagupad setup firewall-add | firewall-remove
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <newdev.h>
#include <sddl.h>
#include <setupapi.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>

#include "win.h"

#define DISPLAY_HWID     L"Root\\TsunaguPadDisplay"
#define DISPLAY_INF      "TsunaguPadDisplay.inf"
#define DISPLAY_REG_KEY  L"SOFTWARE\\TsunaguPad\\Display"
#define DISPLAY_CERT_CN  L"Tsunagu-Pad Display Driver (local)"
#define VDD_HWID         L"Root\\MttVDD"
#define VDD_REG_KEY      L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver"
#define OUR_REG_KEY      L"SOFTWARE\\TsunaguPad"

/* Ukuran yang langsung tersedia: tiga pilihan panel (mendekati rasio iPad),
 * resolusi asli iPad yang umum, dan beberapa ukuran monitor biasa. Daftar yang
 * sama ada di driver sebagai cadangan bila registri kosong. */
static const int default_modes[][2] = {
    { 1280, 800 }, { 1640, 1024 }, { 2360, 1640 },
    { 1024, 768 }, { 1180, 820 }, { 1366, 1024 }, { 1194, 834 }, { 1080, 810 },
    { 2048, 1536 }, { 2160, 1620 }, { 2266, 1488 }, { 2388, 1668 }, { 2420, 1668 },
    { 2732, 2048 }, { 2752, 2064 }, { 2064, 2752 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

static void reg_set_dword(const wchar_t *key, const wchar_t *name, DWORD value)
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_DWORD, &value, sizeof value);
}

static DWORD reg_get_dword(const wchar_t *key, const wchar_t *name)
{
    DWORD value = 0, size = sizeof value;
    RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, &value, &size);
    return value;
}

/* ---------- daftar ukuran monitor virtual (registri) ---------- */

static gboolean has_mode(GArray *modes, int w, int h)
{
    for (guint i = 0; i < modes->len; i += 2)
        if (g_array_index(modes, int, i) == w && g_array_index(modes, int, i + 1) == h)
            return TRUE;
    return FALSE;
}

static void add_mode(GArray *modes, int w, int h)
{
    if (!has_mode(modes, w, h)) {
        g_array_append_val(modes, w);
        g_array_append_val(modes, h);
    }
}

static GArray *read_modes(void)
{
    GArray *modes = g_array_new(FALSE, FALSE, sizeof(int));
    wchar_t buf[4096];
    DWORD size = sizeof buf - 2 * sizeof(wchar_t);
    memset(buf, 0, sizeof buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, DISPLAY_REG_KEY, L"Modes", RRF_RT_REG_MULTI_SZ, NULL, buf, &size) ==
        ERROR_SUCCESS) {
        for (const wchar_t *p = buf; *p; p += wcslen(p) + 1) {
            int w = 0, h = 0;
            if (swscanf(p, L"%dx%d", &w, &h) == 2 && w >= 320 && h >= 240)
                add_mode(modes, w, h);
        }
    }
    return modes;
}

/* Menulis daftar ukuran: yang sudah ada, ukuran bawaan, lalu tambahan. */
static gboolean write_modes(int extra_w, int extra_h, GError **error)
{
    GArray *modes = read_modes();
    for (guint i = 0; i < G_N_ELEMENTS(default_modes); i++)
        add_mode(modes, default_modes[i][0], default_modes[i][1]);
    if (extra_w > 0)
        add_mode(modes, extra_w, extra_h);
    GString *list = g_string_new(NULL);
    for (guint i = 0; i < modes->len && i < 2 * 64; i += 2) {
        g_string_append_printf(list, "%dx%d", g_array_index(modes, int, i), g_array_index(modes, int, i + 1));
        g_string_append_c(list, '\n');
    }
    g_array_free(modes, TRUE);
    /* REG_MULTI_SZ: setiap ukuran diakhiri NUL, lalu satu NUL lagi. */
    wchar_t *w = win_utf16(list->str);
    for (wchar_t *p = w; *p; p++)
        if (*p == L'\n')
            *p = 0;
    DWORD bytes = (DWORD) ((list->len + 1) * sizeof(wchar_t));
    LONG r = RegSetKeyValueW(HKEY_LOCAL_MACHINE, DISPLAY_REG_KEY, L"Modes", REG_MULTI_SZ, w, bytes);
    g_free(w);
    g_string_free(list, TRUE);
    if (r != ERROR_SUCCESS) {
        char *msg = g_win32_error_message(r);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Daftar ukuran layar kedua tidak bisa ditulis: %s", msg);
        g_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* ---------- perangkat ---------- */

static gboolean multi_sz_has(const wchar_t *list, const wchar_t *want)
{
    for (const wchar_t *p = list; p && *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, want) == 0)
            return TRUE;
    return FALSE;
}

/* Memanggil fn untuk setiap perangkat layar dengan hardware ID itu. */
static guint for_each_device(const wchar_t *hwid, gboolean (*fn)(HDEVINFO, SP_DEVINFO_DATA *, gpointer),
                             gpointer data)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, 0);
    if (set == INVALID_HANDLE_VALUE)
        return 0;
    const wchar_t *short_id = wcschr(hwid, L'\\') ? wcschr(hwid, L'\\') + 1 : hwid;
    guint count = 0;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t ids[1024];
        memset(ids, 0, sizeof ids);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *) ids,
                                               sizeof ids - 2 * sizeof(wchar_t), NULL))
            continue;
        if (!multi_sz_has(ids, hwid) && !multi_sz_has(ids, short_id))
            continue;
        count++;
        if (fn && !fn(set, &dev, data))
            break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

static gboolean restart_one(HDEVINFO set, SP_DEVINFO_DATA *dev, gpointer data)
{
    SP_PROPCHANGE_PARAMS p;
    memset(&p, 0, sizeof p);
    p.ClassInstallHeader.cbSize = sizeof p.ClassInstallHeader;
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.StateChange = DICS_PROPCHANGE;
    p.Scope = DICS_FLAG_CONFIGSPECIFIC;
    if (SetupDiSetClassInstallParamsW(set, dev, &p.ClassInstallHeader, sizeof p))
        *(gboolean *) data = SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, dev) || *(gboolean *) data;
    return TRUE;
}

static gboolean remove_one(HDEVINFO set, SP_DEVINFO_DATA *dev, gpointer data)
{
    GPtrArray *infs = data;
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (key != INVALID_HANDLE_VALUE) {
        wchar_t inf[MAX_PATH];
        DWORD size = sizeof inf - sizeof(wchar_t);
        memset(inf, 0, sizeof inf);
        if (RegQueryValueExW(key, L"InfPath", NULL, NULL, (BYTE *) inf, &size) == ERROR_SUCCESS && inf[0])
            g_ptr_array_add(infs, g_memdup2(inf, sizeof inf));
        RegCloseKey(key);
    }
    SetupDiCallClassInstaller(DIF_REMOVE, set, dev);
    return TRUE;
}

/* Ukuran baru untuk monitor virtual: dicatat di registri, lalu drivernya
 * dimuat ulang supaya membacanya. Dipanggil layanan Tsunagu-Pad. */
gboolean display_add_mode(int width, int height, GError **error)
{
    if (!write_modes(width, height, error))
        return FALSE;
    gboolean ok = FALSE;
    for_each_device(DISPLAY_HWID, restart_one, &ok);
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Driver layar kedua tidak bisa dimuat ulang");
    return ok;
}

/* ---------- driver layar kedua Tsunagu-Pad ---------- */

static int display_install(const char *driver_dir)
{
    char *inf = g_build_filename(driver_dir, DISPLAY_INF, NULL);
    if (!g_file_test(inf, G_FILE_TEST_IS_REGULAR)) {
        g_printerr("%s tidak ditemukan di %s\n", DISPLAY_INF, driver_dir);
        g_free(inf);
        return 1;
    }
    GError *err = NULL;
    if (!write_modes(0, 0, &err)) {
        g_printerr("%s\n", err->message);
        g_clear_error(&err);
    }

    /* Perangkat root "Root\TsunaguPadDisplay" dibuat dulu, lalu drivernya
     * dipasang ke perangkat itu (cara yang sama dengan devcon). */
    if (for_each_device(DISPLAY_HWID, NULL, NULL) == 0) {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, NULL);
        SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
        static const wchar_t hwid[] = DISPLAY_HWID L"\0";
        gboolean created = set != INVALID_HANDLE_VALUE &&
            SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, NULL, NULL, DICD_GENERATE_ID, &dev) &&
            SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE *) hwid, sizeof hwid) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
        DWORD create_error = GetLastError();
        if (set != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(set);
        if (!created) {
            char *msg = g_win32_error_message(create_error);
            g_printerr("Perangkat layar kedua tidak bisa dibuat: %s\n", msg);
            g_free(msg);
            g_free(inf);
            return 1;
        }
    }
    wchar_t *winf = win_utf16(inf);
    BOOL reboot = FALSE;
    BOOL ok = UpdateDriverForPlugAndPlayDevicesW(NULL, DISPLAY_HWID, winf, INSTALLFLAG_FORCE, &reboot);
    DWORD e = GetLastError();
    g_free(winf);
    g_free(inf);
    if (!ok) {
        char *msg = g_win32_error_message(e);
        g_printerr("Driver layar kedua tidak terpasang (0x%08lx): %s\n", (unsigned long) e, msg);
        g_free(msg);
        return 1;
    }
    reg_set_dword(OUR_REG_KEY, L"DisplayInstalled", 1);

    /* Windows biasanya langsung memperluas desktop ke monitor baru. Monitor
     * virtual baru dipakai saat iPad dijadikan layar kedua, jadi dilepas dulu. */
    gboolean seen = FALSE;
    for (int i = 0; i < 75 && !(seen = vdisp_available()); i++)
        Sleep(200);
    Sleep(1500);
    vdisp_detach_all();
    g_print("Driver layar kedua terpasang%s%s.\n", seen ? "" : " (monitornya belum muncul)",
            reboot ? ", perlu restart" : "");
    return reboot ? 3010 : 0;
}

/* Paket driver kita di Driver Store (oemNN.inf), juga yang tidak lagi
 * terhubung ke perangkat mana pun. */
static void remove_driver_packages(void)
{
    char *inf_dir = g_build_filename(g_getenv("SystemRoot") ? g_getenv("SystemRoot") : "C:\\Windows", "INF", NULL);
    GDir *dir = g_dir_open(inf_dir, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "oem") || !g_str_has_suffix(name, ".inf"))
            continue;
        char *path = g_build_filename(inf_dir, name, NULL);
        char *data = NULL;
        gsize len = 0;
        gboolean ours = FALSE;
        if (g_file_get_contents(path, &data, &len, NULL)) {
            /* Berkas INF bisa ASCII atau UTF-16. */
            ours = g_strstr_len(data, len, "TsunaguPadDisplay") != NULL;
            if (!ours && len > 2) {
                char *utf8 = g_utf16_to_utf8((const gunichar2 *) data, (glong) (len / 2), NULL, NULL, NULL);
                ours = utf8 && strstr(utf8, "TsunaguPadDisplay");
                g_free(utf8);
            }
        }
        if (ours) {
            wchar_t *w = win_utf16(name);
            if (SetupUninstallOEMInfW(w, SUOI_FORCEDELETE, NULL))
                g_print("Paket driver %s dihapus.\n", name);
            g_free(w);
        }
        g_free(data);
        g_free(path);
    }
    if (dir)
        g_dir_close(dir);
    g_free(inf_dir);
}

/* Sertifikat sekali pakai dari display-install.ps1 (kunci rahasianya sudah
 * dihapus sejak awal) dikeluarkan dari daftar tepercaya. */
static void remove_certificates(void)
{
    static const wchar_t *const stores[] = { L"Root", L"TrustedPublisher", L"My" };
    for (guint i = 0; i < G_N_ELEMENTS(stores); i++) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                         CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, stores[i]);
        if (!store)
            continue;
        PCCERT_CONTEXT cert = NULL;
        while ((cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                  CERT_FIND_SUBJECT_STR_W, DISPLAY_CERT_CN, NULL))) {
            /* CertDeleteCertificateFromStore juga melepas konteksnya. */
            if (!CertDeleteCertificateFromStore(cert))
                break;
            char *store_name = win_utf8(stores[i]);
            g_print("Sertifikat driver layar kedua dihapus dari %s.\n", store_name);
            g_free(store_name);
        }
        CertCloseStore(store, 0);
    }
}

static int display_remove(void)
{
    vdisp_detach_all();
    GPtrArray *infs = g_ptr_array_new_with_free_func(g_free);
    for_each_device(DISPLAY_HWID, remove_one, infs);
    for (guint i = 0; i < infs->len; i++)
        SetupUninstallOEMInfW(infs->pdata[i], SUOI_FORCEDELETE, NULL);
    g_ptr_array_unref(infs);
    remove_driver_packages();
    remove_certificates();
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, OUR_REG_KEY, L"DisplayInstalled");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, DISPLAY_REG_KEY);
    g_print("Driver layar kedua dilepas.\n");
    return 0;
}

/* ---------- Virtual Display Driver dari versi 0.2.0 ---------- */

static int vdd_remove(void)
{
    /* Driver yang dipasang sendiri oleh pengguna sebelum Tsunagu-Pad dibiarkan. */
    if (!reg_get_dword(OUR_REG_KEY, L"VddInstalledByTsunaguPad")) {
        g_print("Virtual Display Driver tidak dipasang oleh Tsunagu-Pad; dibiarkan.\n");
        return 0;
    }
    vdisp_detach_all();
    GPtrArray *infs = g_ptr_array_new_with_free_func(g_free);
    for_each_device(VDD_HWID, remove_one, infs);
    for (guint i = 0; i < infs->len; i++)
        SetupUninstallOEMInfW(infs->pdata[i], SUOI_FORCEDELETE, NULL);
    g_ptr_array_unref(infs);
    char *vdd_dir = g_build_filename(win_program_data_dir(), "vdd", NULL);
    char *settings = g_build_filename(vdd_dir, "vdd_settings.xml", NULL);
    g_remove(settings);
    g_rmdir(vdd_dir);
    g_free(settings);
    g_free(vdd_dir);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, OUR_REG_KEY, L"VddInstalledByTsunaguPad");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, L"VDDPATH");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, VDD_REG_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech");   /* hanya bila sudah kosong */
    g_print("Virtual Display Driver dilepas.\n");
    return 0;
}

/* ---------- PATH ---------- */

#define ENV_KEY L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"

static int path_edit(const char *dir, gboolean add)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENV_KEY, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) {
        g_printerr("PATH sistem tidak bisa dibuka (butuh administrator).\n");
        return 1;
    }
    DWORD type = REG_EXPAND_SZ, size = 0;
    RegQueryValueExW(key, L"Path", NULL, &type, NULL, &size);
    wchar_t *value = g_malloc0(size + 2 * sizeof(wchar_t));
    RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *) value, &size);
    char *current = win_utf8(value);
    g_free(value);

    char **parts = g_strsplit(current, ";", -1);
    GString *out = g_string_new(NULL);
    gboolean present = FALSE;
    for (char **p = parts; *p; p++) {
        if (!**p)
            continue;
        if (g_ascii_strcasecmp(*p, dir) == 0) {
            present = TRUE;
            if (!add)
                continue;
        }
        if (out->len)
            g_string_append_c(out, ';');
        g_string_append(out, *p);
    }
    if (add && !present) {
        if (out->len)
            g_string_append_c(out, ';');
        g_string_append(out, dir);
    }
    wchar_t *wout = win_utf16(out->str);
    LONG r = RegSetValueExW(key, L"Path", 0, type == REG_SZ ? REG_SZ : REG_EXPAND_SZ, (const BYTE *) wout,
                            (DWORD) ((wcslen(wout) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    g_free(wout);
    g_string_free(out, TRUE);
    g_strfreev(parts);
    g_free(current);
    DWORD_PTR result;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) L"Environment", SMTO_ABORTIFHUNG, 3000, &result);
    return r == ERROR_SUCCESS ? 0 : 1;
}

/* ---------- firewall ---------- */

/* Hanya subnet lokal dan Tailscale (100.64.0.0/10, fd7a:115c:a1e0::/48) yang
 * diizinkan masuk; daemon sendiri juga menolak alamat publik. */
#define REMOTE_SCOPE "LocalSubnet,100.64.0.0/10,fd7a:115c:a1e0::/48"

static char *netsh_path(void)
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, G_N_ELEMENTS(dir));
    char *sys = n ? win_utf8(dir) : g_strdup("C:\\Windows\\System32");
    char *path = g_build_filename(sys, "netsh.exe", NULL);
    g_free(sys);
    return path;
}

static void netsh(const char *const *args)
{
    char *exe = netsh_path();
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, exe);
    for (guint i = 0; args[i]; i++)
        g_ptr_array_add(argv, (char *) args[i]);
    g_ptr_array_add(argv, NULL);
    win_run((const char *const *) argv->pdata, NULL, NULL, NULL, NULL);
    g_ptr_array_free(argv, TRUE);
    g_free(exe);
}

static void firewall_rule(const char *name, const char *program)
{
    char *n = g_strdup_printf("name=%s", name);
    char *p = g_strdup_printf("program=%s", program);
    const char *del[] = { "advfirewall", "firewall", "delete", "rule", n, NULL };
    netsh(del);
    const char *add[] = { "advfirewall", "firewall", "add", "rule", n, "dir=in", "action=allow", p,
                          "enable=yes", "profile=any", "remoteip=" REMOTE_SCOPE, NULL };
    netsh(add);
    g_free(n);
    g_free(p);
}

static int firewall(gboolean add)
{
    char *exe = win_exe_path();
    char *dir = g_path_get_dirname(exe);
    char *uxplay = g_build_filename(dir, "uxplay.exe", NULL);
    if (add) {
        firewall_rule("Tsunagu-Pad", exe);
        firewall_rule("Tsunagu-Pad AirPlay", uxplay);
    } else {
        const char *a[] = { "advfirewall", "firewall", "delete", "rule", "name=Tsunagu-Pad", NULL };
        const char *b[] = { "advfirewall", "firewall", "delete", "rule", "name=Tsunagu-Pad AirPlay", NULL };
        netsh(a);
        netsh(b);
    }
    g_free(uxplay);
    g_free(dir);
    g_free(exe);
    return 0;
}

int win_cmd_setup(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (!win_is_elevated()) {
        g_printerr("Perintah setup dijalankan oleh pemasang dengan hak administrator.\n");
        return 1;
    }
    if (g_str_equal(what, "display-install") && argc > 2)
        return display_install(argv[2]);
    if (g_str_equal(what, "display-remove"))
        return display_remove();
    if (g_str_equal(what, "vdd-remove"))
        return vdd_remove();
    if (g_str_equal(what, "path-add") && argc > 2)
        return path_edit(argv[2], TRUE);
    if (g_str_equal(what, "path-remove") && argc > 2)
        return path_edit(argv[2], FALSE);
    if (g_str_equal(what, "firewall-add"))
        return firewall(TRUE);
    if (g_str_equal(what, "firewall-remove"))
        return firewall(FALSE);
    g_printerr("Pemakaian: tsunagupad setup display-install FOLDER | display-remove | vdd-remove | "
               "path-add FOLDER | path-remove FOLDER | firewall-add | firewall-remove\n");
    return 1;
}
