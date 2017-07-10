/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2008 Iain Holmes
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef META_DISPLAY_X11_H
#define META_DISPLAY_X11_H

#include <glib-object.h>
#include <X11/Xlib.h>

#include <meta/common.h>
#include <meta/prefs.h>
#include <meta/types.h>

typedef struct _MetaX11DisplayClass MetaX11DisplayClass;

#define META_TYPE_X11_DISPLAY              (meta_x11_display_get_type ())
#define META_X11_DISPLAY(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), META_TYPE_X11_DISPLAY, MetaX11Display))
#define META_X11_DISPLAY_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), META_TYPE_X11_DISPLAY, MetaX111DisplayClass))
#define META_IS_X11_DISPLAY(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), META_TYPE_X11_DISPLAY))
#define META_IS_X11_DISPLAY_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), META_TYPE_X11_DISPLAY))
#define META_X11_DISPLAY_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), META_TYPE_X11_DISPLAY, MetaX11DisplayClass))

GType meta_x11_display_get_type (void) G_GNUC_CONST;

int meta_x11_display_get_xinput_opcode (MetaX11Display *display);

gboolean meta_x11_display_has_shape (MetaX11Display *display);

int meta_x11_display_get_damage_event_base (MetaX11Display *display);
int meta_x11_display_get_shape_event_base (MetaX11Display *display);

Window meta_x11_display_get_xroot (MetaX11Display *x11_display);

void meta_x11_display_set_cm_selection (MetaX11Display *x11_display);

gboolean meta_x11_display_xwindow_is_a_no_focus_window (MetaX11Display *x11_display,
                                                        Window xwindow);

int meta_x11_display_get_screen_number (MetaX11Display *x11_display);

#endif
