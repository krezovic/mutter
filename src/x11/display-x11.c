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

#include <config.h>

#include "display-x11-private.h"

#include "display-private.h"
#include "frame.h"
#include "util-private.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>

#ifdef HAVE_WAYLAND
#include "wayland/meta-xwayland-private.h"
#endif

G_DEFINE_TYPE(MetaX11Display, meta_x11_display, G_TYPE_OBJECT);

static void
meta_x11_display_class_init (MetaX11DisplayClass *klass)
{
}

static void
meta_x11_display_init (MetaX11Display *disp)
{
}

/**
 * meta_x11_display_open:
 *
 * Opens a new X11 display, sets it up, initialises all the X extensions
 * we will need, and adds it to the list of displays.
 *
 * Returns: %TRUE if the display was opened successfully, and %FALSE
 * otherwise-- that is, if the display doesn't exist or it already
 * has a window manager.
 */
gboolean
meta_x11_display_open (MetaDisplay *display)
{
  MetaX11Display *x11_display;
  Display *xdisplay;
  int i;

  /* A list of all atom names, so that we can intern them in one go. */
  const char *atom_names[] = {
#define item(x) #x,
#include <x11/atomnames.h>
#undef item
  };
  Atom atoms[G_N_ELEMENTS(atom_names)];

  meta_verbose ("Opening display '%s'\n", XDisplayName (NULL));

  xdisplay = meta_ui_get_display ();

  if (xdisplay == NULL)
    {
      meta_warning (_("Failed to open X Window System display “%s”\n"),
		    XDisplayName (NULL));
      return FALSE;
    }

#ifdef HAVE_WAYLAND
  if (meta_is_wayland_compositor ())
    meta_xwayland_complete_init ();
#endif

  if (meta_is_syncing ())
    XSynchronize (xdisplay, True);

  x11_display = g_object_new (META_TYPE_X11_DISPLAY, NULL);

  x11_display->display = display;
  /* So functions that use meta_get_x11_display () before this function
   * returns don't break */
  display->x11_display = x11_display;

  /* here we use XDisplayName which is what the user
   * probably put in, vs. DisplayString(display) which is
   * canonicalized by XOpenDisplay()
   */
  x11_display->name = g_strdup (XDisplayName (NULL));
  x11_display->xdisplay = xdisplay;

  meta_verbose ("Creating %d atoms\n", (int) G_N_ELEMENTS (atom_names));
  XInternAtoms (xdisplay, (char **)atom_names, G_N_ELEMENTS (atom_names),
                False, atoms);

  i = 0;
#define item(x) x11_display->atom_##x = atoms[i++];
#include <x11/atomnames.h>
#undef item

  i = 0;
/* XXX: Transitional */
#define item(x) display->atom_##x = atoms[i++];
#include <x11/atomnames.h>
#undef item

  return TRUE;
}

void
meta_x11_display_close (MetaX11Display  *display,
                        guint32          timestamp)
{
  g_assert (display != NULL);

  display->display->x11_display = NULL;

  XFlush (display->xdisplay);

  g_free (display->name);

  g_object_unref (display);
}

MetaX11Display*
meta_get_x11_display (void)
{
  return meta_get_display()->x11_display;
}
