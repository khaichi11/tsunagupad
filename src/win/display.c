/* Tsunagu-Pad untuk Windows: monitor dan layar kedua.
 *
 * Layar kedua memakai Virtual Display Driver (IddCx, lisensi MIT) yang
 * dipasang pemasang Tsunagu-Pad. Driver itu selalu menyediakan satu monitor
 * virtual; di sini monitor itu disambungkan ke desktop saat iPad dipakai
 * sebagai layar kedua, diberi ukuran, letak, dan skala, lalu dilepas lagi
 * saat selesai. Semuanya lewat API konfigurasi tampilan Windows (CCD), yang
 * boleh dipakai pengguna biasa tanpa hak administrator.
 *
 * Seperti di GNOME, letak yang diatur pengguna di Pengaturan Tampilan dicatat
 * sendiri (relatif terhadap monitor utama) dan dipasang lagi di sesi berikutnya.
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#define COBJMACROS
#include <initguid.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

/* EDID driver layar virtual: pabrikan "MTT", produk 0x1337, nama "VDD by MTT". */
#define VDD_EDID_ID   "MTT1337"
#define VDD_EDID_NAME "VDD by MTT"

struct _Vdisp {
    char device[64];
    int width, height;
    gboolean was_active;    /* monitor virtual sudah menyala sebelum kita mulai */
};

/* ---------- konfigurasi tampilan (CCD) ---------- */

typedef struct {
    UINT32 npaths, nmodes;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
} Config;

static void config_clear(Config *c)
{
    g_free(c->paths);
    g_free(c->modes);
    memset(c, 0, sizeof *c);
}

static gboolean config_query(UINT32 flags, Config *c)
{
    memset(c, 0, sizeof *c);
    for (int tries = 0; tries < 5; tries++) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS)
            return FALSE;
        c->paths = g_new0(DISPLAYCONFIG_PATH_INFO, MAX(np, 1));
        c->modes = g_new0(DISPLAYCONFIG_MODE_INFO, MAX(nm, 1));
        LONG r = QueryDisplayConfig(flags, &np, c->paths, &nm, c->modes, NULL);
        if (r == ERROR_SUCCESS) {
            c->npaths = np;
            c->nmodes = nm;
            return TRUE;
        }
        config_clear(c);
        if (r != ERROR_INSUFFICIENT_BUFFER)
            return FALSE;
    }
    return FALSE;
}

typedef struct {
    char name[128];
    gboolean is_vdd, internal;
} TargetInfo;

static gboolean target_info(LUID adapter, UINT32 id, TargetInfo *out)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME t;
    memset(&t, 0, sizeof t);
    t.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    t.header.size = sizeof t;
    t.header.adapterId = adapter;
    t.header.id = id;
    memset(out, 0, sizeof *out);
    if (DisplayConfigGetDeviceInfo(&t.header) != ERROR_SUCCESS)
        return FALSE;
    char *friendly = win_utf8(t.monitorFriendlyDeviceName);
    char *path = win_utf8(t.monitorDevicePath);
    char *upper = g_ascii_strup(path, -1);
    g_strlcpy(out->name, friendly, sizeof out->name);
    out->is_vdd = strstr(upper, VDD_EDID_ID) != NULL || g_str_has_prefix(friendly, VDD_EDID_NAME);
    out->internal = t.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                    t.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                    t.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
    g_free(friendly);
    g_free(path);
    g_free(upper);
    return TRUE;
}

static gboolean source_gdi(LUID adapter, UINT32 id, char out[64])
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME s;
    memset(&s, 0, sizeof s);
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    s.header.size = sizeof s;
    s.header.adapterId = adapter;
    s.header.id = id;
    if (DisplayConfigGetDeviceInfo(&s.header) != ERROR_SUCCESS)
        return FALSE;
    char *name = win_utf8(s.viewGdiDeviceName);
    g_strlcpy(out, name, 64);
    g_free(name);
    return TRUE;
}

