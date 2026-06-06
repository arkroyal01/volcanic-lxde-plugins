/*
 * volcanic-icon - recoloring icon loader shared by the Volcanic LXDE plugins.
 *
 * Breeze (and other KDE) monochrome icons are "stylable" SVGs: they carry an
 * embedded stylesheet
 *
 *     .ColorScheme-Text { color:#232629; }     +     fill="currentColor"
 *
 * which KDE/Qt's icon engine (KIconLoader) rewrites to the active text colour
 * at load time. GTK has no such step, so it rasterises the SVG verbatim and the
 * icon stays near-black -- invisible on a dark panel. These helpers do what
 * KIconLoader does: inject the wanted text colour into the SVG before handing it
 * to gdk-pixbuf, so panel icons follow the panel foreground. Full-colour and
 * raster icons are loaded unchanged.
 *
 * Copyright (C) 2026 Ark Royal <awright42mk1@protonmail.com>
 * License: GPL-2.0-or-later
 */

#ifndef VOLCANIC_ICON_H
#define VOLCANIC_ICON_H

#include <gtk/gtk.h>
#include <lxpanel/panel.h>

G_BEGIN_DECLS

/* The foreground to recolour to when drawing on the panel: the panel's
 * configured font colour (lxpanel keeps it in its default style fg), falling
 * back to white -- the sane default for a dark panel. */
void volcanic_panel_fg(LXPanel *panel, GdkColor *out);

/* Load @icon_name from @theme (NULL = default) at @pixel_size px. If the icon is
 * a stylable Breeze-type SVG it is recoloured to @fg (NULL = no recolour, load
 * as-is). Returns a new pixbuf (transfer full), or NULL if the icon is missing. */
GdkPixbuf *volcanic_icon_pixbuf(GtkIconTheme *theme, const char *icon_name,
                                int pixel_size, const GdkColor *fg);

/* Convenience: set such an icon on an existing GtkImage. Falls back to the plain
 * named icon if recolouring/loading fails so something always shows. */
void volcanic_icon_image_set(GtkImage *img, GtkIconTheme *theme,
                             const char *icon_name, int pixel_size,
                             const GdkColor *fg);

G_END_DECLS

#endif /* VOLCANIC_ICON_H */
