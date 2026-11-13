/* Pembantu pemasang Windows. Dijalankan pemasang (NSIS) dengan hak
 * administrator; tidak dipakai saat Tsunagu-Pad berjalan biasa.
 *
 *   tsunagupad setup vdd-install FOLDER_DRIVER   pasang driver layar virtual
 *   tsunagupad setup vdd-remove                  lepas driver yang dipasang Tsunagu-Pad
 *   tsunagupad setup path-add FOLDER | path-remove FOLDER
 *   tsunagupad setup firewall-add | firewall-remove
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <aclapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <newdev.h>
#include <sddl.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

#include "win.h"

#define VDD_HWID       L"Root\\MttVDD"
#define VDD_REG_KEY    L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver"
#define OUR_REG_KEY    L"SOFTWARE\\TsunaguPad"
/* Folder pengaturan driver: driver (LOCAL SERVICE) menulis berkasnya sendiri,
 * pengguna biasa hanya membaca. */
#define VDD_DIR_SDDL   L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGWGX;;;LS)(A;OICI;GRGX;;;BU)"

/* Ukuran yang langsung tersedia: tiga pilihan panel (mendekati rasio iPad),
 * resolusi asli iPad yang umum, dan beberapa ukuran monitor biasa. */
static const int default_modes[][2] = {
    { 1280, 800 }, { 1640, 1024 }, { 2360, 1640 },
    { 1024, 768 }, { 1180, 820 }, { 1366, 1024 }, { 1194, 834 }, { 1080, 810 },
    { 2048, 1536 }, { 2160, 1620 }, { 2266, 1488 }, { 2388, 1668 }, { 2420, 1668 },
    { 2732, 2048 }, { 2752, 2064 }, { 2064, 2752 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

/* ---------- pengaturan driver layar virtual ---------- */

static char *vdd_dir(void)
{
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof buf - sizeof(wchar_t);
    memset(buf, 0, sizeof buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, L"VDDPATH", RRF_RT_REG_SZ, NULL, buf, &size) == ERROR_SUCCESS &&
        buf[0])
        return win_utf8(buf);
    return g_strdup("C:\\VirtualDisplayDriver");
}

static char *vdd_settings_xml(GArray *modes)
{
    GString *x = g_string_new(
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<!-- Ditulis oleh Tsunagu-Pad. Ukuran baru ditambahkan sendiri bila diminta lewat --second. -->\n"
        "<vdd_settings>\n"
        "    <monitors>\n        <count>1</count>\n    </monitors>\n"
        "    <gpu>\n        <friendlyname>default</friendlyname>\n    </gpu>\n"
        "    <global>\n        <g_refresh_rate>60</g_refresh_rate>\n    </global>\n"
        "    <resolutions>\n");
    for (guint i = 0; i < modes->len; i += 2)
        g_string_append_printf(x,
            "        <resolution>\n            <width>%d</width>\n            <height>%d</height>\n"
            "            <refresh_rate>60</refresh_rate>\n        </resolution>\n",
            g_array_index(modes, int, i), g_array_index(modes, int, i + 1));
    g_string_append(x,
        "    </resolutions>\n"
        "    <options>\n"
        "        <CustomEdid>false</CustomEdid>\n        <PreventSpoof>false</PreventSpoof>\n"
        "        <EdidCeaOverride>false</EdidCeaOverride>\n        <HardwareCursor>true</HardwareCursor>\n"
        "        <SDR10bit>false</SDR10bit>\n        <HDRPlus>false</HDRPlus>\n"
        "        <logging>false</logging>\n        <debuglogging>false</debuglogging>\n"
        "    </options>\n"
        "</vdd_settings>\n");
    return g_string_free(x, FALSE);
}

/* Ukuran yang sudah tercantum di berkas pengaturan yang ada. */
static GArray *read_modes(const char *xml)
{
    GArray *modes = g_array_new(FALSE, FALSE, sizeof(int));
    GRegex *re = g_regex_new("<width>\\s*(\\d+)\\s*</width>\\s*<height>\\s*(\\d+)\\s*</height>",
                             G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *m = NULL;
    g_regex_match(re, xml ? xml : "", 0, &m);
    while (g_match_info_matches(m)) {
        char *w = g_match_info_fetch(m, 1), *h = g_match_info_fetch(m, 2);
        int iw = atoi(w), ih = atoi(h);
        g_array_append_val(modes, iw);
        g_array_append_val(modes, ih);
        g_free(w);
        g_free(h);
        g_match_info_next(m, NULL);
    }
    g_match_info_free(m);
    g_regex_unref(re);
    return modes;
}

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

static gboolean write_settings(GArray *extra, GError **error)
{
    char *dir = vdd_dir();
    char *path = g_build_filename(dir, "vdd_settings.xml", NULL);
    char *old = NULL;
    g_file_get_contents(path, &old, NULL, NULL);
    GArray *modes = read_modes(old);
    for (guint i = 0; i < G_N_ELEMENTS(default_modes); i++)
        add_mode(modes, default_modes[i][0], default_modes[i][1]);
    for (guint i = 0; extra && i < extra->len; i += 2)
        add_mode(modes, g_array_index(extra, int, i), g_array_index(extra, int, i + 1));
    char *xml = vdd_settings_xml(modes);
    g_mkdir_with_parents(dir, 0755);
    gboolean ok = g_file_set_contents(path, xml, -1, error);
    g_free(xml);
    g_free(old);
    g_array_free(modes, TRUE);
    g_free(path);
    g_free(dir);
    return ok;
}

/* ---------- perangkat ---------- */

static gboolean multi_sz_has(const wchar_t *list, const wchar_t *want)
{
    for (const wchar_t *p = list; p && *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, want) == 0)
            return TRUE;
    return FALSE;
}

/* Memanggil fn untuk setiap perangkat layar virtual yang terpasang. */
static guint for_each_vdd(gboolean (*fn)(HDEVINFO, SP_DEVINFO_DATA *, gpointer), gpointer data)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, 0);
    if (set == INVALID_HANDLE_VALUE)
        return 0;
    guint count = 0;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t ids[1024];
        memset(ids, 0, sizeof ids);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *) ids,
                                               sizeof ids - 2 * sizeof(wchar_t), NULL))
            continue;
        if (!multi_sz_has(ids, VDD_HWID) && !multi_sz_has(ids, L"MttVDD"))
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