static gboolean same_luid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static gboolean path_is_vdd(const DISPLAYCONFIG_PATH_INFO *p)
{
    TargetInfo t;
    return target_info(p->targetInfo.adapterId, p->targetInfo.id, &t) && t.is_vdd;
}

/* Menerapkan jalur yang diberikan dengan mode yang masih dirujuk saja. */
static LONG config_apply(DISPLAYCONFIG_PATH_INFO *paths, UINT32 n, const Config *from)
{
    DISPLAYCONFIG_MODE_INFO *modes = g_new0(DISPLAYCONFIG_MODE_INFO, MAX(from->nmodes, 1));
    UINT32 nm = 0;
    for (UINT32 i = 0; i < n; i++) {
        UINT32 *idx[] = { &paths[i].sourceInfo.modeInfoIdx, &paths[i].targetInfo.modeInfoIdx };
        for (guint k = 0; k < G_N_ELEMENTS(idx); k++) {
            if (*idx[k] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || *idx[k] >= from->nmodes) {
                *idx[k] = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                continue;
            }
            modes[nm] = from->modes[*idx[k]];
            *idx[k] = nm++;
        }
    }
    LONG r = SetDisplayConfig(n, paths, nm, nm ? modes : NULL,
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                              SDC_SAVE_TO_DATABASE);
    g_free(modes);
    return r;
}

/* ---------- daftar monitor ---------- */

static BOOL CALLBACK collect_monitor(HMONITOR mon, HDC dc, LPRECT rect, LPARAM data)
{
    GArray *list = (GArray *) data;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *) &mi))
        return TRUE;
    WinMonitor m;
    memset(&m, 0, sizeof m);
    char *device = win_utf8(mi.szDevice);
    g_strlcpy(m.device, device, sizeof m.device);
    g_free(device);
    m.handle = (guint64) (guintptr) mon;
    m.x = mi.rcMonitor.left;
    m.y = mi.rcMonitor.top;
    m.width = mi.rcMonitor.right - mi.rcMonitor.left;
    m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    g_array_append_val(list, m);
    return TRUE;
}

static gint compare_monitor(gconstpointer a, gconstpointer b)
{
    const WinMonitor *x = a, *y = b;
    if (x->primary != y->primary)
        return x->primary ? -1 : 1;
    if (x->x != y->x)
        return x->x < y->x ? -1 : 1;
    return x->y < y->y ? -1 : x->y > y->y;
}

GArray *win_monitors(void)
{
    GArray *list = g_array_new(FALSE, TRUE, sizeof(WinMonitor));
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM) list);

    /* Nama dari EDID dan jenis sambungan diambil dari konfigurasi CCD. */
    Config c;
    if (config_query(QDC_ONLY_ACTIVE_PATHS, &c)) {
        for (UINT32 i = 0; i < c.npaths; i++) {
            char gdi[64];
            TargetInfo t;
            if (!source_gdi(c.paths[i].sourceInfo.adapterId, c.paths[i].sourceInfo.id, gdi) ||
                !target_info(c.paths[i].targetInfo.adapterId, c.paths[i].targetInfo.id, &t))
                continue;
            for (guint m = 0; m < list->len; m++) {
                WinMonitor *mon = &g_array_index(list, WinMonitor, m);
                if (g_ascii_strcasecmp(mon->device, gdi) != 0)
                    continue;
                mon->is_virtual = mon->is_virtual || t.is_vdd;
                mon->internal = mon->internal || t.internal;
                if (!mon->name[0] && t.name[0])
                    g_strlcpy(mon->name, t.name, sizeof mon->name);
            }
        }
        config_clear(&c);
    }
    g_array_sort(list, compare_monitor);
    for (guint m = 0; m < list->len; m++) {
        WinMonitor *mon = &g_array_index(list, WinMonitor, m);
        mon->index = (int) m + 1;
        if (mon->internal)
            g_strlcpy(mon->name, "Layar laptop", sizeof mon->name);
        else if (mon->is_virtual)
            g_strlcpy(mon->name, "Layar kedua iPad", sizeof mon->name);
        else if (!mon->name[0]) {
            const char *base = strrchr(mon->device, '\\');
            g_strlcpy(mon->name, base ? base + 1 : mon->device, sizeof mon->name);
        }
    }
    return list;
}

