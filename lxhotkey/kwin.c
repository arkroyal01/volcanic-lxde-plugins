/*
 * lxhotkey WM backend plugin for KWin (Volcanic / kwin_x11).
 *
 * Bridges LXDE's lxhotkey editor to KWin's global shortcuts, which live in
 * ~/.config/kglobalshortcutsrc under the [kwin] component in the format:
 *
 *     ActionName=current,default,FriendlyName
 *
 * where `current`/`default` are either "none" or e.g. "Meta+Ctrl+A", with a
 * literal "\t" separating alternate bindings and "\," escaping commas inside
 * the (localized) friendly text.
 *
 * The plugin registers under the name "KWin" so lxhotkey matches it against the
 * running WM's _NET_WM_NAME (case-insensitive). This clears the
 * "Could not find a plugin for window manager KWin" error.
 *
 * NOTE: kglobalshortcutsrc is a KConfig file, not a GKeyFile — it uses
 * double-bracket groups ("[services][foo.desktop]") and KConfig escaping that
 * GLib's GKeyFile rejects/garbles. So we parse and edit it line-by-line,
 * touching only the [kwin] group and preserving every other line verbatim.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later (same as lxhotkey, whose API headers this uses)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <lxhotkey/lxhotkey.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <string.h>

#include <fnmatch.h>

#define KWIN_GROUP "[kwin]"

#define LXKEYS_KWIN_ERROR lxhotkey_kwin_error_quark()
static GQuark lxhotkey_kwin_error_quark(void)
{
    static GQuark q = 0;
    if G_UNLIKELY(q == 0)
        q = g_quark_from_static_string("lxhotkey-kwin-error");
    return q;
}
enum { LXKEYS_FILE_ERROR, LXKEYS_PARSE_ERROR };

typedef struct {
    char *path;          /* ~/.config/kglobalshortcutsrc                       */
    GPtrArray *lines;    /* gchar* per file line (owned), no trailing newline  */
    GList *actions;      /* LXHotkeyGlobal*; data1 = GUINT_TO_POINTER(line+1)  */
    GList *available;    /* LXHotkeyAttr* (action descriptors, transfer none)  */
} KWinCfg;

/* ---- LXHotkey data helpers ------------------------------------------- */

static void attr_free(LXHotkeyAttr *a)
{
    g_free(a->name);
    g_list_free_full(a->values, g_free);
    g_free(a->desc);
    g_slice_free(LXHotkeyAttr, a);
}

static void global_free(LXHotkeyGlobal *g)
{
    g_list_free_full(g->actions, (GDestroyNotify)attr_free);
    g_free(g->accel1);
    g_free(g->accel2);
    g_free(g);
}

/* ---- accelerator conversion ------------------------------------------ */
/* KDE uses "Meta+Ctrl+A" (Meta == Super); lxhotkey wants GDK format
 * "<Super><Control>A". Key names are passed through verbatim; only the
 * modifier tokens are translated, which keeps set/get round-trips stable. */

static gchar *kde_to_gdk(const char *kde)
{
    if (!kde || !*kde || g_ascii_strcasecmp(kde, "none") == 0)
        return NULL;

    gchar **parts = g_strsplit(kde, "+", -1);
    guint n = g_strv_length(parts);
    GString *s = g_string_sized_new(16);

    for (guint i = 0; i < n; i++) {
        const char *p = parts[i];
        if (i + 1 < n) {                 /* a modifier */
            if (!g_ascii_strcasecmp(p, "Meta"))       g_string_append(s, "<Super>");
            else if (!g_ascii_strcasecmp(p, "Ctrl"))  g_string_append(s, "<Control>");
            else if (!g_ascii_strcasecmp(p, "Alt"))   g_string_append(s, "<Alt>");
            else if (!g_ascii_strcasecmp(p, "Shift")) g_string_append(s, "<Shift>");
            else g_string_append_printf(s, "<%s>", p);
        } else {                         /* the key itself */
            g_string_append(s, p);
        }
    }
    g_strfreev(parts);
    return g_string_free(s, FALSE);
}

