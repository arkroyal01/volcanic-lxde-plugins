/*
 * volcanic-sni - lxpanel plugin: a StatusNotifierItem (SNI / KSNI) system tray.
 *
 * lxpanel only ships the legacy XEmbed tray; modern Qt/GTK4 apps (e.g.
 * EasyEffects) publish their tray icon over D-Bus via the StatusNotifierItem
 * spec instead, so they never appear -- or, bridged through snixembed, appear
 * as near-black icons because the bridge can't recolour them. This plugin is a
 * native SNI host: it owns org.kde.StatusNotifierWatcher, registers as a host,
 * and draws each item itself -- so the icon goes through volcanic-icon's
 * recolour path and follows the panel foreground (white on a dark panel).
 *
 * SNI is pure D-Bus (GDBus, no GTK dependency), so this builds fine as a GTK2
 * lxpanel plugin. Item context menus use libdbusmenu-gtk2 (DbusmenuGtkMenu).
 *
 * Milestone scope: watcher + host, item icons (IconName recoloured, IconPixmap
 * fallback), NewIcon/NewStatus refresh, removal on name loss, left-click
 * Activate / middle SecondaryActivate / right-click menu (dbusmenu or
 * ContextMenu). OverlayIcon, ToolTip popups and Scroll are TODO.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <lxpanel/plugin.h>
#include "volcanic-icon.h"          /* recoloured icons (Breeze on dark) */
#include <gio/gio.h>
#include <libdbusmenu-gtk/menu.h>
#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>                 /* getpid */

#define SNI_WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH  "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_IFACE    "org.kde.StatusNotifierItem"

typedef struct {
    GtkWidget        *box;          /* panel widget: a row/column of items */
    LXPanel          *panel;
    config_setting_t *settings;

    GDBusConnection  *conn;         /* session bus (borrowed) */
    guint             watcher_owner_id;
    guint             host_owner_id;
    guint             watcher_reg_id;
    gboolean          host_registered;

    GHashTable       *items;        /* key (g_strdup bus+path) -> SniItem* */
} SniPlugin;

typedef struct {
    SniPlugin *sni;
    char      *key;                 /* hash key, also the "service" we advertise */
    char      *bus;                 /* item's bus name (unique or well-known) */
    char      *path;                /* item's object path */

    GtkWidget *ev;                  /* event box (one per item) */
    GtkWidget *img;                 /* the icon image */

    guint      watch_id;            /* g_bus_watch_name: detect the item leaving */
    guint      sub_id;              /* subscription to the item's New* signals */
    GCancellable *cancel;           /* cancels in-flight GetAll on free (no UAF) */

    char      *icon_name;
    char      *attention_icon_name;
    char      *overlay_icon_name;
    char      *status;
    char      *menu_path;           /* com.canonical.dbusmenu object, or NULL */
    char      *icon_theme_path;     /* app-private icon dir, or NULL */
    char      *tooltip;             /* pango markup, or NULL */
    GdkPixbuf *pixmap;              /* IconPixmap fallback (unscaled best), or NULL */
    GtkWidget *menu;                /* cached DbusmenuGtkMenu (built ahead), or NULL */
} SniItem;

/* ---- forward decls ------------------------------------------------------ */
static void sni_item_render(SniItem *it);
static void sni_item_refresh(SniItem *it);

/* ---- icon helpers ------------------------------------------------------- */

/* Build a GdkPixbuf from an SNI IconPixmap variant "a(iiay)" (ARGB32, network
 * byte order, straight alpha). Returns the largest frame, or NULL. */