static gboolean device_matches(const char *device, const char *spec)
{
    if (g_ascii_strcasecmp(device, spec) == 0)
        return TRUE;
    const char *base = strrchr(device, '\\');
    return base && g_ascii_strcasecmp(base + 1, spec) == 0;   /* "DISPLAY2" */
}

/* spec: NULL/"primary" untuk monitor utama, nomor urut, "@x,y" untuk monitor
 * yang memuat titik itu, nama GDI, atau nama dari EDID. */
gboolean win_monitor_find(const char *spec, WinMonitor *out)
{
    GArray *list = win_monitors();
    const WinMonitor *found = NULL;
    int px = 0, py = 0;
    gboolean point = spec && spec[0] == '@' && sscanf(spec + 1, "%d,%d", &px, &py) == 2;
    char *end = NULL;
    long index = spec && *spec ? strtol(spec, &end, 10) : 0;
    gboolean numeric = spec && *spec && end && *end == '\0';

    for (guint i = 0; i < list->len && !found; i++) {
        const WinMonitor *m = &g_array_index(list, WinMonitor, i);
        if (!spec || !*spec || g_ascii_strcasecmp(spec, "primary") == 0 || g_ascii_strcasecmp(spec, "utama") == 0)
            found = m->primary ? m : NULL;
        else if (point)
            found = px >= m->x && px < m->x + m->width && py >= m->y && py < m->y + m->height ? m : NULL;
        else if (numeric)
            found = m->index == index ? m : NULL;
        else
            found = device_matches(m->device, spec) || g_ascii_strcasecmp(m->name, spec) == 0 ? m : NULL;
    }
    if (!found && (!spec || !*spec) && list->len)
        found = &g_array_index(list, WinMonitor, 0);
    if (found)
        *out = *found;
    g_array_free(list, TRUE);
    return found != NULL;
}

gboolean win_monitor_by_device(const char *device, WinMonitor *out)
{
    if (!device)
        return FALSE;
    GArray *list = win_monitors();
    gboolean found = FALSE;
    for (guint i = 0; i < list->len && !found; i++) {
        const WinMonitor *m = &g_array_index(list, WinMonitor, i);
        if (g_ascii_strcasecmp(m->device, device) == 0) {
            *out = *m;
            found = TRUE;
        }
    }
    g_array_free(list, TRUE);
    return found;
}

/* Satu baris JSON untuk panel: daftar monitor yang menyala. */
int win_cmd_monitors(void)
{
    GArray *list = win_monitors();
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "event");
    json_builder_add_string_value(b, "monitors");
    json_builder_set_member_name(b, "monitors");
    json_builder_begin_array(b);
    for (guint i = 0; i < list->len; i++) {
        const WinMonitor *m = &g_array_index(list, WinMonitor, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "device");
        json_builder_add_string_value(b, m->device);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, m->name);
        json_builder_set_member_name(b, "index");
        json_builder_add_int_value(b, m->index);
        json_builder_set_member_name(b, "x");
        json_builder_add_int_value(b, m->x);
        json_builder_set_member_name(b, "y");
        json_builder_add_int_value(b, m->y);
        json_builder_set_member_name(b, "width");
        json_builder_add_int_value(b, m->width);
        json_builder_set_member_name(b, "height");
        json_builder_add_int_value(b, m->height);
        json_builder_set_member_name(b, "primary");
        json_builder_add_boolean_value(b, m->primary);
        json_builder_set_member_name(b, "virtual");
        json_builder_add_boolean_value(b, m->is_virtual);
        json_builder_set_member_name(b, "internal");
        json_builder_add_boolean_value(b, m->internal);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    char *line = json_to_string(root, FALSE);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    g_free(line);
    json_node_unref(root);
    g_object_unref(b);
    g_array_free(list, TRUE);
    return 0;
}