static gchar *gdk_to_kde(const char *gdk)
{
    if (!gdk || !*gdk)
        return g_strdup("none");

    GString *s = g_string_sized_new(16);
    const char *p = gdk;
    gboolean first = TRUE;

    while (*p) {
        if (*p == '<') {
            const char *end = strchr(p, '>');
            if (!end) break;
            gchar *mod = g_strndup(p + 1, end - p - 1);
            const char *kde = NULL;
            if (!g_ascii_strcasecmp(mod, "Super") || !g_ascii_strcasecmp(mod, "Meta") ||
                !g_ascii_strcasecmp(mod, "Mod4"))                         kde = "Meta";
            else if (!g_ascii_strncasecmp(mod, "Control", 4) ||
                     !g_ascii_strcasecmp(mod, "Ctrl") ||
                     !g_ascii_strcasecmp(mod, "Primary"))                 kde = "Ctrl";
            else if (!g_ascii_strcasecmp(mod, "Alt") || !g_ascii_strcasecmp(mod, "Mod1")) kde = "Alt";
            else if (!g_ascii_strcasecmp(mod, "Shift"))                   kde = "Shift";
            g_free(mod);
            if (kde) {
                if (!first) g_string_append_c(s, '+');
                g_string_append(s, kde);
                first = FALSE;
            }
            p = end + 1;
        } else {                          /* the rest is the key */
            if (!first) g_string_append_c(s, '+');
            g_string_append(s, p);
            break;
        }
    }
    if (s->len == 0) {
        g_string_free(s, TRUE);
        return g_strdup("none");
    }
    return g_string_free(s, FALSE);
}

/* ---- raw value (un)parsing ------------------------------------------- */
/* Split a kglobalshortcutsrc value into its 3 comma fields, honouring the
 * KConfig "\" escape so escaped commas inside the friendly text are kept.
 * Escape pairs are copied verbatim (so the embedded "\t" alt-separator and
 * "\," survive into the field text). */
static void split3(const char *raw, gchar **f0, gchar **f1, gchar **f2)
{
    gchar *fields[3] = { NULL, NULL, NULL };
    int fi = 0;
    GString *cur = g_string_sized_new(16);

    for (const char *p = raw ? raw : ""; *p; p++) {
        if (*p == '\\' && p[1]) {
            g_string_append_c(cur, *p);
            g_string_append_c(cur, p[1]);
            p++;
        } else if (*p == ',' && fi < 2) {
            fields[fi++] = g_string_free(cur, FALSE);
            cur = g_string_sized_new(16);
        } else {
            g_string_append_c(cur, *p);
        }
    }
    fields[fi] = g_string_free(cur, FALSE);

    *f0 = fields[0] ? fields[0] : g_strdup("");
    *f1 = fields[1] ? fields[1] : g_strdup("");
    *f2 = fields[2] ? fields[2] : g_strdup("");
}

/* ---- model <-> file lines -------------------------------------------- */

static void clear_model(KWinCfg *cfg)
{
    g_list_free_full(cfg->actions, (GDestroyNotify)global_free);
    g_list_free_full(cfg->available, (GDestroyNotify)attr_free);
    cfg->actions = NULL;
    cfg->available = NULL;
}

