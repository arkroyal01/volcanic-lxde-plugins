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
#include <string.h>                /* strstr / strtol for panel-font probe */
#include <stdlib.h>
#include <gdk/gdkkeysyms.h>        /* GDK_Escape */
#include <NetworkManager.h>        /* libnm umbrella header */

typedef struct {
    LXPanel          *panel;
    config_setting_t *settings;
    GtkWidget        *button;      /* panel icon button */
    GtkWidget        *popup;       /* the dropdown window (NULL when hidden) */
    GtkWidget        *list_box;    /* VBox holding AP rows, rebuilt on demand */
    GtkWidget        *wifi_check;  /* the Wi-Fi enable toggle */
    GtkWidget        *inline_editor; /* in-popup password row, or NULL */
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

/* Per-strength icon-name fallback chains -- the first name the active theme
   actually has wins. Covers breeze (signal-* in status/, plus connected-NN),
   freedesktop/Adwaita (signal-*), oxygen, and NM's own nm-signal-* set, with a
   generic "network-wireless" last so something always renders. */
static const char *const *strength_names(guint8 s)
{
    static const char *excellent[] = {
        "network-wireless-signal-excellent", "network-wireless-connected-100",
        "network-wireless-100", "nm-signal-100", "network-wireless", NULL };
    static const char *good[] = {
        "network-wireless-signal-good", "network-wireless-connected-75",
        "network-wireless-80", "nm-signal-75", "network-wireless", NULL };
    static const char *okk[] = {
        "network-wireless-signal-ok", "network-wireless-connected-50",
        "network-wireless-60", "nm-signal-50", "network-wireless", NULL };
    static const char *weak[] = {
        "network-wireless-signal-weak", "network-wireless-connected-25",
        "network-wireless-40", "nm-signal-25", "network-wireless", NULL };
    static const char *none_[] = {
        "network-wireless-signal-none", "network-wireless-connected-00",
        "network-wireless-20", "nm-signal-0", "network-wireless", NULL };
    if (s >= 80) return excellent;
    if (s >= 55) return good;
    if (s >= 30) return okk;
    if (s >= 5)  return weak;
    return none_;
}

/* First name in the chain that the current icon theme actually provides. */
static const char *best_icon(const char *const *names)
{
    GtkIconTheme *t = gtk_icon_theme_get_default();
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i]))
            return names[i];
    return names[0];
}

/* A GtkImage rendering the first available name from a chain (theme-agnostic). */
static GtkWidget *image_from_names(const char *const *names, GtkIconSize size)
{
    GIcon     *gi  = g_themed_icon_new_from_names((char **) names, -1);
    GtkWidget *img = gtk_image_new_from_gicon(gi, size);
    g_object_unref(gi);
    return img;
}

static const char *icon_disconnected(void)
{
    static const char *n[] = { "network-wireless-disconnected",
        "network-wireless-offline", "network-wireless-0", "network-wireless", NULL };
    return best_icon(n);
}

static const char *icon_disabled(void)
{
    static const char *n[] = { "network-wireless-disabled", "network-wireless-off",
        "network-wireless-hardware-disabled", "network-wireless", NULL };
    return best_icon(n);
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
    const char *icon = icon_disconnected();
    char       *tip  = NULL;

    if (!np->client || !nm_client_wireless_get_enabled(np->client)) {
        icon = icon_disabled();
        tip  = g_strdup(_("Wi-Fi disabled"));
    } else if (np->wifi) {
        NMAccessPoint *act = nm_device_wifi_get_active_access_point(np->wifi);
        if (act) {
            guint8 s = nm_access_point_get_strength(act);
            gchar *ssid = ap_ssid_str(act);
            icon = best_icon(strength_names(s));
            tip  = g_strdup_printf(_("Connected: %s (%u%%)"), ssid, s);
            g_free(ssid);
        } else {
            icon = icon_disconnected();
            tip  = g_strdup(_("Not connected"));
        }
    } else {
        icon = icon_disconnected();
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

/* Surface NM errors to the user (and the log) instead of failing silently. */
static void report_error(const char *action, const GError *err)
{
    g_warning("volcanic-network: %s: %s", action,
              err ? err->message : "unknown error");
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE, "%s:\n%s", action, err ? err->message : "unknown error");
    gtk_window_set_title(GTK_WINDOW(d), _("Network"));
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
}

static void activate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_activate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) {
        report_error(_("Could not connect"), err);
        g_clear_error(&err);
    }
}