/* ---------- pengawas ukuran layar ---------- */

typedef struct {
    char device[64];
    int x, y, width, height;
    GMainLoop *loop;
    gboolean *changed;
} Watch;

static gboolean watch_tick(gpointer data)
{
    Watch *w = data;
    WinMonitor m;
    if (!win_monitor_by_device(w->device, &m) || m.x != w->x || m.y != w->y ||
        m.width != w->width || m.height != w->height) {
        *w->changed = TRUE;
        g_main_loop_quit(w->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void win_screen_watch(App *app, GMainLoop *loop, gboolean *changed)
{
    WinMonitor m;
    if (!win_monitor_by_device(app->monitor, &m))
        return;
    Watch *w = g_new0(Watch, 1);
    g_strlcpy(w->device, m.device, sizeof w->device);
    w->x = m.x;
    w->y = m.y;
    w->width = m.width;
    w->height = m.height;
    w->loop = loop;
    w->changed = changed;
    g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, watch_tick, w, g_free);
}

/* ---------- monitor virtual ---------- */

/* Jalur CCD menuju monitor virtual. Bila belum menyala, dipilih jalur dengan
 * sumber yang belum dipakai monitor lain di adaptor yang sama. */
static gboolean vdd_path(const Config *all, const Config *active, DISPLAYCONFIG_PATH_INFO *out,
                         gboolean *is_active)
{
    for (UINT32 i = 0; i < active->npaths; i++) {
        if (path_is_vdd(&active->paths[i])) {
            *out = active->paths[i];
            *is_active = TRUE;
            return TRUE;
        }
    }
    *is_active = FALSE;
    for (UINT32 i = 0; i < all->npaths; i++) {
        const DISPLAYCONFIG_PATH_INFO *p = &all->paths[i];
        if (!p->targetInfo.targetAvailable || !path_is_vdd(p))
            continue;
        gboolean used = FALSE;
        for (UINT32 a = 0; a < active->npaths && !used; a++)
            used = same_luid(active->paths[a].sourceInfo.adapterId, p->sourceInfo.adapterId) &&
                   active->paths[a].sourceInfo.id == p->sourceInfo.id;
        if (!used) {
            *out = *p;
            return TRUE;
        }
    }
    return FALSE;
}

/* Pabrikan GPU yang menggambar monitor ini menurut DXGI (0x8086 Intel,
 * 0x10DE NVIDIA, 0x1002 AMD), 0 bila tidak diketahui. Di laptop dengan dua
 * GPU, layar bawaan biasanya di GPU Intel dan monitor HDMI di NVIDIA; encoder
 * di GPU yang sama dengan monitornya membaca frame tanpa salinan antar-GPU. */
guint win_monitor_gpu_vendor(const char *device)
{
    if (!device)
        return 0;
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &factory)))
        return 0;
    guint vendor = 0;
    IDXGIAdapter1 *adapter = NULL;
    for (UINT i = 0; !vendor && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 ad;
        memset(&ad, 0, sizeof ad);
        IDXGIAdapter1_GetDesc1(adapter, &ad);
        IDXGIOutput *output = NULL;
        for (UINT k = 0; !vendor && IDXGIAdapter1_EnumOutputs(adapter, k, &output) != DXGI_ERROR_NOT_FOUND; k++) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(IDXGIOutput_GetDesc(output, &od))) {
                char *name = win_utf8(od.DeviceName);
                if (g_ascii_strcasecmp(name, device) == 0)
                    vendor = ad.VendorId;
                g_free(name);
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return vendor;
}

gboolean vdisp_available(void)
{
    Config all, active;
    gboolean found = FALSE;
    if (config_query(QDC_ALL_PATHS, &all)) {
        if (config_query(QDC_ONLY_ACTIVE_PATHS, &active)) {
            DISPLAYCONFIG_PATH_INFO p;
            gboolean on;
            found = vdd_path(&all, &active, &p, &on);
            config_clear(&active);
        }
        config_clear(&all);
    }
    return found;
}

