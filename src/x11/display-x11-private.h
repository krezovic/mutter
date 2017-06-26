/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Mutter X display handler */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2002 Red Hat, Inc.
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
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

#ifndef META_DISPLAY_X11_PRIVATE_H
#define META_DISPLAY_X11_PRIVATE_H

#ifndef PACKAGE
#error "config.h not included"
#endif

#include <glib.h>
#include <X11/Xlib.h>

#include <meta/common.h>
#include <meta/types.h>

#include "display-private.h"

#include <x11/display-x11.h>

struct _MetaX11Display
{
  GObject parent_instance;

  MetaDisplay *display;

  char *name;
  Display *xdisplay;

  /* Pull in all the names of atoms as fields; we will intern them when the
   * class is constructed.
   */
#define item(x)  Atom atom_##x;
#include <x11/atomnames.h>
#undef item
};

struct _MetaX11DisplayClass
{
  GObjectClass parent_class;
};

gboolean meta_x11_display_open  (MetaDisplay    *display);
void     meta_x11_display_close (MetaX11Display *display,
                                 guint32         timestamp);

MetaX11Display *meta_get_x11_display (void);

#endif