static void add_activate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) {
        report_error(_("Could not connect"), err);
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
                         NM_SETTING_WIRELESS_SECURITY_PSK, psk,
                         /* system-wide: NM stores the secret on disk
                            (psk-flags=NONE). A per-user encrypted store via a
                            libsecret/Secret-Service agent is backlog. */
                         NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS,
                         NM_SETTING_SECRET_FLAG_NONE, NULL);
        nm_connection_add_setting(c, NM_SETTING(ss));
    }
    g_free(ssid);
    return c;
}

/* (the password entry is an inline row in the popup now -- show_inline_editor) */

/* Deferred connect: after committing a converted (now system-stored)
   connection, activate it. Carries the AP path across the async commit. */
typedef struct { NetPlugin *np; NMRemoteConnection *conn; gchar *apath; } ConnectCtx;

static void commit_then_activate(GObject *src, GAsyncResult *res, gpointer data)
{
    ConnectCtx *c = data;
    GError *err = NULL;
    nm_remote_connection_commit_changes_finish(NM_REMOTE_CONNECTION(src), res, &err);
    if (err) {
        report_error(_("Could not save password"), err);
        g_clear_error(&err);
    } else {
        nm_client_activate_connection_async(c->np->client, NM_CONNECTION(c->conn),
            NM_DEVICE(c->np->wifi), c->apath, NULL, activate_done, c->np);
    }
    g_object_unref(c->conn);
    g_free(c->apath);
    g_free(c);
}

/* Connect to ap with an optional psk. saved+open / saved+system-stored ->
   activate; saved+psk -> convert to system-stored (commit) then activate;
   new -> add+activate. */
static void do_connect(NetPlugin *np, NMAccessPoint *ap, const char *psk)
{
    const char *apath = nm_object_get_path(NM_OBJECT(ap));
    NMRemoteConnection *saved = saved_conn_for_ap(np, ap);

    if (saved) {
        if (psk) {
            NMSettingWirelessSecurity *sec =
                nm_connection_get_setting_wireless_security(NM_CONNECTION(saved));
            if (!sec) {
                sec = (NMSettingWirelessSecurity *)
                          nm_setting_wireless_security_new();
                g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
                             "wpa-psk", NULL);
                nm_connection_add_setting(NM_CONNECTION(saved), NM_SETTING(sec));
                sec = nm_connection_get_setting_wireless_security(
                          NM_CONNECTION(saved));
            }
            g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, psk,
                         NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS,
                         NM_SETTING_SECRET_FLAG_NONE, NULL);
            ConnectCtx *ctx = g_new0(ConnectCtx, 1);
            ctx->np    = np;
            ctx->conn  = g_object_ref(saved);
            ctx->apath = g_strdup(apath);
            nm_remote_connection_commit_changes_async(saved, TRUE, NULL,
                                                      commit_then_activate, ctx);
        } else {
            nm_client_activate_connection_async(np->client, NM_CONNECTION(saved),
                NM_DEVICE(np->wifi), apath, NULL, activate_done, np);
        }
    } else {
        NMConnection *c = make_wifi_conn(ap, psk);   /* psk NULL for open nets */
        nm_client_add_and_activate_connection_async(np->client, c,
            NM_DEVICE(np->wifi), apath, NULL, add_activate_done, np);
        g_object_unref(c);
    }
}

static void editor_remove(NetPlugin *np)
{
    if (np->inline_editor) {
        GtkWidget *e = np->inline_editor;
        np->inline_editor = NULL;        /* clear first so rebuild isn't blocked */
        gtk_widget_destroy(e);
    }
}

