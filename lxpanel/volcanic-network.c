/*
 * volcanic-network - lxpanel plugin: a NetworkManager Wi-Fi applet that mimics
 * Plasma's plasma-nm applet, in GTK2.
 *
 * Panel icon reflects the live Wi-Fi signal/state. Left-click opens a popup with
 * a Wi-Fi on/off toggle, the active connection (with Disconnect), and a scrolled
 * list of access points (signal icon + SSID + lock badge). Clicking an AP
 * connects: a saved connection is activated; an open AP is added+activated; a
 * secured-but-unknown AP prompts for a password (GTK2 dialog) then add+activate.
 *
 * Talks to NetworkManager via libnm (GObject, no GTK dependency), so it builds
 * against whatever GTK lxpanel uses (GTK2 here). Only Wi-Fi (+ an active-VPN
 * indicator) is shown -- wired/virtual devices never clutter it.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <lxpanel/plugin.h>
#include <lxpanel/misc.h>          /* lxpanel_button_new_for_icon / set_icon */
#include <glib/gi18n.h>
#include <NetworkManager.h>        /* libnm umbrella header */

typedef struct {
    LXPanel          *panel;
    config_setting_t *settings;
    GtkWidget        *button;      /* panel icon button */
    GtkWidget        *popup;       /* the dropdown window (NULL when hidden) */
    GtkWidget        *list_box;    /* VBox holding AP rows, rebuilt on demand */
    GtkWidget        *wifi_check;  /* the Wi-Fi enable toggle */
    gboolean          updating_toggle;

    NMClient         *client;
    NMDeviceWifi     *wifi;        /* first Wi-Fi device, or NULL */
    guint             icon_idle;
} NetPlugin;

/* ----- small helpers ----------------------------------------------------- */

static gchar *ap_ssid_str(NMAccessPoint *ap)
{
    GBytes *ssid = nm_access_point_get_ssid(ap);
    if (!ssid)
        return g_strdup("");
    return nm_utils_ssid_to_utf8(g_bytes_get_data(ssid, NULL),
                                 g_bytes_get_size(ssid));
}

static gboolean ap_is_secured(NMAccessPoint *ap)
{
    NM80211ApFlags         f   = nm_access_point_get_flags(ap);
    NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
    NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
    return (f & NM_802_11_AP_FLAGS_PRIVACY) || wpa || rsn;
}

static const char *strength_icon(guint8 s, gboolean secured)
{
    /* icon-theme names shipped by NetworkManager / breeze-icons */
    if (s >= 80) return "network-wireless-signal-excellent";
    if (s >= 55) return "network-wireless-signal-good";
    if (s >= 30) return "network-wireless-signal-ok";
    if (s >= 5)  return "network-wireless-signal-weak";
    (void) secured;
    return "network-wireless-signal-none";
}

/* Find a saved connection whose SSID matches the AP. Returns a borrowed ref. */
static NMRemoteConnection *saved_conn_for_ap(NetPlugin *np, NMAccessPoint *ap)
{
    GBytes *want = nm_access_point_get_ssid(ap);
    if (!want)
        return NULL;
    const GPtrArray *cons = nm_client_get_connections(np->client);
    for (guint i = 0; cons && i < cons->len; i++) {
        NMRemoteConnection *rc = g_ptr_array_index(cons, i);
        NMSettingWireless *sw =
            nm_connection_get_setting_wireless(NM_CONNECTION(rc));
        if (!sw)
            continue;
        GBytes *have = nm_setting_wireless_get_ssid(sw);
        if (have && g_bytes_equal(have, want))
            return rc;
    }
    return NULL;
}

/* ----- panel icon -------------------------------------------------------- */