static gboolean vdd_attach(GError **error)
{
    Config all, active;
    if (!config_query(QDC_ALL_PATHS, &all)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Konfigurasi tampilan Windows tidak bisa dibaca");
        return FALSE;
    }
    if (!config_query(QDC_ONLY_ACTIVE_PATHS, &active)) {
        config_clear(&all);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Konfigurasi tampilan Windows tidak bisa dibaca");
        return FALSE;
    }
    DISPLAYCONFIG_PATH_INFO p;
    gboolean on = FALSE;
    gboolean ok = vdd_path(&all, &active, &p, &on);
    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Driver layar virtual belum terpasang. Pasang ulang Tsunagu-Pad dan centang "
                    "komponen \"Layar kedua\".");
    } else if (!on) {
        DISPLAYCONFIG_PATH_INFO *paths = g_new0(DISPLAYCONFIG_PATH_INFO, active.npaths + 1);
        memcpy(paths, active.paths, active.npaths * sizeof *paths);
        paths[active.npaths] = p;
        paths[active.npaths].flags = DISPLAYCONFIG_PATH_ACTIVE;
        paths[active.npaths].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths[active.npaths].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        LONG r = config_apply(paths, active.npaths + 1, &active);
        g_free(paths);
        if (r != ERROR_SUCCESS) {
            /* Cadangan: perluas desktop ke semua monitor yang tersambung. */
            r = SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        }
        if (r != ERROR_SUCCESS) {
            char *msg = g_win32_error_message(r);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Monitor virtual tidak bisa dinyalakan: %s", msg);
            g_free(msg);
            ok = FALSE;
        }
    }
    config_clear(&all);
    config_clear(&active);
    return ok;
}

/* Nama GDI monitor virtual yang sedang menyala. */
static gboolean vdd_active_device(char out[64], LUID *adapter, UINT32 *source, guint *active_count)
{
    Config active;
    if (!config_query(QDC_ONLY_ACTIVE_PATHS, &active))
        return FALSE;
    gboolean found = FALSE;
    for (UINT32 i = 0; i < active.npaths && !found; i++) {
        if (!path_is_vdd(&active.paths[i]))
            continue;
        found = source_gdi(active.paths[i].sourceInfo.adapterId, active.paths[i].sourceInfo.id, out);
        if (found && adapter)
            *adapter = active.paths[i].sourceInfo.adapterId;
        if (found && source)
            *source = active.paths[i].sourceInfo.id;
    }
    if (active_count)
        *active_count = active.npaths;
    config_clear(&active);
    return found;
}

