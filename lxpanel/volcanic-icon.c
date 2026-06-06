/*
 * volcanic-icon - recoloring icon loader shared by the Volcanic LXDE plugins.
 * See volcanic-icon.h for the why.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later
 */

#include "volcanic-icon.h"
#include <string.h>

void volcanic_panel_fg(LXPanel *panel, GdkColor *out)
{
    /* Default: opaque white -- the right choice for a typical dark panel, and the
     * fallback when the panel uses no custom font colour. (panel_get_defstyle()'s
     * fg is the GTK *theme* text colour, not the panel's configured fontcolor, so
     * it's wrong here -- it's dark under a light GTK theme.) */
    out->red = out->green = out->blue = 0xffff;
    out->pixel = 0;
    if (!panel)
        return;

    /* lxpanel has no public getter for the configured panel font colour, but its
     * label helper bakes it into the markup as color="#rrggbb" when custom_color
     * is requested and the panel has usefontcolor set. Probe a throwaway label and
     * parse it back -- same trick the popup uses for the panel font size. */
    GtkWidget *probe = gtk_label_new(NULL);
    g_object_ref_sink(probe);
    lxpanel_draw_label_text(panel, probe, "0",
                            FALSE /*bold*/, 1.0 /*size factor*/, TRUE /*custom color*/);
    const char *markup = gtk_label_get_label(GTK_LABEL(probe));
    const char *c = markup ? strstr(markup, "color=\"#") : NULL;
    if (c) {
        char hex[8];                         /* "#rrggbb" + NUL */
        GdkColor parsed;
        g_strlcpy(hex, c + strlen("color=\""), sizeof hex);
        if (gdk_color_parse(hex, &parsed))
            *out = parsed;
    }
    g_object_unref(probe);
}

/* Rasterise (possibly modified) SVG bytes to a square pixbuf. Uses an untyped
 * loader so gdk-pixbuf content-sniffs the SVG module -- on this system the svg
 * loader resolves by sniffing even though it is absent from loaders.cache, so
 * loading it by name ("svg") is unreliable. set_size forces the output size. */
static GdkPixbuf *render_svg(const char *data, gsize len, int size)
{
    GError *err = NULL;
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
    GdkPixbuf *pix = NULL;

    gdk_pixbuf_loader_set_size(ld, size, size);
    if (gdk_pixbuf_loader_write(ld, (const guchar *) data, len, &err) &&
        gdk_pixbuf_loader_close(ld, &err)) {
        pix = gdk_pixbuf_loader_get_pixbuf(ld);   /* owned by the loader */
        if (pix)
            g_object_ref(pix);
    } else {
        g_clear_error(&err);
    }
    g_object_unref(ld);
    return pix;
}

/* If @svg is a stylable Breeze icon, return a copy with an !important override
 * of .ColorScheme-Text injected just before the closing </svg> (later +
 * !important beats the embedded rule regardless of librsvg's cascade order).
 * Only the Text colour is touched, so multi-colour status icons keep their
 * semantic Highlight/Positive/Negative colours. Returns NULL if not stylable. */
static char *recolor_svg(const char *svg, const GdkColor *fg, gsize *out_len)
{
    if (!strstr(svg, "ColorScheme-Text"))
        return NULL;                         /* not a recolourable mono icon */

    const char *tail = g_strrstr(svg, "</svg>");
    if (!tail)
        return NULL;

    char *css = g_strdup_printf(
        "<style type=\"text/css\">.ColorScheme-Text{color:#%02x%02x%02x"
        " !important;}</style>",
        fg->red >> 8, fg->green >> 8, fg->blue >> 8);

    GString *s = g_string_new_len(svg, tail - svg);
    g_string_append(s, css);
    g_string_append(s, tail);
    g_free(css);

    *out_len = s->len;
    return g_string_free(s, FALSE);
}

GdkPixbuf *volcanic_icon_pixbuf(GtkIconTheme *theme, const char *icon_name,
                                int pixel_size, const GdkColor *fg)
{
    if (!theme)
        theme = gtk_icon_theme_get_default();

    /* FORCE_SVG so a stylable icon comes back as the SVG file (which we can
     * recolour) rather than a pre-rendered raster; retry plainly if that misses. */
    GtkIconInfo *info = gtk_icon_theme_lookup_icon(theme, icon_name, pixel_size,
                                                   GTK_ICON_LOOKUP_FORCE_SVG);
    if (!info)
        info = gtk_icon_theme_lookup_icon(theme, icon_name, pixel_size, 0);
    if (!info)
        return NULL;

    GdkPixbuf *pix = NULL;
    const char *file = gtk_icon_info_get_filename(info);
    if (fg && file && g_str_has_suffix(file, ".svg")) {
        char *svg = NULL;
        gsize len = 0;
        if (g_file_get_contents(file, &svg, &len, NULL)) {
            gsize rlen = 0;
            char *recol = recolor_svg(svg, fg, &rlen);
            if (recol) {
                pix = render_svg(recol, rlen, pixel_size);
                g_free(recol);
            }
            g_free(svg);
        }
    }
    if (!pix)                                /* not stylable, or anything failed */
        pix = gtk_icon_info_load_icon(info, NULL);

    gtk_icon_info_free(info);                /* GTK2 */
    return pix;
}

void volcanic_icon_image_set(GtkImage *img, GtkIconTheme *theme,
                             const char *icon_name, int pixel_size,
                             const GdkColor *fg)
{
    GdkPixbuf *pix = volcanic_icon_pixbuf(theme, icon_name, pixel_size, fg);
    if (pix) {
        gtk_image_set_from_pixbuf(img, pix);
        g_object_unref(pix);
    } else {
        gtk_image_set_from_icon_name(img, icon_name, GTK_ICON_SIZE_BUTTON);
    }
}