/* Parse the [kwin] group out of cfg->lines into the model. */
static void parse_lines(KWinCfg *cfg)
{
    gboolean in_kwin = FALSE;

    for (guint i = 0; i < cfg->lines->len; i++) {
        const char *line = g_ptr_array_index(cfg->lines, i);

        if (line[0] == '[') {                       /* group header */
            in_kwin = (g_strcmp0(line, KWIN_GROUP) == 0);
            continue;
        }
        if (!in_kwin || line[0] == '\0' || line[0] == '#')
            continue;

        const char *eq = strchr(line, '=');
        if (!eq)
            continue;

        gchar *key = g_strstrip(g_strndup(line, eq - line));
        if (key[0] == '\0' || g_str_has_prefix(key, "_k_")) { /* meta keys */
            g_free(key);
            continue;
        }

        gchar *f0, *f1, *f2;
        split3(eq + 1, &f0, &f1, &f2);
        gchar **alts = g_strsplit(f0, "\\t", -1);     /* alternate bindings */

        LXHotkeyAttr *act = g_slice_new0(LXHotkeyAttr);
        act->name = g_strdup(key);
        act->desc = (f2 && *f2) ? g_strdup(f2) : NULL;

        LXHotkeyGlobal *glob = g_new0(LXHotkeyGlobal, 1);
        glob->actions = g_list_prepend(NULL, act);
        glob->accel1 = kde_to_gdk(alts[0]);
        if (alts[0] && alts[1])
            glob->accel2 = kde_to_gdk(alts[1]);
        glob->data1 = GUINT_TO_POINTER(i + 1);        /* owning line, 1-based */
        cfg->actions = g_list_prepend(cfg->actions, glob);

        LXHotkeyAttr *avail = g_slice_new0(LXHotkeyAttr);
        avail->name = g_strdup(key);
        avail->desc = (f2 && *f2) ? g_strdup(f2) : NULL;
        cfg->available = g_list_prepend(cfg->available, avail);

        g_strfreev(alts);
        g_free(f0); g_free(f1); g_free(f2);
        g_free(key);
    }
    cfg->actions = g_list_reverse(cfg->actions);
    cfg->available = g_list_reverse(cfg->available);
}

/* ---- plugin entry points --------------------------------------------- */

static void kwincfg_free(gpointer config)
{
    KWinCfg *cfg = config;
    if (!cfg)
        return;
    clear_model(cfg);
    if (cfg->lines)
        g_ptr_array_unref(cfg->lines);
    g_free(cfg->path);
    g_free(cfg);
}

static gboolean read_lines(KWinCfg *cfg, GError **error)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(cfg->path, &contents, NULL, error))
        return FALSE;

    if (cfg->lines)
        g_ptr_array_unref(cfg->lines);
    cfg->lines = g_ptr_array_new_with_free_func(g_free);

    gchar **split = g_strsplit(contents, "\n", -1);
    for (guint i = 0; split[i]; i++)
        g_ptr_array_add(cfg->lines, g_strdup(split[i]));
    g_strfreev(split);
    g_free(contents);
    return TRUE;
}

static gpointer kwincfg_load(gpointer config, GError **error)
{
    KWinCfg *cfg = config;

    if (cfg)
        clear_model(cfg);
    else {
        cfg = g_new0(KWinCfg, 1);
        cfg->path = g_build_filename(g_get_user_config_dir(), "kglobalshortcutsrc", NULL);
    }

    if (!read_lines(cfg, error)) {
        kwincfg_free(cfg);
        return NULL;
    }
    parse_lines(cfg);
    if (cfg->actions == NULL) {
        g_set_error_literal(error, LXKEYS_KWIN_ERROR, LXKEYS_FILE_ERROR,
                            _("No [kwin] shortcuts found in kglobalshortcutsrc."));
        kwincfg_free(cfg);
        return NULL;
    }
    return cfg;
}

static gboolean kwincfg_save(gpointer config, GError **error)
{
    KWinCfg *cfg = config;

    /* rejoin verbatim — only [kwin] lines were ever touched */
    g_ptr_array_add(cfg->lines, NULL);             /* NULL-terminate for strjoinv */
    gchar *out = g_strjoinv("\n", (gchar **)cfg->lines->pdata);
    g_ptr_array_remove_index(cfg->lines, cfg->lines->len - 1);

    gboolean ok = g_file_set_contents(cfg->path, out, -1, error);
    g_free(out);
    if (!ok)
        return FALSE;

    /* Apply: kglobalacceld owns the live state and only re-reads the file at
     * startup, so bounce it. It is D-Bus activatable; we also relaunch eagerly.
     * Best-effort, non-fatal. */
    g_spawn_command_line_sync("kquitapp6 kglobalacceld", NULL, NULL, NULL, NULL);
    g_spawn_command_line_async("/usr/lib/kglobalacceld", NULL);
    return TRUE;
}