static gboolean editor_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    if (e->keyval == GDK_Escape) {       /* Esc cancels the inline prompt */
        editor_remove((NetPlugin *) data);
        return TRUE;
    }
    return FALSE;
}

static void editor_submit(GtkWidget *w, gpointer data)
{
    GtkWidget     *editor = data;
    NetPlugin     *np    = g_object_get_data(G_OBJECT(editor), "np");
    NMAccessPoint *ap    = g_object_get_data(G_OBJECT(editor), "ap");
    GtkWidget     *entry = g_object_get_data(G_OBJECT(editor), "entry");
    const char    *t     = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!t || !*t)
        return;                          /* empty -> ignore */
    gchar         *psk = g_strdup(t);
    NMAccessPoint *apr = g_object_ref(ap);
    popup_hide(np);                      /* closes the popup (and the editor) */
    do_connect(np, apr, psk);
    g_object_unref(apr);
    g_free(psk);
}

/* Expand an inline password row directly beneath the clicked network (KDE-style). */
static void show_inline_editor(NetPlugin *np, GtkWidget *row, NMAccessPoint *ap)
{
    editor_remove(np);

    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 2);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    GtkWidget *btn = gtk_button_new_with_label(_("Connect"));
    gtk_box_pack_start(GTK_BOX(box), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(box), "np", np);
    g_object_set_data_full(G_OBJECT(box), "ap", g_object_ref(ap), g_object_unref);
    g_object_set_data(G_OBJECT(box), "entry", entry);
    g_signal_connect(entry, "activate", G_CALLBACK(editor_submit), box);
    g_signal_connect(btn,   "clicked",  G_CALLBACK(editor_submit), box);
    g_signal_connect(entry, "key-press-event", G_CALLBACK(editor_key), np);

    GList *kids = gtk_container_get_children(GTK_CONTAINER(np->list_box));
    gint idx = g_list_index(kids, row);
    g_list_free(kids);

    gtk_box_pack_start(GTK_BOX(np->list_box), box, FALSE, FALSE, 0);
    if (idx >= 0)
        gtk_box_reorder_child(GTK_BOX(np->list_box), box, idx + 1);
    np->inline_editor = box;
    gtk_widget_show_all(box);
    gtk_widget_grab_focus(entry);
}

static void add_detail(GtkWidget *box, const char *label, const char *value)
{
    if (!value || !*value)
        return;
    gchar *m = g_markup_printf_escaped("<span size=\"small\">%s</span> %s",
                                       label, value);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), m);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    g_free(m);
}

static const char *ap_security_str(NMAccessPoint *ap)
{
    NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
    NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
    if (rsn & NM_802_11_AP_SEC_KEY_MGMT_SAE) return "WPA3";
    if (rsn)                                 return "WPA2";
    if (wpa)                                 return "WPA";
    if (nm_access_point_get_flags(ap) & NM_802_11_AP_FLAGS_PRIVACY) return "WEP";
    return "Open";
}

static const char *freq_band(guint32 mhz)
{
    if (mhz >= 5925) return "6 GHz";
    if (mhz >= 4900) return "5 GHz";
    if (mhz >= 2400) return "2.4 GHz";
    return "";
}

static void add_ip_details(GtkWidget *vb, NMIPConfig *ip, const char *fam)
{
    if (!ip)
        return;
    const GPtrArray *addrs = nm_ip_config_get_addresses(ip);
    for (guint i = 0; addrs && i < addrs->len; i++) {
        NMIPAddress *a = g_ptr_array_index(addrs, i);
        gchar *s = g_strdup_printf("%s/%u", nm_ip_address_get_address(a),
                                   nm_ip_address_get_prefix(a));
        gchar *l = g_strdup_printf(_("%s address:"), fam);
        add_detail(vb, l, s);
        g_free(s); g_free(l);
    }
    gchar *gl = g_strdup_printf(_("%s gateway:"), fam);
    add_detail(vb, gl, nm_ip_config_get_gateway(ip));
    g_free(gl);
    const char *const *dns = nm_ip_config_get_nameservers(ip);
    for (int i = 0; dns && dns[i]; i++) {
        gchar *dl = g_strdup_printf(_("%s DNS:"), fam);
        add_detail(vb, dl, dns[i]);
        g_free(dl);
    }
}