static void update_icon(NetPlugin *np)
{
    const char *icon = "network-wireless-disconnected";
    char       *tip  = NULL;

    if (!np->client || !nm_client_wireless_get_enabled(np->client)) {
        icon = "network-wireless-disabled";
        tip  = g_strdup(_("Wi-Fi disabled"));
    } else if (np->wifi) {
        NMAccessPoint *act = nm_device_wifi_get_active_access_point(np->wifi);
        if (act) {
            guint8 s = nm_access_point_get_strength(act);
            gchar *ssid = ap_ssid_str(act);
            icon = strength_icon(s, ap_is_secured(act));
            tip  = g_strdup_printf(_("Connected: %s (%u%%)"), ssid, s);
            g_free(ssid);
        } else {
            icon = "network-wireless-disconnected";
            tip  = g_strdup(_("Not connected"));
        }
    } else {
        icon = "network-wireless-disconnected";
        tip  = g_strdup(_("No Wi-Fi device"));
    }

    lxpanel_button_set_icon(np->button, icon, -1);
    if (tip) {
        gtk_widget_set_tooltip_text(np->button, tip);
        g_free(tip);
    }
}

static gboolean icon_idle_cb(gpointer data)
{
    NetPlugin *np = data;
    np->icon_idle = 0;
    update_icon(np);
    return FALSE;
}

/* Coalesce bursts of NM signals into a single icon refresh. */
static void schedule_icon(NetPlugin *np)
{
    if (!np->icon_idle)
        np->icon_idle = g_idle_add(icon_idle_cb, np);
}

/* ----- connect / disconnect --------------------------------------------- */

static void activate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_activate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) {
        g_warning("volcanic-network: activate failed: %s", err->message);
        g_clear_error(&err);
    }
}

static void add_activate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) {
        g_warning("volcanic-network: add+activate failed: %s", err->message);
        g_clear_error(&err);
    }
}

static void popup_hide(NetPlugin *np);

/* Build a new Wi-Fi connection for an AP, optionally with a PSK. */
static NMConnection *make_wifi_conn(NMAccessPoint *ap, const char *psk)
{
    NMConnection *c = nm_simple_connection_new();
    gchar *ssid = ap_ssid_str(ap);

    NMSettingConnection *sc = (NMSettingConnection *) nm_setting_connection_new();
    g_object_set(sc, NM_SETTING_CONNECTION_ID, ssid,
                     NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
                     NULL);
    nm_connection_add_setting(c, NM_SETTING(sc));

    NMSettingWireless *sw = (NMSettingWireless *) nm_setting_wireless_new();
    g_object_set(sw, NM_SETTING_WIRELESS_SSID, nm_access_point_get_ssid(ap), NULL);
    nm_connection_add_setting(c, NM_SETTING(sw));

    if (psk) {
        NMSettingWirelessSecurity *ss =
            (NMSettingWirelessSecurity *) nm_setting_wireless_security_new();
        g_object_set(ss, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                         NM_SETTING_WIRELESS_SECURITY_PSK, psk, NULL);
        nm_connection_add_setting(c, NM_SETTING(ss));
    }
    g_free(ssid);
    return c;
}

/* Ask for a password (modal). Returns newly-allocated string or NULL. */
static gchar *ask_password(NetPlugin *np, const char *ssid)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        _("Wi-Fi Password"),
        GTK_WINDOW(gtk_widget_get_toplevel(np->button)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_CONNECT, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gchar *msg = g_strdup_printf(_("Password for \"%s\":"), ssid);
    GtkWidget *lbl = gtk_label_new(msg);
    g_free(msg);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);

    gchar *out = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
        if (t && *t)
            out = g_strdup(t);
    }
    gtk_widget_destroy(dlg);
    return out;
}

static void on_ap_clicked(GtkWidget *w, gpointer data)
{
    NetPlugin     *np = g_object_get_data(G_OBJECT(w), "np");
    NMAccessPoint *ap = g_object_get_data(G_OBJECT(w), "ap");
    if (!np || !ap || !np->wifi)
        return;

    const char *apath = nm_object_get_path(NM_OBJECT(ap));
    NMRemoteConnection *saved = saved_conn_for_ap(np, ap);

    popup_hide(np);

    if (saved) {
        nm_client_activate_connection_async(np->client, NM_CONNECTION(saved),
            NM_DEVICE(np->wifi), apath, NULL, activate_done, np);
        return;
    }

    NMConnection *c;
    if (ap_is_secured(ap)) {
        gchar *ssid = ap_ssid_str(ap);
        gchar *psk  = ask_password(np, ssid);
        g_free(ssid);
        if (!psk)
            return;                 /* cancelled */
        c = make_wifi_conn(ap, psk);
        g_free(psk);
    } else {
        c = make_wifi_conn(ap, NULL);
    }
    nm_client_add_and_activate_connection_async(np->client, c,
        NM_DEVICE(np->wifi), apath, NULL, add_activate_done, np);
    g_object_unref(c);
}