static GdkPixbuf *sni_pixbuf_from_iconpixmap(GVariant *v)
{
    GVariantIter it;
    gint w, h, best_w = 0, best_h = 0;
    GVariant *bytes = NULL, *best = NULL;

    if (v == NULL)
        return NULL;
    g_variant_iter_init(&it, v);
    while (g_variant_iter_next(&it, "(ii@ay)", &w, &h, &bytes)) {
        if (w > 0 && h > 0 && (gint64) w * h > (gint64) best_w * best_h) {
            best_w = w; best_h = h;
            if (best) g_variant_unref(best);
            best = g_variant_ref(bytes);
        }
        g_variant_unref(bytes);
    }
    if (best == NULL)
        return NULL;

    gsize n = 0;
    const guchar *argb = g_variant_get_fixed_array(best, &n, 1);
    GdkPixbuf *pix = NULL;
    if (argb != NULL && n >= (gsize) best_w * best_h * 4) {
        pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, best_w, best_h);
        guchar *dst = gdk_pixbuf_get_pixels(pix);
        int rs = gdk_pixbuf_get_rowstride(pix);
        for (int y = 0; y < best_h; y++) {
            for (int x = 0; x < best_w; x++) {
                const guchar *s = argb + (y * best_w + x) * 4; /* A R G B */
                guchar *d = dst + y * rs + x * 4;              /* R G B A */
                d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
            }
        }
    }
    g_variant_unref(best);
    return pix;
}

/* Load @name from the item's private IconThemePath (app icons not in the system
 * theme). Returns a new pixbuf at @size, or NULL. */
static GdkPixbuf *sni_load_from_themepath(SniItem *it, const char *name, int size)
{
    static const char *const ext[] = { ".svg", ".png", ".xpm", "", NULL };

    if (!it->icon_theme_path || !it->icon_theme_path[0] || !name || !name[0])
        return NULL;
    for (int i = 0; ext[i]; i++) {
        char *f = g_strdup_printf("%s/%s%s", it->icon_theme_path, name, ext[i]);
        GdkPixbuf *p = NULL;
        if (g_file_test(f, G_FILE_TEST_EXISTS))
            p = gdk_pixbuf_new_from_file_at_size(f, size, size, NULL);
        g_free(f);
        if (p)
            return p;
    }
    return NULL;
}

/* The base icon at @size (owned), recoloured to @fg where it's a stylable SVG.
 * Tries IconName (system theme, then private path), then the IconPixmap. */
static GdkPixbuf *sni_base_pixbuf(SniItem *it, int size, const GdkColor *fg)
{
    const char *name = it->icon_name;

    if (it->status && g_strcmp0(it->status, "NeedsAttention") == 0 &&
        it->attention_icon_name && it->attention_icon_name[0])
        name = it->attention_icon_name;

    if (name && name[0]) {
        GdkPixbuf *p = volcanic_icon_pixbuf(panel_get_icon_theme(it->sni->panel),
                                            name, size, fg);
        if (p)
            return p;
        p = sni_load_from_themepath(it, name, size);
        if (p)
            return p;
    }
    if (it->pixmap)
        return gdk_pixbuf_scale_simple(it->pixmap, size, size, GDK_INTERP_BILINEAR);
    return NULL;
}

static void sni_item_render(SniItem *it)
{
    int size = panel_get_icon_size(it->sni->panel);
    GdkColor fg;
    GdkPixbuf *base;

    volcanic_panel_fg(it->sni->panel, &fg);
    base = sni_base_pixbuf(it, size, &fg);

    if (base == NULL) {
        gtk_image_set_from_icon_name(GTK_IMAGE(it->img), "image-missing",
                                     GTK_ICON_SIZE_BUTTON);
    } else {
        /* composite the overlay badge (small, bottom-right) if present */
        if (it->overlay_icon_name && it->overlay_icon_name[0] && size > 1) {
            int osz = (size * 2) / 3;
            GdkPixbuf *ov = volcanic_icon_pixbuf(panel_get_icon_theme(it->sni->panel),
                                                 it->overlay_icon_name, osz, &fg);
            if (ov) {
                if (!gdk_pixbuf_get_has_alpha(base)) {
                    GdkPixbuf *a = gdk_pixbuf_add_alpha(base, FALSE, 0, 0, 0);
                    g_object_unref(base);
                    base = a;
                }
                int ow = gdk_pixbuf_get_width(ov), oh = gdk_pixbuf_get_height(ov);
                gdk_pixbuf_composite(ov, base, size - ow, size - oh, ow, oh,
                                     size - ow, size - oh, 1, 1,
                                     GDK_INTERP_BILINEAR, 255);
                g_object_unref(ov);
            }
        }
        gtk_image_set_from_pixbuf(GTK_IMAGE(it->img), base);
        g_object_unref(base);
    }

    gtk_widget_set_tooltip_markup(it->ev,
                                  (it->tooltip && it->tooltip[0]) ? it->tooltip : NULL);
}

