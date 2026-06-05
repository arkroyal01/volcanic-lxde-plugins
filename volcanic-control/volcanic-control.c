/*
 * volcanic-control - a centralized settings hub for the Volcanic/LXDE session.
 *
 * LXDE has no control center: settings live in scattered single-purpose apps
 * (lxappearance, lxinput, lxrandr, lxhotkey, lxsession-*, ...). This presents
 * them as one window -- an old-Mac-OS / KDE4-style grid of icons grouped under
 * category headers. Each tile launches the corresponding tool; the hub stays
 * open so you can bounce between settings.
 *
 * Discovery is "hybrid": scan every freedesktop Settings-category .desktop file
 * (so new LXDE tools appear automatically) but drop a small blocklist of heavy
 * foreign panels (KDE System Settings, CoreCtrl, ...) that don't belong here.
 *
 * GTK2 + GIO (GDesktopAppInfo), matching the rest of the Volcanic stack.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later
 */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

/* ----- categories -------------------------------------------------------- */

enum {
    GRP_APPEARANCE,
    GRP_HARDWARE,
    GRP_SESSION,
    GRP_NETWORK,
    GRP_SECURITY,
    GRP_SYSTEM,
    N_GROUPS
};

static const char *group_titles[N_GROUPS] = {
    "Appearance",
    "Input & Hardware",
    "Session & Startup",
    "Network",
    "Security",
    "System",
};

/* Settings-category entries we never want in the hub: heavy foreign panels,
 * web-launchers, oddball IMEs, and our own launcher. Matched by desktop id. */
static const char *blocklist[] = {
    "systemsettings.desktop",
    "kdesystemsettings.desktop",
    "org.kde.kcolorschemeeditor.desktop",
    "org.corectrl.CoreCtrl.desktop",
    "scim-setup.desktop",
    "silver-settings.desktop",
    "volcanic-control.desktop",     /* don't list ourselves */
    NULL
};

/* A few entries whose freedesktop categories don't map to where a user expects
 * them; pin those by desktop id. Everything else is grouped by category token. */
typedef struct { const char *id; int group; } CatOverride;
static const CatOverride overrides[] = {
    { "lxsession-edit.desktop",         GRP_SESSION },
    { "lxsession-default-apps.desktop", GRP_SESSION },
    { "libfm-pref-apps.desktop",        GRP_SESSION },
    { "lxhotkey-gtk.desktop",           GRP_HARDWARE },
    { NULL, 0 }
};

/* ----- helpers ----------------------------------------------------------- */

static gboolean id_in_list(const char *id, const char *const *list)
{
    for (; *list; list++)
        if (g_strcmp0(id, *list) == 0)
            return TRUE;
    return FALSE;
}

/* TRUE if the ';'-separated category string contains token exactly. */
static gboolean has_category(const char *cats, const char *token)
{
    if (!cats)
        return FALSE;
    gchar **v = g_strsplit(cats, ";", -1);
    gboolean found = FALSE;
    for (gchar **p = v; *p && !found; p++)
        if (g_strcmp0(*p, token) == 0)
            found = TRUE;
    g_strfreev(v);
    return found;
}

static int group_for(const char *id, const char *cats)
{
    for (const CatOverride *o = overrides; o->id; o++)
        if (g_strcmp0(id, o->id) == 0)
            return o->group;

    if (has_category(cats, "Security"))                              return GRP_SECURITY;
    if (has_category(cats, "Network"))                              return GRP_NETWORK;
    if (has_category(cats, "HardwareSettings") ||
        has_category(cats, "Printing") ||
        has_category(cats, "Mixer") ||
        has_category(cats, "Audio"))                               return GRP_HARDWARE;
    if (has_category(cats, "DesktopSettings"))                     return GRP_APPEARANCE;
    return GRP_SYSTEM;
}