static void deactivate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_deactivate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) { g_warning("volcanic-network: disconnect failed: %s", err->message);
               g_clear_error(&err); }
}

static void on_disconnect_clicked(GtkWidget *w, gpointer data)
{
    NetPlugin *np = data;
    NMActiveConnection *ac =
        np->wifi ? nm_device_get_active_connection(NM_DEVICE(np->wifi)) : NULL;
    popup_hide(np);
    if (ac)
        nm_client_deactivate_connection_async(np->client, ac, NULL,
                                              deactivate_done, np);
}

static void on_wifi_toggled(GtkToggleButton *t, gpointer data)
{
    NetPlugin *np = data;
    if (np->updating_toggle)
        return;
    /* non-deprecated path: set the NM WirelessEnabled property over D-Bus */
    nm_client_dbus_set_property(np->client,
        NM_DBUS_PATH, NM_DBUS_INTERFACE, "WirelessEnabled",
        g_variant_new_boolean(gtk_toggle_button_get_active(t)),
        -1, NULL, NULL, NULL);
}

/* ----- popup ------------------------------------------------------------- */

/* Row factory: an icon + SSID + (optional) lock, packed into a clickable button. */
static GtkWidget *make_ap_row(NetPlugin *np, NMAccessPoint *ap, gboolean active)
{
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    GtkWidget *hb = gtk_hbox_new(FALSE, 6);
    gtk_container_add(GTK_CONTAINER(btn), hb);

    guint8 s = nm_access_point_get_strength(ap);
    GtkWidget *sig = gtk_image_new_from_icon_name(
        strength_icon(s, ap_is_secured(ap)), GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(hb), sig, FALSE, FALSE, 0);

    gchar *ssid = ap_ssid_str(ap);
    gchar *markup = active ? g_markup_printf_escaped("<b>%s</b>", ssid)
                           : g_markup_escape_text(ssid, -1);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hb), lbl, TRUE, TRUE, 0);
    g_free(markup);
    g_free(ssid);

    if (ap_is_secured(ap)) {
        GtkWidget *lock = gtk_image_new_from_icon_name(
            "network-wireless-encrypted", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start(GTK_BOX(hb), lock, FALSE, FALSE, 0);
    }

    g_object_set_data(G_OBJECT(btn), "np", np);
    g_object_set_data_full(G_OBJECT(btn), "ap", g_object_ref(ap), g_object_unref);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_ap_clicked), NULL);
    return btn;
}

/* Strongest AP per SSID, sorted by strength desc. */
static GPtrArray *collect_aps(NMDeviceWifi *wifi)
{
    GHashTable *best = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const GPtrArray *aps = nm_device_wifi_get_access_points(wifi);
    for (guint i = 0; aps && i < aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        gchar *ssid = ap_ssid_str(ap);
        if (!*ssid) { g_free(ssid); continue; }
        NMAccessPoint *cur = g_hash_table_lookup(best, ssid);
        if (!cur || nm_access_point_get_strength(ap) >
                    nm_access_point_get_strength(cur))
            g_hash_table_insert(best, g_strdup(ssid), ap);
        g_free(ssid);
    }

    GPtrArray *out = g_ptr_array_new();
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, best);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(out, v);
    g_hash_table_destroy(best);
    return out;
}

static gint cmp_strength(gconstpointer a, gconstpointer b)
{
    NMAccessPoint *x = *(NMAccessPoint **) a, *y = *(NMAccessPoint **) b;
    return (gint) nm_access_point_get_strength(y) -
           (gint) nm_access_point_get_strength(x);
}