/* ---- item property fetch (GetAll) --------------------------------------- */

static void on_item_getall(GObject *src, GAsyncResult *res, gpointer data)
{
    SniItem *it = data;
    GError  *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (ret == NULL) {
        g_clear_error(&err);         /* item may have vanished mid-flight */
        return;
    }

    GVariant *props = NULL;
    g_variant_get(ret, "(@a{sv})", &props);

    g_clear_pointer(&it->icon_name, g_free);
    g_clear_pointer(&it->attention_icon_name, g_free);
    g_clear_pointer(&it->overlay_icon_name, g_free);
    g_clear_pointer(&it->status, g_free);
    g_clear_pointer(&it->menu_path, g_free);
    g_clear_pointer(&it->icon_theme_path, g_free);
    g_clear_pointer(&it->tooltip, g_free);
    g_clear_pointer(&it->pixmap, g_object_unref);

    GVariantIter iter;
    const char *key;
    GVariant *val;
    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &val)) {
        if (g_strcmp0(key, "IconName") == 0)
            it->icon_name = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "AttentionIconName") == 0)
            it->attention_icon_name = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "OverlayIconName") == 0)
            it->overlay_icon_name = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "Status") == 0)
            it->status = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "Menu") == 0)
            it->menu_path = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "IconThemePath") == 0)
            it->icon_theme_path = g_variant_dup_string(val, NULL);
        else if (g_strcmp0(key, "IconPixmap") == 0 && !it->pixmap)
            it->pixmap = sni_pixbuf_from_iconpixmap(val);
        else if (g_strcmp0(key, "AttentionIconPixmap") == 0 && !it->pixmap &&
                 (!it->icon_name || !it->icon_name[0]))
            it->pixmap = sni_pixbuf_from_iconpixmap(val);
        else if (g_strcmp0(key, "ToolTip") == 0 &&
                 g_variant_is_of_type(val, G_VARIANT_TYPE("(sa(iiay)ss)"))) {
            const char *title = NULL, *body = NULL;
            GVariant *tpix = NULL;
            g_variant_get(val, "(&s@a(iiay)&s&s)", NULL, &tpix, &title, &body);
            if (tpix)
                g_variant_unref(tpix);
            if (title && title[0] && body && body[0])
                it->tooltip = g_markup_printf_escaped("<b>%s</b>\n%s", title, body);
            else if ((title && title[0]) || (body && body[0]))
                it->tooltip = g_markup_escape_text((title && title[0]) ? title : body, -1);
        }
        g_variant_unref(val);
    }
    g_variant_unref(props);
    g_variant_unref(ret);

    /* Build a persistent dbusmenu once, so its layout is already fetched by the
       time the user right-clicks. Popping an unpopulated (zero-height) menu makes
       GTK squish it into a scrolled stub -- building ahead avoids that. */
    if (!it->menu && it->menu_path && it->menu_path[0] &&
        g_strcmp0(it->menu_path, "/") != 0) {
        DbusmenuGtkMenu *m = dbusmenu_gtkmenu_new(it->bus, it->menu_path);
        if (m)
            it->menu = GTK_WIDGET(g_object_ref_sink(m));
    }

    sni_item_render(it);
}

static void sni_item_refresh(SniItem *it)
{
    if (!it->sni->conn)
        return;
    g_dbus_connection_call(it->sni->conn, it->bus, it->path,
                           "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", SNI_ITEM_IFACE),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                           -1, it->cancel, on_item_getall, it);
}

/* ---- item interactions -------------------------------------------------- */