/* Full connection details: a Close-able dialog with everything NM exposes. */
static void show_details_dialog(NetPlugin *np, NMAccessPoint *ap)
{
    NMDevice  *dev = NM_DEVICE(np->wifi);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Connection Details"),
        GTK_WINDOW(gtk_widget_get_toplevel(np->button)),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *vb = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 8);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       vb, TRUE, TRUE, 0);

    gchar *ssid = ap_ssid_str(ap);
    add_detail(vb, _("Network:"), ssid);
    g_free(ssid);
    add_detail(vb, _("BSSID:"), nm_access_point_get_bssid(ap));
    add_detail(vb, _("Security:"), ap_security_str(ap));

    guint32 freq = nm_access_point_get_frequency(ap);
    gchar *fs = g_strdup_printf("%u MHz (%s)", freq, freq_band(freq));
    add_detail(vb, _("Frequency:"), fs);
    g_free(fs);

    guint32 br = nm_device_wifi_get_bitrate(np->wifi);   /* kb/s */
    if (br) { gchar *s = g_strdup_printf(_("%u Mbit/s"), br / 1000);
              add_detail(vb, _("Speed:"), s); g_free(s); }
    guint32 mb = nm_access_point_get_max_bitrate(ap);
    if (mb) { gchar *s = g_strdup_printf(_("%u Mbit/s"), mb / 1000);
              add_detail(vb, _("Max rate:"), s); g_free(s); }

    gchar *sig = g_strdup_printf("%u%%", nm_access_point_get_strength(ap));
    add_detail(vb, _("Signal:"), sig);
    g_free(sig);
    add_detail(vb, _("Adapter MAC:"), nm_device_get_hw_address(dev));

    gtk_box_pack_start(GTK_BOX(vb), gtk_hseparator_new(), FALSE, FALSE, 2);
    add_ip_details(vb, nm_device_get_ip4_config(dev), "IPv4");
    add_ip_details(vb, nm_device_get_ip6_config(dev), "IPv6");

    g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(dlg);
}

static void on_details_clicked(GtkWidget *w, gpointer data)
{
    NetPlugin     *np = g_object_get_data(G_OBJECT(w), "np");
    NMAccessPoint *ap = g_object_get_data(G_OBJECT(w), "ap");
    if (np && ap)
        show_details_dialog(np, ap);
}

/* Expand an inline read-only details panel (IP/gateway/DNS/signal/speed +
   a Details button) under the connected network. Reuses the single inline slot. */
static void show_inline_details(NetPlugin *np, GtkWidget *row, NMAccessPoint *ap)
{
    editor_remove(np);

    GtkWidget *box = gtk_vbox_new(FALSE, 1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);

    NMIPConfig *ip = nm_device_get_ip4_config(NM_DEVICE(np->wifi));
    if (ip) {
        const GPtrArray *addrs = nm_ip_config_get_addresses(ip);
        if (addrs && addrs->len) {
            NMIPAddress *a = g_ptr_array_index(addrs, 0);
            gchar *s = g_strdup_printf("%s/%u", nm_ip_address_get_address(a),
                                       nm_ip_address_get_prefix(a));
            add_detail(box, _("IP:"), s);
            g_free(s);
        }
        add_detail(box, _("Gateway:"), nm_ip_config_get_gateway(ip));
        const char *const *dns = nm_ip_config_get_nameservers(ip);
        if (dns && dns[0])
            add_detail(box, _("DNS:"), dns[0]);
    }
    gchar *sig = g_strdup_printf("%u%%", nm_access_point_get_strength(ap));
    add_detail(box, _("Signal:"), sig);
    g_free(sig);

    guint32 br = nm_device_wifi_get_bitrate(np->wifi);   /* kb/s */
    if (br) {
        gchar *s = g_strdup_printf(_("%u Mbit/s"), br / 1000);
        add_detail(box, _("Speed:"), s);
        g_free(s);
    }

    /* Details button -> opens the full details in a separate window */
    GtkWidget *bb = gtk_hbox_new(FALSE, 0);
    GtkWidget *db = gtk_button_new_with_label(_("Details\xE2\x80\xA6"));
    g_object_set_data(G_OBJECT(db), "np", np);
    g_object_set_data_full(G_OBJECT(db), "ap", g_object_ref(ap), g_object_unref);
    g_signal_connect(db, "clicked", G_CALLBACK(on_details_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bb), db, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 2);

    /* mark which AP this panel is for, so a second click collapses it */
    g_object_set_data(G_OBJECT(box), "details_ap", ap);

    GList *kids = gtk_container_get_children(GTK_CONTAINER(np->list_box));
    gint idx = g_list_index(kids, row);
    g_list_free(kids);

    gtk_box_pack_start(GTK_BOX(np->list_box), box, FALSE, FALSE, 0);
    if (idx >= 0)
        gtk_box_reorder_child(GTK_BOX(np->list_box), box, idx + 1);
    np->inline_editor = box;
    gtk_widget_show_all(box);
}