static gint cmp_by_name(gconstpointer a, gconstpointer b)
{
    const char *na = g_app_info_get_name(G_APP_INFO(a));
    const char *nb = g_app_info_get_name(G_APP_INFO(b));
    gchar *fa = na ? g_utf8_casefold(na, -1) : g_strdup("");
    gchar *fb = nb ? g_utf8_casefold(nb, -1) : g_strdup("");
    gint r = g_strcmp0(fa, fb);
    g_free(fa);
    g_free(fb);
    return r;
}

/* ----- discovery --------------------------------------------------------- */

static void scan_dir(const char *appdir, GHashTable *seen, GList **lists)
{
    GDir *d = g_dir_open(appdir, 0, NULL);
    if (!d)
        return;

    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".desktop"))
            continue;
        if (g_hash_table_contains(seen, name))
            continue;                       /* earlier (higher-priority) dir won */
        g_hash_table_add(seen, g_strdup(name));

        if (id_in_list(name, blocklist))
            continue;

        gchar *path = g_build_filename(appdir, name, NULL);
        GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(path);
        g_free(path);
        if (!info)
            continue;

        const char *cats = g_desktop_app_info_get_categories(info);
        if (!g_app_info_should_show(G_APP_INFO(info)) ||
            !has_category(cats, "Settings")) {
            g_object_unref(info);
            continue;
        }

        int grp = group_for(name, cats);
        lists[grp] = g_list_prepend(lists[grp], info);   /* takes the ref */
    }
    g_dir_close(d);
}

static void discover(GList **lists)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* user dir first so a user override shadows the system copy */
    gchar *udir = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    scan_dir(udir, seen, lists);
    g_free(udir);

    const char *const *sys = g_get_system_data_dirs();
    for (; sys && *sys; sys++) {
        gchar *sdir = g_build_filename(*sys, "applications", NULL);
        scan_dir(sdir, seen, lists);
        g_free(sdir);
    }

    g_hash_table_destroy(seen);

    for (int i = 0; i < N_GROUPS; i++)
        lists[i] = g_list_sort(lists[i], cmp_by_name);
}

/* ----- UI ---------------------------------------------------------------- */

#define GRID_COLUMNS 5
#define TILE_ICON_SIZE GTK_ICON_SIZE_DIALOG   /* 48px */
#define TILE_LABEL_WIDTH 100

static void on_tile_clicked(GtkWidget *btn, gpointer data)
{
    GAppInfo *info = g_object_get_data(G_OBJECT(btn), "appinfo");
    if (!info)
        return;
    GdkAppLaunchContext *ctx = gdk_app_launch_context_new();
    GError *err = NULL;
    if (!g_app_info_launch(info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_warning("volcanic-control: launch failed: %s",
                  err ? err->message : "(unknown)");
        g_clear_error(&err);
    }
    g_object_unref(ctx);
}

static GtkWidget *make_tile(GDesktopAppInfo *info)
{
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);

    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);

    GIcon *gicon = g_app_info_get_icon(G_APP_INFO(info));
    GtkWidget *img = gicon
        ? gtk_image_new_from_gicon(gicon, TILE_ICON_SIZE)
        : gtk_image_new_from_icon_name("applications-system", TILE_ICON_SIZE);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);

    GtkWidget *lbl = gtk_label_new(g_app_info_get_name(G_APP_INFO(info)));
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_misc_set_alignment(GTK_MISC(lbl), 0.5, 0.0);
    gtk_widget_set_size_request(lbl, TILE_LABEL_WIDTH, -1);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(btn), box);

    const char *desc = g_app_info_get_description(G_APP_INFO(info));
    if (desc && *desc)
        gtk_widget_set_tooltip_text(btn, desc);

    g_object_set_data_full(G_OBJECT(btn), "appinfo",
                           g_object_ref(info), g_object_unref);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tile_clicked), NULL);
    return btn;
}