static void sni_item_invoke(SniItem *it, const char *method, int x, int y)
{
    if (!it->sni->conn)
        return;
    g_dbus_connection_call(it->sni->conn, it->bus, it->path, SNI_ITEM_IFACE,
                           method, g_variant_new("(ii)", x, y), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean on_item_button(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    SniItem *it = data;
    int x = (int) e->x_root, y = (int) e->y_root;
    (void) w;

    if (e->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (e->button == 1) {
        sni_item_invoke(it, "Activate", x, y);
        return TRUE;
    }
    if (e->button == 2) {
        sni_item_invoke(it, "SecondaryActivate", x, y);
        return TRUE;
    }
    if (e->button == 3) {
        if (it->menu) {                /* prebuilt + populated (see on_item_getall) */
            /* NULL positioner = spawn at the pointer; since the menu is already
               populated, GTK knows its size and flips it up/left at screen edges
               instead of running off-screen or going to scroll-mode. */
            gtk_menu_popup(GTK_MENU(it->menu), NULL, NULL, NULL, NULL,
                           e->button, e->time);
            return TRUE;
        }
        sni_item_invoke(it, "ContextMenu", x, y);  /* item draws its own menu */
        return TRUE;
    }
    return FALSE;
}

static gboolean on_item_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data)
{
    SniItem *it = data;
    const char *orient = "vertical";
    int delta = 0;
    (void) w;

    switch (e->direction) {
        case GDK_SCROLL_UP:    delta = -1; orient = "vertical";   break;
        case GDK_SCROLL_DOWN:  delta =  1; orient = "vertical";   break;
        case GDK_SCROLL_LEFT:  delta = -1; orient = "horizontal"; break;
        case GDK_SCROLL_RIGHT: delta =  1; orient = "horizontal"; break;
        default: return FALSE;
    }
    if (it->sni->conn)
        g_dbus_connection_call(it->sni->conn, it->bus, it->path, SNI_ITEM_IFACE,
                               "Scroll", g_variant_new("(is)", delta, orient),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    return TRUE;
}

/* ---- item lifecycle ----------------------------------------------------- */

static void on_item_signal(GDBusConnection *c, const char *sender,
                           const char *path, const char *iface,
                           const char *signal, GVariant *params, gpointer data)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) params;
    /* NewIcon / NewAttentionIcon / NewStatus / NewToolTip / NewTitle:
       SNI signals carry no usable payload by spec -- just re-read. */
    sni_item_refresh((SniItem *) data);
}

static void sni_remove_item(SniPlugin *np, const char *key); /* fwd */

static void on_item_name_vanished(GDBusConnection *c, const char *name,
                                  gpointer data)
{
    SniItem *it = data;
    (void) c; (void) name;
    sni_remove_item(it->sni, it->key);   /* frees it -- do nothing after */
}

static void sni_item_free(gpointer data)
{
    SniItem *it = data;
    if (it->cancel) {                /* stop any in-flight GetAll touching us */
        g_cancellable_cancel(it->cancel);
        g_object_unref(it->cancel);
    }
    if (it->sub_id)
        g_dbus_connection_signal_unsubscribe(it->sni->conn, it->sub_id);
    if (it->watch_id)
        g_bus_unwatch_name(it->watch_id);
    if (it->menu) {
        gtk_widget_destroy(it->menu);
        g_object_unref(it->menu);    /* balance the g_object_ref_sink at build */
    }
    if (it->ev)
        gtk_widget_destroy(it->ev);
    g_clear_pointer(&it->pixmap, g_object_unref);
    g_free(it->icon_name);
    g_free(it->attention_icon_name);
    g_free(it->overlay_icon_name);
    g_free(it->status);
    g_free(it->menu_path);
    g_free(it->icon_theme_path);
    g_free(it->tooltip);
    g_free(it->key);
    g_free(it->bus);
    g_free(it->path);
    g_free(it);
}

/* Parse the RegisterStatusNotifierItem argument + message sender into a bus
 * name and object path. Apps pass either "bus/path", a bare bus name (item at
 * /StatusNotifierItem), or just an object path (bus = the message sender). */
static void sni_parse_service(const char *service, const char *sender,
                              char **bus, char **path)
{
    if (service && service[0] == '/') {
        *bus = g_strdup(sender);
        *path = g_strdup(service);
    } else if (service && strchr(service, '/')) {
        const char *slash = strchr(service, '/');
        *bus = g_strndup(service, slash - service);
        *path = g_strdup(slash);
    } else {
        *bus = g_strdup(service && service[0] ? service : sender);
        *path = g_strdup("/StatusNotifierItem");
    }
}

static void sni_add_item(SniPlugin *np, const char *service, const char *sender)
{
    char *bus = NULL, *path = NULL;
    sni_parse_service(service, sender, &bus, &path);
    char *key = g_strdup_printf("%s%s", bus, path);

    if (g_hash_table_contains(np->items, key)) {
        g_free(key); g_free(bus); g_free(path);
        return;
    }

    SniItem *it = g_new0(SniItem, 1);
    it->sni    = np;
    it->key    = key;
    it->bus    = bus;
    it->path   = path;
    it->cancel = g_cancellable_new();

    it->ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(it->ev), FALSE);
    gtk_widget_add_events(it->ev, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    it->img = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(it->ev), it->img);
    g_signal_connect(it->ev, "button-press-event",
                     G_CALLBACK(on_item_button), it);
    g_signal_connect(it->ev, "scroll-event",
                     G_CALLBACK(on_item_scroll), it);
    gtk_box_pack_start(GTK_BOX(np->box), it->ev, FALSE, FALSE, 0);
    gtk_widget_show_all(it->ev);

    it->sub_id = g_dbus_connection_signal_subscribe(np->conn, it->bus,
                     SNI_ITEM_IFACE, NULL, it->path, NULL,
                     G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal, it, NULL);
    it->watch_id = g_bus_watch_name_on_connection(np->conn, it->bus,
                     G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
                     on_item_name_vanished, it, NULL);

    g_hash_table_insert(np->items, g_strdup(key), it);
    sni_item_refresh(it);

    g_dbus_connection_emit_signal(np->conn, NULL, SNI_WATCHER_PATH,
        SNI_WATCHER_IFACE, "StatusNotifierItemRegistered",
        g_variant_new("(s)", key), NULL);
}

