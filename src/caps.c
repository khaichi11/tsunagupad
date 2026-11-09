/* Mengenali mesin dari kemampuannya, bukan dari daftar model, supaya varian
 * Jetson dan PC yang belum pernah diuji tetap mendapat pilihan yang benar.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <unistd.h>

#include "tsunagupad.h"

static gboolean group_has(const char *names, const char *wanted)
{
    gboolean found = FALSE;
    char **parts = g_strsplit(names, ",", -1);
    for (char **p = parts; *p; p++)
        found = found || g_str_equal(g_strstrip(*p), wanted);
    g_strfreev(parts);
    return found;
}

/* Satu kombinasi "iw list": managed dan AP harus bisa hidup bersamaan. */
static gboolean combo_allows_ap(const char *combo)
{
    GRegex *group_re = g_regex_new("#\\{([^}]*)\\}\\s*<=\\s*(\\d+)", 0, 0, NULL);
    GRegex *total_re = g_regex_new("total\\s*<=\\s*(\\d+)", 0, 0, NULL);
    int managed = -1, ap = -1, index = 0;
    long managed_max = 0;
    GMatchInfo *m = NULL;

    g_regex_match(group_re, combo, 0, &m);
    while (g_match_info_matches(m)) {
        char *names = g_match_info_fetch(m, 1);
        char *max = g_match_info_fetch(m, 2);
        if (group_has(names, "managed")) {
            managed = index;
            managed_max = strtol(max, NULL, 10);
        }
        if (group_has(names, "AP"))
            ap = index;
        g_free(names);
        g_free(max);
        index++;
        g_match_info_next(m, NULL);
    }
    g_match_info_free(m);

    long total = 0;
    g_regex_match(total_re, combo, 0, &m);
    if (g_match_info_matches(m)) {
        char *t = g_match_info_fetch(m, 1);
        total = strtol(t, NULL, 10);
        g_free(t);
    }
    g_match_info_free(m);
    g_regex_unref(group_re);
    g_regex_unref(total_re);

    return managed >= 0 && ap >= 0 && total >= 2 && (managed != ap || managed_max >= 2);
}

gboolean caps_concurrent_ap(const char *iw_output)
{
    gboolean found = FALSE;
    GString *combo = g_string_new(NULL);
    char **lines = g_strsplit(iw_output, "\n", -1);

    for (char **l = lines; *l && !found; l++) {
        const char *s = g_strstrip(*l);
        if (g_str_has_prefix(s, "* #{"))
            g_string_assign(combo, s);
        else if (combo->len)
            g_string_append_printf(combo, " %s", s);
        if (combo->len && strstr(combo->str, "total <=")) {
            found = combo_allows_ap(combo->str);
            g_string_truncate(combo, 0);
        }
    }
    g_strfreev(lines);
    g_string_free(combo, TRUE);
    return found;
}

const char *caps_hotspot(void)
{
    char *iw = g_find_program_in_path("iw");
    if (!iw && g_file_test("/usr/sbin/iw", G_FILE_TEST_IS_EXECUTABLE))
        iw = g_strdup("/usr/sbin/iw");
    if (!iw)
        return "unknown";

    char *argv[] = { iw, "list", NULL };
    char *out = NULL;
    int status = 0;
    gboolean ran = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                                &out, NULL, &status, NULL);
    const char *answer = "unknown";
    if (ran && g_spawn_check_wait_status(status, NULL))
        answer = out && caps_concurrent_ap(out) ? "yes" : "no";
    g_free(out);
    g_free(iw);
    return answer;
}

char *caps_model(void)
{
    static const char *const sources[] = {
        "/proc/device-tree/model",          /* Jetson dan papan ARM lain */
        "/sys/class/dmi/id/product_name",   /* PC dan laptop */
    };
    for (guint i = 0; i < G_N_ELEMENTS(sources); i++) {
        char *text = NULL;
        if (g_file_get_contents(sources[i], &text, NULL, NULL)) {
            g_strstrip(text);
            if (*text)
                return text;
        }
        g_free(text);
    }
    return g_strdup("");
}

gboolean caps_is_jetson(void)
{
    return g_file_test("/etc/nv_tegra_release", G_FILE_TEST_EXISTS);
}

gboolean caps_has_uinput(void)
{
    return access("/dev/uinput", F_OK) == 0;
}
