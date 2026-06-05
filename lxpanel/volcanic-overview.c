/*
 * volcanic-overview - lxpanel plugin: a panel button that toggles the KWin
 * (Volcanic) window overview.
 *
 * Activation goes through KGlobalAccel's D-Bus interface
 * (org.kde.kglobalaccel /component/kwin invokeShortcut "Overview"), which is
 * how the overview is reached anyway. Note: this needs kglobalacceld running
 * (autostarted in the LXDE session). When Pillar B Phase 3 replaces KGlobalAccel
 * with an in-kwin XGrabKey layer, re-point this at the new trigger.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later (same as the lxpanel plugin API it builds against)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <lxpanel/plugin.h>
#include <lxpanel/misc.h>     /* lxpanel_button_new_for_icon() */
#include <gio/gio.h>
#include <glib/gi18n.h>

#define KGA_SERVICE "org.kde.kglobalaccel"
#define KGA_PATH    "/component/kwin"
#define KGA_IFACE   "org.kde.kglobalaccel.Component"
#define VOLCANIC_OVERVIEW_ICON "view-grid"

static void invoke_overview(void)
{
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        g_warning("volcanic-overview: no session bus: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    /* fire-and-forget; the overview toggles itself */
    g_dbus_connection_call(bus, KGA_SERVICE, KGA_PATH, KGA_IFACE,
                           "invokeShortcut", g_variant_new("(s)", "Overview"),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref(bus);
}

static GtkWidget *volcanic_overview_new(LXPanel *panel, config_setting_t *settings)
{
    GtkWidget *button = lxpanel_button_new_for_icon(panel, VOLCANIC_OVERVIEW_ICON, NULL, NULL);
    gtk_widget_set_tooltip_text(button, _("Window overview"));
    return button;
}

static gboolean volcanic_overview_button_press(GtkWidget *widget, GdkEventButton *event,
                                               LXPanel *panel)
{
    if (event->button == 1) {        /* left click toggles the overview */
        invoke_overview();
        return TRUE;
    }
    return FALSE;                     /* let lxpanel handle right-click menu etc. */
}

FM_DEFINE_MODULE(lxpanel_gtk, volcanic_overview)

LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name        = N_("Window Overview"),
    .description = N_("Toggle the KWin window overview (Volcanic)."),
    .new_instance = volcanic_overview_new,
    .button_press_event = volcanic_overview_button_press,
};