static void sni_remove_item(SniPlugin *np, const char *key)
{
    char *k = g_strdup(key);
    if (g_hash_table_remove(np->items, k))   /* value-destroy frees SniItem */
        g_dbus_connection_emit_signal(np->conn, NULL, SNI_WATCHER_PATH,
            SNI_WATCHER_IFACE, "StatusNotifierItemUnregistered",
            g_variant_new("(s)", k), NULL);
    g_free(k);
}

/* ---- the watcher object ------------------------------------------------- */

static const char sni_watcher_xml[] =
    "<node>"
    " <interface name='" SNI_WATCHER_IFACE "'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg type='s' direction='in'/></method>"
    "  <method name='RegisterStatusNotifierHost'>"
    "   <arg type='s' direction='in'/></method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    "  <signal name='StatusNotifierHostUnregistered'/>"
    " </interface>"
    "</node>";

static void watcher_method(GDBusConnection *c, const char *sender,
                           const char *path, const char *iface,
                           const char *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer data)
{
    SniPlugin *np = data;
    (void) c; (void) path; (void) iface;

    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const char *service = NULL;
        g_variant_get(params, "(&s)", &service);
        sni_add_item(np, service, sender);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0) {
        np->host_registered = TRUE;
        g_dbus_method_invocation_return_value(inv, NULL);
        g_dbus_connection_emit_signal(np->conn, NULL, SNI_WATCHER_PATH,
            SNI_WATCHER_IFACE, "StatusNotifierHostRegistered", NULL, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method);
    }
}