static void add_group_section(GtkWidget *vb, const char *title, GList *items)
{
    GtkWidget *hdr = gtk_label_new(NULL);
    gchar *m = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(hdr), m);
    g_free(m);
    gtk_misc_set_alignment(GTK_MISC(hdr), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vb), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), gtk_hseparator_new(), FALSE, FALSE, 0);

    guint n = g_list_length(items);
    guint rows = (n + GRID_COLUMNS - 1) / GRID_COLUMNS;
    GtkWidget *table = gtk_table_new(rows, GRID_COLUMNS, TRUE);

    guint i = 0;
    for (GList *l = items; l; l = l->next, i++) {
        guint col = i % GRID_COLUMNS;
        guint row = i / GRID_COLUMNS;
        gtk_table_attach_defaults(GTK_TABLE(table), make_tile(l->data),
                                  col, col + 1, row, row + 1);
    }
    gtk_box_pack_start(GTK_BOX(vb), table, FALSE, FALSE, 0);
}

/* ----- window geometry persistence --------------------------------------
 * KWin (and most WMs) won't remember an app's size on its own -- that needs a
 * manual per-window rule. So persist it ourselves to a tiny key file under
 * $XDG_CONFIG_HOME/volcanic-control/. */

#define DEFAULT_WIDTH  660
#define DEFAULT_HEIGHT 620

typedef struct {
    int      width;
    int      height;
    gboolean maximized;
} WinGeom;

static gchar *geometry_path(void)
{
    gchar *dir = g_build_filename(g_get_user_config_dir(), "volcanic-control", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "state.ini", NULL);
    g_free(dir);
    return path;
}

static void geometry_load(WinGeom *g)
{
    g->width = DEFAULT_WIDTH;
    g->height = DEFAULT_HEIGHT;
    g->maximized = FALSE;

    gchar *path = geometry_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        int w = g_key_file_get_integer(kf, "window", "width", &e);
        if (!e && w > 0) g->width = w;
        g_clear_error(&e);
        int h = g_key_file_get_integer(kf, "window", "height", &e);
        if (!e && h > 0) g->height = h;
        g_clear_error(&e);
        gboolean m = g_key_file_get_boolean(kf, "window", "maximized", &e);
        if (!e) g->maximized = m;
        g_clear_error(&e);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void geometry_save(const WinGeom *g)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "window", "width", g->width);
    g_key_file_set_integer(kf, "window", "height", g->height);
    g_key_file_set_boolean(kf, "window", "maximized", g->maximized);
    gchar *path = geometry_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

/* track size only while unmaximized, so we restore the "normal" size */
static gboolean on_configure(GtkWidget *w, GdkEventConfigure *e, gpointer data)
{
    WinGeom *g = data;
    if (!g->maximized) {
        g->width = e->width;
        g->height = e->height;
    }
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer data)
{
    WinGeom *g = data;
    g->maximized = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    GList *lists[N_GROUPS] = { NULL };
    discover(lists);

    WinGeom geom;
    geometry_load(&geom);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Volcanic Control Center");
    gtk_window_set_icon_name(GTK_WINDOW(win), "preferences-system");
    gtk_window_set_default_size(GTK_WINDOW(win), geom.width, geom.height);
    if (geom.maximized)
        gtk_window_maximize(GTK_WINDOW(win));
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure), &geom);
    g_signal_connect(win, "window-state-event", G_CALLBACK(on_window_state), &geom);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(win), scroll);

    GtkWidget *vb = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 12);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), vb);

    gboolean any = FALSE;
    for (int g = 0; g < N_GROUPS; g++) {
        if (!lists[g])
            continue;
        add_group_section(vb, group_titles[g], lists[g]);
        any = TRUE;
    }

    if (!any) {
        GtkWidget *empty = gtk_label_new("No settings modules found.");
        gtk_box_pack_start(GTK_BOX(vb), empty, FALSE, FALSE, 0);
    }

    /* tiles hold their own refs now; release the discovery lists */
    for (int g = 0; g < N_GROUPS; g++)
        g_list_free_full(lists[g], g_object_unref);

    gtk_widget_show_all(win);
    gtk_main();

    geometry_save(&geom);
    return 0;
}