static void on_ap_clicked(GtkWidget *w, gpointer data)
{
    NetPlugin     *np = g_object_get_data(G_OBJECT(w), "np");
    NMAccessPoint *ap = g_object_get_data(G_OBJECT(w), "ap");
    if (!np || !ap || !np->wifi)
        return;

    /* clicking the connected network expands inline details (IP/gateway/...);
       it never disconnects (only the Disconnect button does) nor dismisses.
       A second click collapses it. */
    if (ap == nm_device_wifi_get_active_access_point(np->wifi)) {
        if (np->inline_editor &&
            g_object_get_data(G_OBJECT(np->inline_editor), "details_ap") == ap)
            editor_remove(np);
        else
            show_inline_details(np, w, ap);
        return;
    }

    NMRemoteConnection *saved = saved_conn_for_ap(np, ap);
    gboolean needs_pw;
    if (!ap_is_secured(ap)) {
        needs_pw = FALSE;                 /* open network */
    } else if (saved) {
        NMSettingWirelessSecurity *sec =
            nm_connection_get_setting_wireless_security(NM_CONNECTION(saved));
        NMSettingSecretFlags flags = sec
            ? nm_setting_wireless_security_get_psk_flags(sec)
            : NM_SETTING_SECRET_FLAG_NONE;
        needs_pw = (flags != NM_SETTING_SECRET_FLAG_NONE); /* agent-owned -> ask */
    } else {
        needs_pw = TRUE;                  /* new secured network */
    }

    if (needs_pw) {
        show_inline_editor(np, w, ap);    /* submit -> do_connect(np, ap, psk) */
    } else {
        popup_hide(np);
        do_connect(np, ap, NULL);
    }
}

static void deactivate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    nm_client_deactivate_connection_finish(NM_CLIENT(src), res, &err);
    if (err) { report_error(_("Could not disconnect"), err);
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
    GtkWidget *sig = image_from_names(strength_names(s), GTK_ICON_SIZE_MENU);
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
        static const char *lk[] = { "network-wireless-encrypted",
            "object-locked", "channel-secure-symbolic", "security-high",
            "lock", NULL };
        GtkWidget *lock = image_from_names(lk, GTK_ICON_SIZE_MENU);
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
    if (np->inline_editor)
        return;             /* don't tear down an open inline password editor */
    gtk_container_foreach(GTK_CONTAINER(np->list_box),
                          (GtkCallback) gtk_widget_destroy, NULL);

    if (!np->wifi || !nm_client_wireless_get_enabled(np->client)) {
        GtkWidget *l = gtk_label_new(_("Wi-Fi is off"));
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(np->list_box), l, FALSE, FALSE, 4);
        gtk_widget_show_all(np->list_box);
        return;
    }

    /* match the active network by SSID, not object identity: the list keeps the
       strongest BSSID per SSID, which may differ from the associated AP object */
    NMAccessPoint *active = nm_device_wifi_get_active_access_point(np->wifi);
    GBytes *active_ssid = active ? nm_access_point_get_ssid(active) : NULL;
    GPtrArray *aps = collect_aps(np->wifi);
    g_ptr_array_sort(aps, cmp_strength);
    for (guint i = 0; i < aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        GBytes *ssid = nm_access_point_get_ssid(ap);
        gboolean is_active = active_ssid && ssid && g_bytes_equal(active_ssid, ssid);
        gtk_box_pack_start(GTK_BOX(np->list_box),
                           make_ap_row(np, ap, is_active), FALSE, FALSE, 0);
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
        np->inline_editor = NULL;
    }
}