static gboolean vdd_detach(void)
{
    Config active;
    if (!config_query(QDC_ONLY_ACTIVE_PATHS, &active))
        return FALSE;
    DISPLAYCONFIG_PATH_INFO *keep = g_new0(DISPLAYCONFIG_PATH_INFO, MAX(active.npaths, 1));
    UINT32 n = 0;
    char gdi[64] = "";
    for (UINT32 i = 0; i < active.npaths; i++) {
        if (path_is_vdd(&active.paths[i]))
            source_gdi(active.paths[i].sourceInfo.adapterId, active.paths[i].sourceInfo.id, gdi);
        else
            keep[n++] = active.paths[i];
    }
    gboolean ok = TRUE;
    /* Monitor virtual tidak pernah dilepas bila hanya ia yang menyala (mesin
     * tanpa layar lain): tanpanya Windows tidak punya desktop sama sekali. */
    if (n > 0 && n < active.npaths) {
        LONG r = config_apply(keep, n, &active);
        if (r != ERROR_SUCCESS && gdi[0]) {
            /* Cadangan lewat API lama: ukuran nol melepas monitor dari desktop. */
            wchar_t *wdev = win_utf16(gdi);
            DEVMODEW dm;
            memset(&dm, 0, sizeof dm);
            dm.dmSize = sizeof dm;
            dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
            r = ChangeDisplaySettingsExW(wdev, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
            if (r == DISP_CHANGE_SUCCESSFUL)
                r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
            ok = r == DISP_CHANGE_SUCCESSFUL;
            g_free(wdev);
        }
    }
    g_free(keep);
    config_clear(&active);
    return ok;
}

gboolean vdisp_detach_all(void)
{
    return vdd_detach();
}

/* Ukuran yang tersedia di driver, dengan laju segar paling dekat ke 60 Hz:
 * laju 144 atau 240 Hz hanya membuat penangkap layar bekerja lebih keras. */
static gboolean find_mode(const wchar_t *dev, int w, int h, DWORD *freq)
{
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    gboolean found = FALSE;
    DWORD best = 0;
    for (DWORD i = 0; EnumDisplaySettingsExW(dev, i, &dm, 0); i++) {
        if ((int) dm.dmPelsWidth != w || (int) dm.dmPelsHeight != h)
            continue;
        DWORD f = dm.dmDisplayFrequency;
        if (!found || abs((int) f - 60) < abs((int) best - 60))
            best = f;
        found = TRUE;
    }
    if (freq)
        *freq = best;
    return found;
}

static char *list_modes(const wchar_t *dev)
{
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsExW(dev, i, &dm, 0); i++) {
        char *key = g_strdup_printf("%lux%lu", (unsigned long) dm.dmPelsWidth, (unsigned long) dm.dmPelsHeight);
        if (g_hash_table_add(seen, key)) {
            if (out->len)
                g_string_append(out, ", ");
            g_string_append(out, key);
        }
    }
    g_hash_table_destroy(seen);
    return g_string_free(out, FALSE);
}

static char *layout_path(void)
{
    char *dir = win_state_dir();
    char *path = g_build_filename(dir, "second-screen", NULL);
    g_free(dir);
    return path;
}

static gboolean layout_load(int *dx, int *dy)
{
    char *path = layout_path(), *text = NULL;
    gboolean ok = g_file_get_contents(path, &text, NULL, NULL) && sscanf(text, "%d %d", dx, dy) == 2;
    g_free(text);
    g_free(path);
    return ok;
}

static void layout_save(int dx, int dy)
{
    char *path = layout_path();
    char *text = g_strdup_printf("%d %d\n", dx, dy);
    g_file_set_contents(path, text, -1, NULL);
    g_free(text);
    g_free(path);
}

/* Letak bawaan: di kanan monitor paling kanan, sejajar atas dengan monitor utama. */
static void default_position(const char *self, int *x, int *y)
{
    GArray *list = win_monitors();
    int right = 0, top = 0;
    gboolean any = FALSE;
    for (guint i = 0; i < list->len; i++) {
        const WinMonitor *m = &g_array_index(list, WinMonitor, i);
        if (g_ascii_strcasecmp(m->device, self) == 0)
            continue;
        if (!any || m->x + m->width > right)
            right = m->x + m->width;
        if (m->primary)
            top = m->y;
        any = TRUE;
    }
    g_array_free(list, TRUE);
    *x = right;
    *y = top;
}

static gboolean primary_origin(int *x, int *y)
{
    WinMonitor p;
    if (!win_monitor_find("primary", &p))
        return FALSE;
    *x = p.x;
    *y = p.y;
    return TRUE;
}

/* ---------- skala (DPI) per monitor ----------
 * Windows tidak menyediakan API resmi untuk mengubah skala satu monitor.
 * Pengaturan Tampilan sendiri memakai jenis informasi -3/-4 pada
 * DisplayConfigGetDeviceInfo/SetDeviceInfo, dengan nilai relatif terhadap
 * skala yang dianjurkan Windows untuk monitor itu. */

static const int dpi_values[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_rel, cur_rel, max_rel;
} DpiGet;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 rel;
} DpiSet;

