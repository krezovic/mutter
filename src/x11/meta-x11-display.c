/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2002, 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2003, 2004 Rob Adams
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

/**
 * SECTION:display
 * @title: MetaX11Display
 * @short_description: Mutter X display handler
 *
 * The X11 display is represented as a #MetaX11Display struct.
 */

#include "config.h"

#include "x11/meta-x11-display-private.h"

G_DEFINE_TYPE (MetaX11Display, meta_x11_display, G_TYPE_OBJECT)

static void
meta_x11_display_class_init (MetaX11DisplayClass *klass)
{
}

static void
meta_x11_display_init (MetaX11Display *x11_display)
{
}

/**
 * meta_x11_display_open:
 *
 * Opens a new X11 display, sets it up, initialises all the X extensions
 * we will need, and adds it to the list of displays.
 *
 * Returns: #MetaX11Display if the display was opened successfully,
 * and %NULL otherwise-- that is, if the display doesn't exist or
 *  it already has a window manager.
 */
MetaX11Display *
meta_x11_display_open (MetaDisplay *display)
{
  MetaX11Display *x11_display;

  x11_display = g_object_new (META_TYPE_X11_DISPLAY, NULL);
  x11_display->display = display;

  return x11_display;
}

void
meta_x11_display_close (MetaX11Display *x11_display)
{
  g_assert (x11_display != NULL);

  g_object_unref (x11_display);
}