static void rebuild_list(NetPlugin *np)
{
    if (!np->list_box)
        return;
    gtk_container_foreach(GTK_CONTAINER(np->list_box),
                          (GtkCallback) gtk_widget_destroy, NULL);

    if (!np->wifi || !nm_client_wireless_get_enabled(np->client)) {
        GtkWidget *l = gtk_label_new(_("Wi-Fi is off"));
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(np->list_box), l, FALSE, FALSE, 4);
        gtk_widget_show_all(np->list_box);
        return;
    }

    NMAccessPoint *active = nm_device_wifi_get_active_access_point(np->wifi);
    GPtrArray *aps = collect_aps(np->wifi);
    g_ptr_array_sort(aps, cmp_strength);
    for (guint i = 0; i < aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        gtk_box_pack_start(GTK_BOX(np->list_box),
                           make_ap_row(np, ap, ap == active), FALSE, FALSE, 0);
    }
    if (aps->len == 0) {
        GtkWidget *l = gtk_label_new(_("No networks found"));
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(np->list_box), l, FALSE, FALSE, 4);
    }
    g_ptr_array_free(aps, TRUE);
    gtk_widget_show_all(np->list_box);
}

static void scan_done(GObject *src, GAsyncResult *res, gpointer data)
{
    nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(src), res, NULL);
    /* list refresh happens via access-point-added/removed signals */
}

static gboolean popup_focus_out(GtkWidget *w, GdkEventFocus *e, gpointer data)
{
    popup_hide((NetPlugin *) data);
    return FALSE;
}

static void popup_hide(NetPlugin *np)
{
    if (np->popup) {
        gtk_widget_destroy(np->popup);
        np->popup = NULL;
        np->list_box = NULL;
        np->wifi_check = NULL;
    }
}

static void popup_show(NetPlugin *np)
{
    /* TOPLEVEL (not POPUP) so it can take focus -> focus-out dismiss works */
    np->popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(np->popup), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(np->popup), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(np->popup), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(np->popup), GDK_WINDOW_TYPE_HINT_MENU);
    gtk_container_set_border_width(GTK_CONTAINER(np->popup), 4);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_container_add(GTK_CONTAINER(np->popup), frame);

    GtkWidget *vb = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 6);
    gtk_container_add(GTK_CONTAINER(frame), vb);

    /* header: Wi-Fi toggle */
    GtkWidget *hdr = gtk_hbox_new(FALSE, 6);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), _("<b>Wi-Fi</b>"));
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hdr), title, TRUE, TRUE, 0);
    np->wifi_check = gtk_check_button_new();
    np->updating_toggle = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(np->wifi_check),
                                 nm_client_wireless_get_enabled(np->client));
    np->updating_toggle = FALSE;
    g_signal_connect(np->wifi_check, "toggled",
                     G_CALLBACK(on_wifi_toggled), np);
    gtk_box_pack_start(GTK_BOX(hdr), np->wifi_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), hdr, FALSE, FALSE, 0);

    /* active connection + disconnect */
    NMAccessPoint *active =
        np->wifi ? nm_device_wifi_get_active_access_point(np->wifi) : NULL;
    if (active) {
        GtkWidget *row = gtk_hbox_new(FALSE, 6);
        gchar *ssid = ap_ssid_str(active);
        gchar *m = g_markup_printf_escaped(_("Connected: <b>%s</b>"), ssid);
        GtkWidget *l = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(l), m);
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
        GtkWidget *dc = gtk_button_new_with_label(_("Disconnect"));
        g_signal_connect(dc, "clicked", G_CALLBACK(on_disconnect_clicked), np);
        gtk_box_pack_start(GTK_BOX(row), dc, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vb), row, FALSE, FALSE, 0);
        g_free(ssid); g_free(m);
    }

    gtk_box_pack_start(GTK_BOX(vb), gtk_hseparator_new(), FALSE, FALSE, 0);

    /* scrolled AP list */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 260, 300);
    np->list_box = gtk_vbox_new(FALSE, 0);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll),
                                          np->list_box);
    gtk_box_pack_start(GTK_BOX(vb), scroll, TRUE, TRUE, 0);

    rebuild_list(np);

    g_signal_connect(np->popup, "focus-out-event",
                     G_CALLBACK(popup_focus_out), np);

    gtk_widget_show_all(np->popup);
    lxpanel_plugin_adjust_popup_position(np->popup, np->button);
    gtk_window_present(GTK_WINDOW(np->popup));

    /* kick a fresh scan; results arrive via AP add/remove signals */
    if (np->wifi && nm_client_wireless_get_enabled(np->client))
        nm_device_wifi_request_scan_async(np->wifi, NULL, scan_done, np);
}