static GList *kwincfg_get_wm_keys(gpointer config, const char *mask, GError **error)
{
    KWinCfg *cfg = config;
    GList *list = NULL, *l;

    for (l = cfg->actions; l; l = l->next) {
        LXHotkeyGlobal *g = l->data;
        if (g->accel1 == NULL)            /* unbound: skip in the key list */
            continue;
        if (mask == NULL || fnmatch(mask, g->accel1, 0) == 0 ||
            (g->accel2 && fnmatch(mask, g->accel2, 0) == 0))
            list = g_list_prepend(list, g);
    }
    return g_list_reverse(list);
}

static GList *kwincfg_get_wm_actions(gpointer config, GError **error)
{
    KWinCfg *cfg = config;
    return cfg->available;                /* transfer none */
}

static gboolean kwincfg_set_wm_key(gpointer config, LXHotkeyGlobal *data, GError **error)
{
    KWinCfg *cfg = config;
    LXHotkeyAttr *first;
    LXHotkeyGlobal *target = NULL;
    GList *l;

    if (!data->actions || !(first = data->actions->data) || !first->name) {
        g_set_error_literal(error, LXKEYS_KWIN_ERROR, LXKEYS_PARSE_ERROR,
                            _("Keybinding must name a KWin action."));
        return FALSE;
    }
    for (l = cfg->actions; l; l = l->next) {
        LXHotkeyGlobal *g = l->data;
        LXHotkeyAttr *a = g->actions ? g->actions->data : NULL;
        if (a && g_strcmp0(a->name, first->name) == 0) { target = g; break; }
    }
    if (!target) {
        g_set_error(error, LXKEYS_KWIN_ERROR, LXKEYS_PARSE_ERROR,
                    _("Action '%s' isn't a known KWin shortcut."), first->name);
        return FALSE;
    }

    guint idx = GPOINTER_TO_UINT(target->data1);
    if (idx == 0 || idx > cfg->lines->len) {
        g_set_error_literal(error, LXKEYS_KWIN_ERROR, LXKEYS_PARSE_ERROR,
                            _("Internal error: lost the config line."));
        return FALSE;
    }
    idx--;

    /* preserve default + friendly fields, rewrite only the current field */
    const char *line = g_ptr_array_index(cfg->lines, idx);
    const char *eq = strchr(line, '=');
    gchar *f0, *f1, *f2;
    split3(eq ? eq + 1 : "", &f0, &f1, &f2);

    gchar *cur;
    if (data->accel1 == NULL) {
        cur = g_strdup("none");
    } else if (data->accel2) {
        gchar *k1 = gdk_to_kde(data->accel1), *k2 = gdk_to_kde(data->accel2);
        cur = g_strdup_printf("%s\\t%s", k1, k2);
        g_free(k1); g_free(k2);
    } else {
        cur = gdk_to_kde(data->accel1);
    }

    gchar *newline = g_strdup_printf("%s=%s,%s,%s", first->name, cur, f1, f2);
    g_ptr_array_index(cfg->lines, idx) = newline;
    g_free((gchar *)line);

    /* keep the in-memory model in sync */
    g_free(target->accel1); g_free(target->accel2);
    target->accel1 = data->accel1 ? g_strdup(data->accel1) : NULL;
    target->accel2 = data->accel2 ? g_strdup(data->accel2) : NULL;

    g_free(cur);
    g_free(f0); g_free(f1); g_free(f2);
    return TRUE;
}

FM_DEFINE_MODULE(lxhotkey, KWin)

LXHotkeyPluginInit fm_module_init_lxhotkey = {
    .load           = kwincfg_load,
    .save           = kwincfg_save,
    .free           = kwincfg_free,
    .get_wm_keys    = kwincfg_get_wm_keys,
    .set_wm_key     = kwincfg_set_wm_key,
    .get_wm_actions = kwincfg_get_wm_actions,
    /* app-launch shortcuts (the KDE "services"/khotkeys component) are not
     * handled yet; leaving these NULL makes lxhotkey report them unsupported. */
};
