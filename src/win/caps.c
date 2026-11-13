/* Mengenali kemampuan mesin Windows ini untuk panel.
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <string.h>

#include "win.h"

static char *bios_value(const wchar_t *name)
{
    wchar_t buf[256];
    DWORD size = sizeof buf - sizeof(wchar_t);
    memset(buf, 0, sizeof buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name,
                     RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS)
        return g_strdup("");
    char *text = win_utf8(buf);
    return g_strstrip(text);
}

/* Lenovo menaruh nama pasarannya (mis. "Legion 5 15ACH6H") di SystemVersion,
 * sedangkan SystemProductName hanya kode mesin. */
char *caps_model(void)
{
    char *maker = bios_value(L"SystemManufacturer");
    char *product = bios_value(L"SystemProductName");
    char *version = bios_value(L"SystemVersion");
    const char *name = (g_ascii_strcasecmp(maker, "LENOVO") == 0 && *version) ? version : product;
    char *model = *maker && *name ? g_strdup_printf("%s %s", maker, name) : g_strdup(name);
    g_free(maker);
    g_free(product);
    g_free(version);
    return model;
}

gboolean caps_is_jetson(void)
{
    return FALSE;
}

const char *caps_hotspot(void)
{
    return win_hotspot_capability();
}