/* ----- NM signal plumbing ------------------------------------------------ */

static void on_nm_changed(GObject *o, GParamSpec *p, gpointer data)
{
    NetPlugin *np = data;
    schedule_icon(np);
    if (np->popup) {
        if (np->wifi_check) {
            np->updating_toggle = TRUE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(np->wifi_check),
                nm_client_wireless_get_enabled(np->client));
            np->updating_toggle = FALSE;
        }
        rebuild_list(np);
    }
}

static void on_aps_changed(NMDeviceWifi *w, NMAccessPoint *ap, gpointer data)
{
    NetPlugin *np = data;
    if (np->popup)
        rebuild_list(np);
}

static void hook_wifi_device(NetPlugin *np)
{
    const GPtrArray *devs = nm_client_get_devices(np->client);
    for (guint i = 0; devs && i < devs->len; i++) {
        NMDevice *d = g_ptr_array_index(devs, i);
        if (NM_IS_DEVICE_WIFI(d)) {
            np->wifi = NM_DEVICE_WIFI(d);
            g_signal_connect(d, "notify::" NM_DEVICE_WIFI_ACTIVE_ACCESS_POINT,
                             G_CALLBACK(on_nm_changed), np);
            g_signal_connect(d, "state-changed",
                             G_CALLBACK(on_nm_changed), np);
            g_signal_connect(d, "access-point-added",
                             G_CALLBACK(on_aps_changed), np);
            g_signal_connect(d, "access-point-removed",
                             G_CALLBACK(on_aps_changed), np);
            break;
        }
    }
}

/* ----- lxpanel glue ------------------------------------------------------ */

static gboolean on_button_press(GtkWidget *w, GdkEventButton *e, LXPanel *panel)
{
    NetPlugin *np = lxpanel_plugin_get_data(w);
    if (e->button == 1) {
        if (np->popup)
            popup_hide(np);
        else
            popup_show(np);
        return TRUE;
    }
    return FALSE;
}

static void net_destructor(gpointer data)
{
    NetPlugin *np = data;
    if (np->icon_idle)
        g_source_remove(np->icon_idle);
    popup_hide(np);
    if (np->wifi)
        g_signal_handlers_disconnect_by_data(np->wifi, np);
    if (np->client) {
        g_signal_handlers_disconnect_by_data(np->client, np);
        g_object_unref(np->client);
    }
    g_free(np);
}

static GtkWidget *net_constructor(LXPanel *panel, config_setting_t *settings)
{
    NetPlugin *np = g_new0(NetPlugin, 1);
    np->panel = panel;
    np->settings = settings;

    np->button = lxpanel_button_new_for_icon(panel,
                     "network-wireless-disconnected", NULL, NULL);

    GError *err = NULL;
    np->client = nm_client_new(NULL, &err);
    if (!np->client) {
        g_warning("volcanic-network: cannot connect to NetworkManager: %s",
                  err ? err->message : "?");
        g_clear_error(&err);
        gtk_widget_set_tooltip_text(np->button, _("NetworkManager unavailable"));
    } else {
        hook_wifi_device(np);
        g_signal_connect(np->client,
            "notify::" NM_CLIENT_ACTIVE_CONNECTIONS, G_CALLBACK(on_nm_changed), np);
        g_signal_connect(np->client,
            "notify::" NM_CLIENT_WIRELESS_ENABLED, G_CALLBACK(on_nm_changed), np);
        update_icon(np);
    }

    /* return the icon button directly (matches volcanic-overview); lxpanel
       routes button_press_event to it and calls net_destructor on removal */
    lxpanel_plugin_set_data(np->button, np, net_destructor);
    gtk_widget_show_all(np->button);
    return np->button;
}

FM_DEFINE_MODULE(lxpanel_gtk, volcanic_network)

LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name        = N_("Volcanic Network"),
    .description = N_("NetworkManager Wi-Fi applet (Plasma-style, GTK2)."),
    .new_instance = net_constructor,
    .button_press_event = on_button_press,
};