static gboolean dpi_get(LUID adapter, UINT32 source, int *recommended, int *current, int *max)
{
    DpiGet g;
    memset(&g, 0, sizeof g);
    g.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE) -3;
    g.header.size = sizeof g;
    g.header.adapterId = adapter;
    g.header.id = source;
    if (DisplayConfigGetDeviceInfo(&g.header) != ERROR_SUCCESS)
        return FALSE;
    int rec = abs(g.min_rel);
    *recommended = rec;
    *current = CLAMP(rec + g.cur_rel, 0, (int) G_N_ELEMENTS(dpi_values) - 1);
    *max = CLAMP(rec + g.max_rel, 0, (int) G_N_ELEMENTS(dpi_values) - 1);
    return TRUE;
}

static gboolean dpi_set_percent(LUID adapter, UINT32 source, int percent)
{
    int rec, cur, max;
    if (!dpi_get(adapter, source, &rec, &cur, &max))
        return FALSE;
    int want = 0;
    for (int i = 0; i < (int) G_N_ELEMENTS(dpi_values); i++)
        if (abs(dpi_values[i] - percent) < abs(dpi_values[want] - percent))
            want = i;
    want = CLAMP(want, 0, max);
    if (want == cur)
        return TRUE;
    DpiSet s;
    memset(&s, 0, sizeof s);
    s.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE) -4;
    s.header.size = sizeof s;
    s.header.adapterId = adapter;
    s.header.id = source;
    s.rel = want - rec;
    return DisplayConfigSetDeviceInfo(&s.header) == ERROR_SUCCESS;
}

static int primary_scale_percent(void)
{
    WinMonitor p;
    Config active;
    int percent = 100;
    if (!win_monitor_find("primary", &p) || !config_query(QDC_ONLY_ACTIVE_PATHS, &active))
        return percent;
    for (UINT32 i = 0; i < active.npaths; i++) {
        char gdi[64];
        int rec, cur, max;
        if (source_gdi(active.paths[i].sourceInfo.adapterId, active.paths[i].sourceInfo.id, gdi) &&
            g_ascii_strcasecmp(gdi, p.device) == 0 &&
            dpi_get(active.paths[i].sourceInfo.adapterId, active.paths[i].sourceInfo.id, &rec, &cur, &max)) {
            percent = dpi_values[cur];
            break;
        }
    }
    config_clear(&active);
    return percent;
}

/* Ukuran yang belum dikenal driver ditambahkan oleh layanan Tsunagu-Pad
 * (butuh hak administrator untuk memuat ulang drivernya). */
static gboolean request_mode(const wchar_t *dev, int w, int h)
{
    if (!win_service_installed())
        return FALSE;
    char *cmd = g_strdup_printf("vdd-mode %d %d", w, h);
    gboolean ok = win_service_request(cmd, NULL, NULL);
    g_free(cmd);
    if (!ok)
        return FALSE;
    /* Driver dimuat ulang; monitor virtual hilang sebentar lalu muncul lagi. */
    for (int i = 0; i < 40; i++) {
        g_usleep(250 * 1000);
        char gdi[64];
        if (!vdd_active_device(gdi, NULL, NULL, NULL)) {
            vdd_attach(NULL);
            continue;
        }
        wchar_t *wdev = win_utf16(gdi);
        gboolean found = find_mode(wdev, w, h, NULL);
        g_free(wdev);
        if (found)
            return TRUE;
    }
    return FALSE;
}