/* Memuat ulang driver supaya berkas pengaturannya dibaca lagi. */
gboolean vdd_restart_device(void)
{
    gboolean ok = FALSE;
    for_each_vdd(restart_one, &ok);
    return ok;
}

gboolean vdd_add_mode(int width, int height, GError **error)
{
    GArray *extra = g_array_new(FALSE, FALSE, sizeof(int));
    g_array_append_val(extra, width);
    g_array_append_val(extra, height);
    gboolean ok = write_settings(extra, error);
    g_array_free(extra, TRUE);
    if (ok && !vdd_restart_device()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Driver layar virtual tidak bisa dimuat ulang");
        ok = FALSE;
    }
    return ok;
}

static gboolean set_dir_acl(const char *path)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(VDD_DIR_SDDL, SDDL_REVISION_1, &sd, NULL))
        return FALSE;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = NULL;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    wchar_t *w = win_utf16(path);
    DWORD r = SetNamedSecurityInfoW(w, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL);
    g_free(w);
    LocalFree(sd);
    return r == ERROR_SUCCESS;
}

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

static int vdd_install(const char *driver_dir)
{
    char *inf = g_build_filename(driver_dir, "MttVDD.inf", NULL);
    if (!g_file_test(inf, G_FILE_TEST_IS_REGULAR)) {
        g_printerr("MttVDD.inf tidak ditemukan di %s\n", driver_dir);
        g_free(inf);
        return 1;
    }

    /* Pengaturan driver ditaruh di folder Tsunagu-Pad, kecuali driver ini
     * sudah dipasang sendiri sebelumnya dengan folder lain. */
    gboolean existing = for_each_vdd(NULL, NULL) > 0;
    if (!existing) {
        char *base = win_program_data_dir();
        char *dir = g_build_filename(base, "vdd", NULL);
        g_mkdir_with_parents(dir, 0755);
        set_dir_acl(dir);
        wchar_t *wdir = win_utf16(dir);
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, L"VDDPATH", REG_SZ, wdir,
                        (DWORD) ((wcslen(wdir) + 1) * sizeof(wchar_t)));
        g_free(wdir);
        g_free(dir);
        g_free(base);
    }
    GError *err = NULL;
    if (!write_settings(NULL, &err)) {
        g_printerr("Pengaturan driver layar virtual tidak bisa ditulis: %s\n", err->message);
        g_clear_error(&err);
    }

    if (!existing) {
        /* Perangkat root "Root\MttVDD" dibuat dulu, lalu drivernya dipasang
         * ke perangkat itu (cara yang sama dengan devcon/nefcon). */
        HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, NULL);
        SP_DEVINFO_DATA dev = { .cbSize = sizeof dev };
        static const wchar_t hwid[] = VDD_HWID L"\0";
        gboolean created = set != INVALID_HANDLE_VALUE &&
            SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, NULL, NULL, DICD_GENERATE_ID, &dev) &&
            SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE *) hwid, sizeof hwid) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
        if (set != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(set);
        if (!created) {
            char *msg = g_win32_error_message(GetLastError());
            g_printerr("Perangkat layar virtual tidak bisa dibuat: %s\n", msg);
            g_free(msg);
            g_free(inf);
            return 1;
        }
    }
    wchar_t *winf = win_utf16(inf);
    BOOL reboot = FALSE;
    BOOL ok = UpdateDriverForPlugAndPlayDevicesW(NULL, VDD_HWID, winf, INSTALLFLAG_FORCE, &reboot);
    g_free(winf);
    g_free(inf);
    if (!ok) {
        DWORD e = GetLastError();
        char *msg = g_win32_error_message(e);
        g_printerr("Driver layar virtual tidak terpasang: %s\n", msg);
        g_free(msg);
        return 1;
    }
    if (!existing)
        reg_set_dword(OUR_REG_KEY, L"VddInstalledByTsunaguPad", 1);
    else
        vdd_restart_device();

    /* Windows biasanya langsung memperluas desktop ke monitor baru. Monitor
     * virtual baru dipakai saat iPad dijadikan layar kedua, jadi dilepas dulu. */
    for (int i = 0; i < 50 && !vdisp_available(); i++)
        Sleep(200);
    Sleep(1500);
    vdisp_detach_all();
    g_print("Driver layar virtual terpasang%s.\n", reboot ? " (perlu restart)" : "");
    return reboot ? 3010 : 0;
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

static int vdd_remove(void)
{
    /* Driver yang dipasang sendiri oleh pengguna sebelum Tsunagu-Pad dibiarkan. */
    if (!reg_get_dword(OUR_REG_KEY, L"VddInstalledByTsunaguPad")) {
        g_print("Driver layar virtual tidak dipasang oleh Tsunagu-Pad; dibiarkan.\n");
        return 0;
    }
    vdisp_detach_all();
    GPtrArray *infs = g_ptr_array_new_with_free_func(g_free);
    for_each_vdd(remove_one, infs);
    for (guint i = 0; i < infs->len; i++)
        SetupUninstallOEMInfW(infs->pdata[i], SUOI_FORCEDELETE, NULL);
    g_ptr_array_unref(infs);
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, OUR_REG_KEY, L"VddInstalledByTsunaguPad");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, L"VDDPATH");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, VDD_REG_KEY);
    g_print("Driver layar virtual dilepas.\n");
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
    if (g_str_equal(what, "vdd-install") && argc > 2)
        return vdd_install(argv[2]);
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
    g_printerr("Pemakaian: tsunagupad setup vdd-install FOLDER | vdd-remove | path-add FOLDER | "
               "path-remove FOLDER | firewall-add | firewall-remove\n");
    return 1;
}
