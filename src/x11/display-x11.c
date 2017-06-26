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

#ifdef HAVE_RANDR
#include <X11/extensions/Xrandr.h>
#endif
#include <X11/extensions/shape.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
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

  {
    int major, minor;

    x11_display->have_xsync = FALSE;

    x11_display->xsync_error_base = 0;
    x11_display->xsync_event_base = 0;

    /* I don't think we really have to fill these in */
    major = SYNC_MAJOR_VERSION;
    minor = SYNC_MINOR_VERSION;

    if (!XSyncQueryExtension (xdisplay,
                              &x11_display->xsync_event_base,
                              &x11_display->xsync_error_base) ||
        !XSyncInitialize (xdisplay,
                          &major, &minor))
      {
        x11_display->xsync_error_base = 0;
        x11_display->xsync_event_base = 0;
      }
    else
      {
        x11_display->have_xsync = TRUE;
        XSyncSetPriority (xdisplay, None, 10);
      }

    meta_verbose ("Attempted to init Xsync, found version %d.%d error base %d event base %d\n",
                  major, minor,
                  x11_display->xsync_error_base,
                  x11_display->xsync_event_base);
  }

  {
    x11_display->have_shape = FALSE;

    x11_display->shape_error_base = 0;
    x11_display->shape_event_base = 0;

    if (!XShapeQueryExtension (xdisplay,
                               &x11_display->shape_event_base,
                               &x11_display->shape_error_base))
      {
        x11_display->shape_error_base = 0;
        x11_display->shape_event_base = 0;
      }
    else
      x11_display->have_shape = TRUE;

    meta_verbose ("Attempted to init Shape, found error base %d event base %d\n",
                  x11_display->shape_error_base,
                  x11_display->shape_event_base);
  }

  {
    x11_display->have_composite = FALSE;

    x11_display->composite_error_base = 0;
    x11_display->composite_event_base = 0;

    if (!XCompositeQueryExtension (xdisplay,
                                   &x11_display->composite_event_base,
                                   &x11_display->composite_error_base))
      {
        x11_display->composite_error_base = 0;
        x11_display->composite_event_base = 0;
      }
    else
      {
        x11_display->composite_major_version = 0;
        x11_display->composite_minor_version = 0;
        if (XCompositeQueryVersion (xdisplay,
                                    &x11_display->composite_major_version,
                                    &x11_display->composite_minor_version))
          {
            x11_display->have_composite = TRUE;
          }
        else
          {
            x11_display->composite_major_version = 0;
            x11_display->composite_minor_version = 0;
          }
      }

    meta_verbose ("Attempted to init Composite, found error base %d event base %d "
                  "extn ver %d %d\n",
                  x11_display->composite_error_base,
                  x11_display->composite_event_base,
                  x11_display->composite_major_version,
                  x11_display->composite_minor_version);

    x11_display->have_damage = FALSE;

    x11_display->damage_error_base = 0;
    x11_display->damage_event_base = 0;

    if (!XDamageQueryExtension (xdisplay,
                                &x11_display->damage_event_base,
                                &x11_display->damage_error_base))
      {
        x11_display->damage_error_base = 0;
        x11_display->damage_event_base = 0;
      }
    else
      x11_display->have_damage = TRUE;

    meta_verbose ("Attempted to init Damage, found error base %d event base %d\n",
                  x11_display->damage_error_base,
                  x11_display->damage_event_base);

    x11_display->xfixes_error_base = 0;
    x11_display->xfixes_event_base = 0;

    if (XFixesQueryExtension (xdisplay,
                              &x11_display->xfixes_event_base,
                              &x11_display->xfixes_error_base))
      {
        int xfixes_major, xfixes_minor;

        XFixesQueryVersion (xdisplay, &xfixes_major, &xfixes_minor);

        if (xfixes_major * 100 + xfixes_minor < 500)
          meta_fatal ("Mutter requires XFixes 5.0");
      }
    else
      {
        meta_fatal ("Mutter requires XFixes 5.0");
      }

    meta_verbose ("Attempted to init XFixes, found error base %d event base %d\n",
                  x11_display->xfixes_error_base,
                  x11_display->xfixes_event_base);
  }

  {
    int major = 2, minor = 3;
    gboolean has_xi = FALSE;

    if (XQueryExtension (xdisplay,
                         "XInputExtension",
                         &x11_display->xinput_opcode,
                         &x11_display->xinput_error_base,
                         &x11_display->xinput_event_base))
      {
        if (XIQueryVersion (xdisplay, &major, &minor) == Success)
          {
            int version = (major * 10) + minor;
            if (version >= 22)
              has_xi = TRUE;

#ifdef HAVE_XI23
            if (version >= 23)
              x11_display->have_xinput_23 = TRUE;
#endif /* HAVE_XI23 */
          }
      }

    if (!has_xi)
      meta_fatal ("X server doesn't have the XInput extension, version 2.2 or newer\n");
  }

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

/**
 * meta_x11_display_get_xinput_opcode: (skip)
 * @display: a #MetaX11Display
 *
 */
int
meta_x11_display_get_xinput_opcode (MetaX11Display *display)
{
  return display->xinput_opcode;
}

int
meta_x11_display_get_damage_event_base (MetaX11Display *display)
{
  return display->damage_event_base;
}

gboolean
meta_x11_display_has_shape (MetaX11Display *display)
{
  return META_X11_DISPLAY_HAS_SHAPE (display);
}

int
meta_x11_display_get_shape_event_base (MetaX11Display *display)
{
  return display->shape_event_base;
}