Vdisp *vdisp_open(int width, int height, double scale, GError **error)
{
    char gdi[64];
    gboolean was_active = vdd_active_device(gdi, NULL, NULL, NULL);
    if (!was_active) {
        if (!vdd_attach(error))
            return NULL;
        /* Windows butuh sesaat sampai monitor baru muncul di desktop. */
        for (int i = 0; i < 40 && !vdd_active_device(gdi, NULL, NULL, NULL); i++)
            g_usleep(100 * 1000);
    }
    LUID adapter;
    UINT32 source;
    if (!vdd_active_device(gdi, &adapter, &source, NULL)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Monitor virtual tidak muncul setelah dinyalakan");
        return NULL;
    }

    wchar_t *wdev = win_utf16(gdi);
    DWORD freq = 0;
    if (!find_mode(wdev, width, height, &freq) && request_mode(wdev, width, height)) {
        vdd_active_device(gdi, &adapter, &source, NULL);
        g_free(wdev);
        wdev = win_utf16(gdi);
        find_mode(wdev, width, height, &freq);
    }
    if (!find_mode(wdev, width, height, &freq)) {
        char *modes = list_modes(wdev);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Ukuran %dx%d belum tersedia di driver layar virtual. Ukuran yang tersedia: %s",
                    width, height, modes);
        g_free(modes);
        g_free(wdev);
        if (!was_active)
            vdd_detach();
        return NULL;
    }

    int x, y, px, py, dx, dy;
    if (layout_load(&dx, &dy) && primary_origin(&px, &py)) {
        x = px + dx;
        y = py + dy;
    } else {
        default_position(gdi, &x, &y);
    }

    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | (freq ? DM_DISPLAYFREQUENCY : 0);
    dm.dmPelsWidth = (DWORD) width;
    dm.dmPelsHeight = (DWORD) height;
    dm.dmDisplayFrequency = freq;
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    LONG r = ChangeDisplaySettingsExW(wdev, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    if (r == DISP_CHANGE_SUCCESSFUL)
        r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    g_free(wdev);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Ukuran monitor virtual tidak bisa diatur (kode %ld)", (long) r);
        if (!was_active)
            vdd_detach();
        return NULL;
    }

    /* Skala 1 berarti ukuran yang diminta itu juga luas desktopnya; "ikut"
     * (0) menyamakan dengan monitor utama. */
    int percent = scale > 0 ? (int) (scale * 100 + 0.5) : primary_scale_percent();
    vdd_active_device(gdi, &adapter, &source, NULL);
    if (!dpi_set_percent(adapter, source, percent))
        tsunagu_event("warning", "message", "Skala layar kedua tidak bisa diatur; dipakai skala bawaan Windows", NULL);

    /* Tunggu sampai ukurannya benar-benar berlaku sebelum tangkapan dimulai. */
    for (int i = 0; i < 30; i++) {
        WinMonitor m;
        if (win_monitor_by_device(gdi, &m) && m.width == width && m.height == height)
            break;
        g_usleep(100 * 1000);
    }

    Vdisp *v = g_new0(Vdisp, 1);
    g_strlcpy(v->device, gdi, sizeof v->device);
    v->width = width;
    v->height = height;
    v->was_active = was_active;
    return v;
}

const char *vdisp_device(Vdisp *v)
{
    return v->device;
}

/* Letak terakhir dicatat, lalu monitor virtual dilepas dari desktop. Jendela
 * yang masih ada di sana dipindahkan Windows ke monitor lain. */
void vdisp_close(Vdisp *v)
{
    if (!v)
        return;
    WinMonitor m;
    int px, py;
    if (win_monitor_by_device(v->device, &m) && primary_origin(&px, &py) && !m.primary) {
        int dx, dy;
        if (!layout_load(&dx, &dy) || dx != m.x - px || dy != m.y - py)
            layout_save(m.x - px, m.y - py);
    }
    if (!v->was_active)
        vdd_detach();
    g_free(v);
}

int win_cmd_second_off(void)
{
    char gdi[64];
    guint count = 0;
    if (!vdd_active_device(gdi, NULL, NULL, &count)) {
        tsunagu_event("second", "state", "off", NULL);
        return 0;
    }
    if (count <= 1) {
        tsunagu_event("second", "state", "kept", "message", "Monitor virtual adalah satu-satunya layar", NULL);
        return 0;
    }
    gboolean ok = vdd_detach();
    tsunagu_event("second", "state", ok ? "off" : "failed", NULL);
    return ok ? 0 : 1;
}
