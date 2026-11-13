/* Hotspot langsung di Windows: Mobile Hotspot bawaan Windows, dikendalikan
 * lewat windows/hotspot.ps1 (API WinRT NetworkOperatorTetheringManager).
 *
 *   tsunagupad hotspot on | off | status
 *   tsunagupad hotspot config NAMA_WIFI   (kata sandi dari stdin)
 *
 * Satu baris JSON ke stdout untuk panel, dan kalimat biasa ke stderr.
 * SPDX-License-Identifier: MIT
 */
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>

#include "win.h"

static char *script_path(void)
{
    char *root = win_install_dir();
    char *candidates[] = {
        g_build_filename(root, "share", "tsunagupad", "hotspot.ps1", NULL),
        g_build_filename(root, "windows", "hotspot.ps1", NULL),   /* dari folder sumber */
        NULL,
    };
    char *found = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (!found && g_file_test(candidates[i], G_FILE_TEST_IS_REGULAR))
            found = g_strdup(candidates[i]);
        g_free(candidates[i]);
    }
    g_free(root);
    return found;
}

/* PowerShell dipanggil lewat jalur lengkap di folder sistem, bukan lewat PATH. */
static char *powershell_path(void)
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, G_N_ELEMENTS(dir));
    char *sys = n ? win_utf8(dir) : g_strdup("C:\\Windows\\System32");
    char *path = g_build_filename(sys, "WindowsPowerShell", "v1.0", "powershell.exe", NULL);
    g_free(sys);
    return path;
}

/* Menjalankan skrip dan mengembalikan objek JSON baris terakhirnya. */
static JsonObject *run_script(const char *action, const char *arg, const char *input, JsonParser **parser)
{
    char *script = script_path();
    if (!script)
        return NULL;
    char *ps = powershell_path();
    const char *argv[] = { ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                           "-File", script, action, arg, NULL };
    if (!arg)
        argv[8] = NULL;
    char *out = NULL;
    int code = 0;
    gboolean ran = win_run(argv, input, &out, &code, NULL);
    g_free(ps);
    g_free(script);
    if (!ran || !out) {
        g_free(out);
        return NULL;
    }
    JsonObject *result = NULL;
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = g_strv_length(lines) - 1; i >= 0 && !result; i--) {
        char *line = g_strstrip(lines[i]);
        if (line[0] != '{')
            continue;
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, line, -1, NULL) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            result = json_node_get_object(json_parser_get_root(p));
            *parser = p;
        } else {
            g_object_unref(p);
        }
    }
    g_strfreev(lines);
    g_free(out);
    return result;
}

/* Mobile Hotspot butuh adaptor Wi-Fi, walau Windows kadang tetap melaporkan
 * "bisa" di mesin yang hanya punya Ethernet. */
static gboolean has_wifi_adapter(void)
{
    ULONG size = 32 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && ret == ERROR_BUFFER_OVERFLOW; tries++) {
        g_free(list);
        list = g_malloc(size);
        ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                                   GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    gboolean found = FALSE;
    for (IP_ADAPTER_ADDRESSES *a = ret == NO_ERROR ? list : NULL; a && !found; a = a->Next)
        found = a->IfType == IF_TYPE_IEEE80211;
    g_free(list);
    return found;
}

const char *win_hotspot_capability(void)
{
    static const char *cached;
    if (cached)
        return cached;
    if (!has_wifi_adapter())
        return cached = "no";
    JsonParser *parser = NULL;
    JsonObject *o = run_script("capability", NULL, NULL, &parser);
    const char *cap = o ? json_object_get_string_member_with_default(o, "capability", "unknown") : "unknown";
    cached = g_str_equal(cap, "yes") ? "yes" : g_str_equal(cap, "no") ? "no" : "unknown";
    if (parser)
        g_object_unref(parser);
    return cached;
}

gboolean win_hotspot_active(void)
{
    JsonParser *parser = NULL;
    JsonObject *o = run_script("status", NULL, NULL, &parser);
    gboolean active = o && g_strcmp0(json_object_get_string_member_with_default(o, "state", ""), "active") == 0;
    if (parser)
        g_object_unref(parser);
    return active;
}

static void print_object(JsonObject *o)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, o);
    char *line = json_to_string(node, FALSE);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    g_free(line);
    json_node_unref(node);
}

int win_cmd_hotspot(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    gboolean known = g_str_equal(what, "on") || g_str_equal(what, "off") ||
                     g_str_equal(what, "status") || g_str_equal(what, "config");
    if (!known || (g_str_equal(what, "config") && argc < 3)) {
        g_printerr("Pemakaian: tsunagupad hotspot on|off|status|config NAMA_WIFI\n");
        return 1;
    }

    char *password = NULL;
    if (g_str_equal(what, "config")) {
        /* Kata sandi lewat stdin, bukan argumen, supaya tidak terlihat di daftar proses. */
        char buf[256];
        if (fgets(buf, sizeof buf, stdin))
            password = g_strdup_printf("%s\n", g_strchomp(buf));
        if (!password) {
            g_printerr("Kata sandi dibaca dari stdin.\n");
            return 1;
        }
    }

    JsonParser *parser = NULL;
    JsonObject *o = run_script(what, g_str_equal(what, "config") ? argv[2] : NULL, password, &parser);
    if (password) {
        memset(password, 0, strlen(password));
        g_free(password);
    }
    if (!o) {
        tsunagu_event("error", "message", "Pengendali hotspot tidak bisa dijalankan (hotspot.ps1)", NULL);
        g_printerr("Pengendali hotspot tidak bisa dijalankan.\n");
        return 1;
    }
    print_object(o);
    const char *event = json_object_get_string_member_with_default(o, "event", "");
    int rc = 0;
    if (g_str_equal(event, "error")) {
        g_printerr("%s\n", json_object_get_string_member_with_default(o, "message", "Hotspot gagal"));
        rc = 1;
    } else {
        const char *state = json_object_get_string_member_with_default(o, "state", "inactive");
        if (g_str_equal(what, "on"))
            g_printerr("Hotspot dinyalakan.\n");
        else if (g_str_equal(what, "off"))
            g_printerr("Hotspot dimatikan.\n");
        else if (g_str_equal(what, "config"))
            g_printerr("Hotspot disimpan.\n");
        else
            g_printerr("Hotspot: %s\n", state);
    }
    g_object_unref(parser);
    return rc;
}