static GVariant *watcher_get_prop(GDBusConnection *c, const char *sender,
                                  const char *path, const char *iface,
                                  const char *prop, GError **err, gpointer data)
{
    SniPlugin *np = data;
    (void) c; (void) sender; (void) path; (void) iface; (void) err;

    if (g_strcmp0(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        GHashTableIter it;
        gpointer k;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        g_hash_table_iter_init(&it, np->items);
        while (g_hash_table_iter_next(&it, &k, NULL))
            g_variant_builder_add(&b, "s", (const char *) k);
        return g_variant_builder_end(&b);
    }
    if (g_strcmp0(prop, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (g_strcmp0(prop, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    watcher_method, watcher_get_prop, NULL, { 0 }
};

static void on_bus_acquired(GDBusConnection *conn, const char *name,
                            gpointer data)
{
    SniPlugin *np = data;
    static GDBusNodeInfo *info; /* parsed once */
    GError *err = NULL;
    (void) name;

    np->conn = conn;
    if (info == NULL)
        info = g_dbus_node_info_new_for_xml(sni_watcher_xml, NULL);

    np->watcher_reg_id = g_dbus_connection_register_object(conn,
        SNI_WATCHER_PATH, info->interfaces[0], &watcher_vtable, np, NULL, &err);
    if (err) {
        g_warning("volcanic-sni: register watcher object failed: %s", err->message);
        g_clear_error(&err);
    }
}

static void on_watcher_name_acquired(GDBusConnection *conn, const char *name,
                                     gpointer data)
{
    SniPlugin *np = data;
    (void) name;
    /* We are the watcher, so we are trivially also a registered host. */
    np->host_registered = TRUE;
    g_dbus_connection_emit_signal(conn, NULL, SNI_WATCHER_PATH,
        SNI_WATCHER_IFACE, "StatusNotifierHostRegistered", NULL, NULL);
}

static void on_watcher_name_lost(GDBusConnection *conn, const char *name,
                                 gpointer data)
{
    (void) conn; (void) data;
    /* Another watcher exists (e.g. snixembed still running). Nothing to draw. */
    g_message("volcanic-sni: could not own %s (another tray host is running?)",
              name);
}

/* ---- plugin glue -------------------------------------------------------- */

static void sni_reconfigure(LXPanel *panel, GtkWidget *instance)
{
    SniPlugin *np = lxpanel_plugin_get_data(instance);
    GHashTableIter it;
    gpointer v;
    (void) panel;
    if (!np)
        return;
    g_hash_table_iter_init(&it, np->items);
    while (g_hash_table_iter_next(&it, NULL, &v))
        sni_item_render((SniItem *) v);   /* new size / panel fg */
}

static void sni_destructor(gpointer data)
{
    SniPlugin *np = data;
    if (np->host_owner_id)
        g_bus_unown_name(np->host_owner_id);
    if (np->watcher_owner_id)
        g_bus_unown_name(np->watcher_owner_id);
    if (np->items)
        g_hash_table_destroy(np->items);   /* frees items (and their widgets) */
    if (np->watcher_reg_id && np->conn)
        g_dbus_connection_unregister_object(np->conn, np->watcher_reg_id);
    g_free(np);
}

static GtkWidget *sni_constructor(LXPanel *panel, config_setting_t *settings)
{
    SniPlugin *np = g_new0(SniPlugin, 1);
    np->panel = panel;
    np->settings = settings;
    np->items = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, sni_item_free);

    np->box = panel_box_new(panel, FALSE, 2);

    np->watcher_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, SNI_WATCHER_NAME,
        G_BUS_NAME_OWNER_FLAGS_REPLACE, on_bus_acquired,
        on_watcher_name_acquired, on_watcher_name_lost, np, NULL);

    char *host = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int) getpid());
    np->host_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, host,
        G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, np, NULL);
    g_free(host);

    lxpanel_plugin_set_data(np->box, np, sni_destructor);
    gtk_widget_show(np->box);
    return np->box;
}

FM_DEFINE_MODULE(lxpanel_gtk, volcanic_sni)

LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name        = N_("Volcanic Tray (SNI)"),
    .description = N_("StatusNotifierItem system tray with recoloured icons."),
    .new_instance = sni_constructor,
    .reconfigure  = sni_reconfigure,
};
