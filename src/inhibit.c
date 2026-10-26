/* Tahan penguncian dan pemadaman layar otomatis selama iPad tersambung.
 * Memakai iPad sebagai layar sentuh berarti laptop tidak menerima masukan lokal;
 * tanpa ini GNOME mengunci layar setelah beberapa menit, dan sesi berbagi layar
 * ikut putus karena GNOME menolak sesi baru selama layar terkunci.
 * SPDX-License-Identifier: MIT
 */
#include <gio/gio.h>

#include "tsunagupad.h"

#define INHIBIT_IDLE 8

static guint cookie;

void idle_inhibit_set(gboolean on)
{
    if (on == (cookie != 0))
        return;

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return;
    if (on) {
        GVariant *reply = g_dbus_connection_call_sync(
            bus, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager",
            "Inhibit", g_variant_new("(susu)", "tsunagupad", 0u, "Layar dibagikan ke iPad", INHIBIT_IDLE),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (reply) {
            g_variant_get(reply, "(u)", &cookie);
            g_variant_unref(reply);
        }
    } else {
        GVariant *reply = g_dbus_connection_call_sync(
            bus, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager",
            "Uninhibit", g_variant_new("(u)", cookie), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (reply)
            g_variant_unref(reply);
        cookie = 0;
    }
    g_object_unref(bus);
}