/* The popup is built from plain GtkLabels, so its text renders at the GTK
 * toolkit font (e.g. "Sans 10") and ignores the panel's own font setting --
 * which looks tiny next to a panel using a large custom font size. lxpanel has
 * no public getter for the configured size, but its public label helper bakes
 * that size into the markup it generates. Run a throwaway label through it once,
 * read the point size back out, and use it as the popup's base font so every
 * child label (and relative <b>/size="small" spans) scales with the panel.
 * No-op when the panel uses the default toolkit font, or if the probe fails. */
static void np_apply_panel_font(NetPlugin *np, GtkWidget *w)
{
    GtkWidget *probe = gtk_label_new(NULL);
    g_object_ref_sink(probe);
    lxpanel_draw_label_text(np->panel, probe, "0", FALSE, 1.0, FALSE);
    const char *markup = gtk_label_get_label(GTK_LABEL(probe));
    int pt = 0;
    if (markup) {
        const char *p = strstr(markup, "font_desc=\"");
        if (p)
            pt = (int) strtol(p + strlen("font_desc=\""), NULL, 10);
    }
    g_object_unref(probe);
    if (pt <= 0)
        return;

    PangoFontDescription *fd =
        pango_font_description_copy(gtk_widget_get_style(w)->font_desc);
    pango_font_description_set_size(fd, pt * PANGO_SCALE);
    gtk_widget_modify_font(w, fd);
    pango_font_description_free(fd);
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

    /* scale popup text with the panel's custom font size (see helper above) */
    np_apply_panel_font(np, np->popup);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_container_add(GTK_CONTAINER(np->popup), frame);

    GtkWidget *vb = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 6);
    gtk_container_add(GTK_CONTAINER(frame), vb);

    /* header: Wi-Fi toggle */
    GtkWidget *hdr = gtk_hbox_new(FALSE, 6);
    GtkWidget *title = gtk_label_new(NULL);
    const char *iface = np->wifi ? nm_device_get_iface(NM_DEVICE(np->wifi)) : NULL;
    gchar *hm = (iface && *iface)
        ? g_markup_printf_escaped("<b>%s</b>  <span size=\"small\">(%s)</span>",
                                  _("Wi-Fi"), iface)
        : g_markup_printf_escaped("<b>%s</b>", _("Wi-Fi"));
    gtk_label_set_markup(GTK_LABEL(title), hm);
    g_free(hm);
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
    /* position under the panel icon. Use *_popup_set_position_helper (the one
       the lxpanel binary actually exports) -- adjust_popup_position is declared
       in the header but absent from the binary, which fails our dlopen. */
    gint px = 0, py = 0;
    lxpanel_plugin_popup_set_position_helper(np->panel, np->button,
                                             np->popup, &px, &py);
    gtk_window_move(GTK_WINDOW(np->popup), px, py);
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

/* NMDevice::state-changed has its own signature (device, new, old, reason,
   data) -- it must NOT share on_nm_changed (a notify:: handler), or `data`
   lands on old_state instead of our NetPlugin. */
static void on_dev_state(NMDevice *d, guint new_state, guint old_state,
                         guint reason, gpointer data)
{
    on_nm_changed(NULL, NULL, data);
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
                             G_CALLBACK(on_dev_state), np);
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
