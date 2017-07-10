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

#include "bell.h"
#include "display-private.h"
#include "frame.h"
#include "meta-cursor-tracker-private.h"
#include "util-private.h"
#include "workspace-private.h"

#include "backends/meta-logical-monitor.h"
#include "backends/x11/meta-backend-x11.h"

#include <meta/errors.h>
#include <meta/main.h>
#include <meta/meta-backend.h>

#ifdef HAVE_RANDR
#include <X11/extensions/Xrandr.h>
#endif
#include <X11/extensions/shape.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>

#include "x11/events.h"
#include "x11/group-props.h"
#include "x11/window-props.h"
#include "x11/xprops.h"

#ifdef HAVE_WAYLAND
#include "wayland/meta-xwayland-private.h"
#endif

G_DEFINE_TYPE(MetaX11Display, meta_x11_display, G_TYPE_OBJECT);

static GQuark quark_x11_display_logical_monitor_data = 0;

typedef struct _MetaX11DisplayLogicalMonitorData
{
  int xinerama_index;
} MetaX11DisplayLogicalMonitorData;

static const char *gnome_wm_keybindings = "Mutter";
static const char *net_wm_name = "Mutter";

static void update_cursor_theme (void);

static void prefs_changed_callback (MetaPreference pref,
				    void          *data);

static char* get_screen_name (Display *xdisplay,
                              int      number);

static void on_monitors_changed (MetaDisplay    *display,
                                 MetaX11Display *x11_display);

/* The guard window allows us to leave minimized windows mapped so
 * that compositor code may provide live previews of them.
 * Instead of being unmapped/withdrawn, they get pushed underneath
 * the guard window. We also select events on the guard window, which
 * should effectively be forwarded to events on the background actor,
 * providing that the scene graph is set up correctly.
 */
static Window
create_guard_window (MetaX11Display *x11_display)
{
  MetaDisplay *display = x11_display->display;
  Display *xdisplay = x11_display->xdisplay;
  XSetWindowAttributes attributes;
  Window guard_window;
  gulong create_serial;

  attributes.event_mask = NoEventMask;
  attributes.override_redirect = True;

  /* We have to call record_add() after we have the new window ID,
   * so save the serial for the CreateWindow request until then */
  create_serial = XNextRequest(xdisplay);
  guard_window =
    XCreateWindow (xdisplay,
		   x11_display->xroot,
		   0, /* x */
		   0, /* y */
		   display->rect.width,
		   display->rect.height,
		   0, /* border width */
		   0, /* depth */
		   InputOnly, /* class */
		   CopyFromParent, /* visual */
		   CWEventMask|CWOverrideRedirect,
		   &attributes);

  /* https://bugzilla.gnome.org/show_bug.cgi?id=710346 */
  XStoreName (xdisplay, guard_window, "mutter guard window");

  {
    if (!meta_is_wayland_compositor ())
      {
        MetaBackendX11 *backend = META_BACKEND_X11 (meta_get_backend ());
        Display *backend_xdisplay = meta_backend_x11_get_xdisplay (backend);
        unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
        XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

        XISetMask (mask.mask, XI_ButtonPress);
        XISetMask (mask.mask, XI_ButtonRelease);
        XISetMask (mask.mask, XI_Motion);

        /* Sync on the connection we created the window on to
         * make sure it's created before we select on it on the
         * backend connection. */
        XSync (xdisplay, False);

        XISelectEvents (backend_xdisplay, guard_window, &mask, 1);
      }
  }

  meta_stack_tracker_record_add (display->stack_tracker,
                                 guard_window,
                                 create_serial);

  meta_stack_tracker_lower (display->stack_tracker,
                            guard_window);
  XMapWindow (xdisplay, guard_window);
  return guard_window;
}

static Window
take_manager_selection (MetaX11Display *x11_display,
                        Window          xroot,
                        Atom            manager_atom,
                        int             timestamp,
                        gboolean        should_replace)
{
  Display *xdisplay = x11_display->xdisplay;
  Window current_owner, new_owner;

  current_owner = XGetSelectionOwner (xdisplay, manager_atom);
  if (current_owner != None)
    {
      XSetWindowAttributes attrs;

      if (should_replace)
        {
          /* We want to find out when the current selection owner dies */
          meta_error_trap_push ();
          attrs.event_mask = StructureNotifyMask;
          XChangeWindowAttributes (xdisplay, current_owner, CWEventMask, &attrs);
          if (meta_error_trap_pop_with_return () != Success)
            current_owner = None; /* don't wait for it to die later on */
        }
      else
        {
          meta_warning (_("Display “%s” already has a window manager; try using the --replace option to replace the current window manager."),
                        x11_display->name);
          return None;
        }
    }

  /* We need SelectionClear and SelectionRequest events on the new owner,
   * but those cannot be masked, so we only need NoEventMask.
   */
  new_owner = meta_create_offscreen_window (xdisplay, xroot, NoEventMask);

  XSetSelectionOwner (xdisplay, manager_atom, new_owner, timestamp);

  if (XGetSelectionOwner (xdisplay, manager_atom) != new_owner)
    {
      meta_warning ("Could not acquire selection: %s", XGetAtomName (xdisplay, manager_atom));
      return None;
    }

  {
    /* Send client message indicating that we are now the selection owner */
    XClientMessageEvent ev;

    ev.type = ClientMessage;
    ev.window = xroot;
    ev.message_type = x11_display->atom_MANAGER;
    ev.format = 32;
    ev.data.l[0] = timestamp;
    ev.data.l[1] = manager_atom;

    XSendEvent (xdisplay, xroot, False, StructureNotifyMask, (XEvent *) &ev);
  }

  /* Wait for old window manager to go away */
  if (current_owner != None)
    {
      XEvent event;

      /* We sort of block infinitely here which is probably lame. */

      meta_verbose ("Waiting for old window manager to exit\n");
      do
        XWindowEvent (xdisplay, current_owner, StructureNotifyMask, &event);
      while (event.type != DestroyNotify);
    }

  return new_owner;
}

/**
 * meta_set_wm_name: (skip)
 * @wm_name: value for _NET_WM_NAME
 *
 * Set the value to use for the _NET_WM_NAME property. To take effect,
 * it is necessary to call this function before meta_init().
 */
void
meta_set_wm_name (const char *wm_name)
{
  g_return_if_fail (meta_get_display () == NULL);

  net_wm_name = wm_name;
}

/**
 * meta_set_gnome_wm_keybindings: (skip)
 * @wm_keybindings: value for _GNOME_WM_KEYBINDINGS
 *
 * Set the value to use for the _GNOME_WM_KEYBINDINGS property. To take
 * effect, it is necessary to call this function before meta_init().
 */
void
meta_set_gnome_wm_keybindings (const char *wm_keybindings)
{
  g_return_if_fail (meta_get_display () == NULL);

  gnome_wm_keybindings = wm_keybindings;
}

static void
meta_x11_display_class_init (MetaX11DisplayClass *klass)
{
  quark_x11_display_logical_monitor_data =
    g_quark_from_static_string ("-meta-x11-display-logical-monitor-data");
}

static void
meta_x11_display_init (MetaX11Display *disp)
{
}

static int
set_wm_check_hint (MetaX11Display *x11_display)
{
  unsigned long data[1];

  g_return_val_if_fail (x11_display->leader_window != None, 0);

  data[0] = x11_display->leader_window;

  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_SUPPORTING_WM_CHECK,
                   XA_WINDOW,
                   32, PropModeReplace, (guchar*) data, 1);

  return Success;
}

static void
unset_wm_check_hint (MetaX11Display *x11_display)
{
  XDeleteProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_SUPPORTING_WM_CHECK);
}

static int
set_supported_hint (MetaX11Display *x11_display)
{
  Atom atoms[] = {
#define EWMH_ATOMS_ONLY
#define item(x)  x11_display->atom_##x,
#include <x11/atomnames.h>
#undef item
#undef EWMH_ATOMS_ONLY

    x11_display->atom__GTK_FRAME_EXTENTS,
    x11_display->atom__GTK_SHOW_WINDOW_MENU,
  };

  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_SUPPORTED,
                   XA_ATOM,
                   32, PropModeReplace,
                   (guchar*) atoms, G_N_ELEMENTS(atoms));

  return Success;
}


static int
set_wm_icon_size_hint (MetaX11Display *x11_display)
{
#define N_VALS 6
  gulong vals[N_VALS];

  /* We've bumped the real icon size up to 96x96, but
   * we really should not add these sorts of constraints
   * on clients still using the legacy WM_HINTS interface.
   */
#define LEGACY_ICON_SIZE 32

  /* min width, min height, max w, max h, width inc, height inc */
  vals[0] = LEGACY_ICON_SIZE;
  vals[1] = LEGACY_ICON_SIZE;
  vals[2] = LEGACY_ICON_SIZE;
  vals[3] = LEGACY_ICON_SIZE;
  vals[4] = 0;
  vals[5] = 0;
#undef LEGACY_ICON_SIZE

  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom_WM_ICON_SIZE,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) vals, N_VALS);

  return Success;
#undef N_VALS
}

static void
set_desktop_geometry_hint (MetaX11Display *x11_display)
{
  unsigned long data[2];

  data[0] = x11_display->display->rect.width;
  data[1] = x11_display->display->rect.height;

  meta_verbose ("Setting _NET_DESKTOP_GEOMETRY to %lu, %lu\n", data[0], data[1]);

  meta_error_trap_push ();
  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_DESKTOP_GEOMETRY,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop ();
}

static void
set_desktop_viewport_hint (MetaX11Display *x11_display)
{
  unsigned long data[2];

  /*
   * Mutter does not implement viewports, so this is a fixed 0,0
   */
  data[0] = 0;
  data[1] = 0;

  meta_verbose ("Setting _NET_DESKTOP_VIEWPORT to 0, 0\n");

  meta_error_trap_push ();
  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_DESKTOP_VIEWPORT,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop ();
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
  Window xroot;
  Display *xdisplay;
  int i, number;
  char buf[128];
  Window new_wm_sn_owner;
  gboolean replace_current_wm;
  Atom wm_sn_atom;
  Screen *xscreen;

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

  x11_display->focus_serial = 0;
  x11_display->server_focus_window = None;
  x11_display->server_focus_serial = 0;

  meta_verbose ("Creating %d atoms\n", (int) G_N_ELEMENTS (atom_names));
  XInternAtoms (xdisplay, (char **)atom_names, G_N_ELEMENTS (atom_names),
                False, atoms);

  i = 0;
#define item(x) x11_display->atom_##x = atoms[i++];
#include <x11/atomnames.h>
#undef item

  number = meta_ui_get_screen_number ();

  meta_verbose ("Trying screen %d on display '%s'\n",
                number, x11_display->name);

  xroot = RootWindow (xdisplay, number);

  /* FVWM checks for None here, I don't know if this
   * ever actually happens
   */
  if (xroot == None)
    {
      meta_warning (_("Screen %d on display “%s” is invalid\n"),
                    number, x11_display->name);
      return FALSE;
    }

  x11_display->screen_name = get_screen_name (xdisplay, number);
  x11_display->xroot = xroot;

  meta_bell_init (x11_display);

  meta_prefs_add_listener (prefs_changed_callback, x11_display);

  x11_display->prop_hooks = NULL;
  meta_display_init_window_prop_hooks (x11_display);
  x11_display->group_prop_hooks = NULL;
  meta_display_init_group_prop_hooks (x11_display);

  x11_display->guard_window = None;

  /* Offscreen unmapped window used for _NET_SUPPORTING_WM_CHECK,
   * created in screen_new
   */
  x11_display->leader_window = None;
  x11_display->timestamp_pinging_window = None;

  x11_display->groups_by_leader = NULL;

  meta_display_init_events_x11 (x11_display);

  x11_display->xids = g_hash_table_new (meta_unsigned_long_hash,
                                        meta_unsigned_long_equal);

  x11_display->last_bell_time = 0;

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

  update_cursor_theme ();

  /* Create the leader window here. Set its properties and
   * use the timestamp from one of the PropertyNotify events
   * that will follow.
   */
  {
    gulong data[1];
    XEvent event;

    /* We only care about the PropertyChangeMask in the next 30 or so lines of
     * code.  Note that gdk will at some point unset the PropertyChangeMask for
     * this window, so we can't rely on it still being set later.  See bug
     * 354213 for details.
     */
    x11_display->leader_window =
      meta_create_offscreen_window (xdisplay,
                                    DefaultRootWindow (xdisplay),
                                    PropertyChangeMask);

    meta_prop_set_utf8_string_hint (x11_display,
                                    x11_display->leader_window,
                                    x11_display->atom__NET_WM_NAME,
                                    net_wm_name);

    meta_prop_set_utf8_string_hint (x11_display,
                                    x11_display->leader_window,
                                    x11_display->atom__GNOME_WM_KEYBINDINGS,
                                    gnome_wm_keybindings);

    meta_prop_set_utf8_string_hint (x11_display,
                                    x11_display->leader_window,
                                    x11_display->atom__MUTTER_VERSION,
                                    VERSION);

    data[0] = x11_display->leader_window;
    XChangeProperty (xdisplay,
                     x11_display->leader_window,
                     x11_display->atom__NET_SUPPORTING_WM_CHECK,
                     XA_WINDOW,
                     32, PropModeReplace, (guchar*) data, 1);

    XWindowEvent (xdisplay,
                  x11_display->leader_window,
                  PropertyChangeMask,
                  &event);

    x11_display->timestamp = event.xproperty.time;

    /* Make it painfully clear that we can't rely on PropertyNotify events on
     * this window, as per bug 354213.
     */
    XSelectInput(xdisplay,
                 x11_display->leader_window,
                 NoEventMask);
  }

  /* Make a little window used only for pinging the server for timestamps; note
   * that meta_create_offscreen_window already selects for PropertyChangeMask.
   */
  x11_display->timestamp_pinging_window =
    meta_create_offscreen_window (xdisplay,
                                  DefaultRootWindow (xdisplay),
                                  PropertyChangeMask);

  x11_display->last_focus_time = x11_display->timestamp;

  replace_current_wm = meta_get_replace_current_wm ();

  sprintf (buf, "WM_S%d", number);

  wm_sn_atom = XInternAtom (xdisplay, buf, False);
  new_wm_sn_owner = take_manager_selection (x11_display,
                                            xroot,
                                            wm_sn_atom,
                                            x11_display->timestamp,
                                            replace_current_wm);
  if (new_wm_sn_owner == None)
    return FALSE;

  {
    long event_mask;
    unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
    XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

    XISetMask (mask.mask, XI_Enter);
    XISetMask (mask.mask, XI_Leave);
    XISetMask (mask.mask, XI_FocusIn);
    XISetMask (mask.mask, XI_FocusOut);
#ifdef HAVE_XI23
    if (META_X11_DISPLAY_HAS_XINPUT_23 (x11_display))
      {
        XISetMask (mask.mask, XI_BarrierHit);
        XISetMask (mask.mask, XI_BarrierLeave);
      }
#endif /* HAVE_XI23 */
    XISelectEvents (xdisplay, xroot, &mask, 1);

    event_mask = (SubstructureRedirectMask | SubstructureNotifyMask |
                  StructureNotifyMask | ColormapChangeMask | PropertyChangeMask);
    XSelectInput (xdisplay, xroot, event_mask);
  }

  /* Select for cursor changes so the cursor tracker is up to date. */
  XFixesSelectCursorInput (xdisplay, xroot, XFixesDisplayCursorNotifyMask);

  xscreen = ScreenOfDisplay (xdisplay, number);
  x11_display->default_xvisual = DefaultVisualOfScreen (xscreen);
  x11_display->default_depth = DefaultDepthOfScreen (xscreen);

  x11_display->wm_sn_selection_window = new_wm_sn_owner;
  x11_display->wm_sn_atom = wm_sn_atom;
  x11_display->wm_sn_timestamp = x11_display->timestamp;

  /* Handle creating a no_focus_window for this screen */
  x11_display->no_focus_window =
    meta_create_offscreen_window (xdisplay,
                                  xroot,
                                  FocusChangeMask|KeyPressMask|KeyReleaseMask);
  XMapWindow (xdisplay, x11_display->no_focus_window);
  /* Done with no_focus_window stuff */

  /* If we're a Wayland compositor, then we don't grab the COW, since it
   * will map it. */
  if (!meta_is_wayland_compositor ())
    x11_display->composite_overlay_window = XCompositeGetOverlayWindow (xdisplay, xroot);

  /* Now that we've gotten taken a reference count on the COW, we
   * can close the helper that is holding on to it */
  meta_restart_finish ();

  set_wm_icon_size_hint (x11_display);

  set_supported_hint (x11_display);

  set_wm_check_hint (x11_display);

  set_desktop_viewport_hint (x11_display);

  set_desktop_geometry_hint (x11_display);

  x11_display->ui = meta_ui_new (x11_display->xdisplay);

  g_signal_connect (display, "monitors-changed",
                    G_CALLBACK (on_monitors_changed), x11_display);

  meta_verbose ("Added screen %d ('%s') root 0x%lx\n",
                number,
                x11_display->screen_name,
                x11_display->xroot);

  return TRUE;
}

void
meta_x11_display_close (MetaX11Display  *display,
                        guint32          timestamp)
{
  g_assert (display != NULL);

  display->display->x11_display = NULL;

  meta_ui_free (display->ui);

  unset_wm_check_hint (display);

  meta_prefs_remove_listener (prefs_changed_callback, display);

  meta_bell_shutdown (display);

  /* Stop caring about events */
  meta_display_free_events_x11 (display);

  /* Must be after all calls to meta_window_unmanage() since they
   * unregister windows
   */
  g_hash_table_destroy (display->xids);

  XDestroyWindow (display->xdisplay,
                  display->wm_sn_selection_window);

  if (display->leader_window != None)
    XDestroyWindow (display->xdisplay, display->leader_window);

  meta_error_trap_push ();
  XSelectInput (display->xdisplay, display->xroot, 0);
  if (meta_error_trap_pop_with_return () != Success)
    meta_warning ("Could not release screen %d on display \"%s\"\n",
                  meta_ui_get_screen_number (),
                  display->name);

  XFlush (display->xdisplay);

  meta_display_free_window_prop_hooks (display);
  meta_display_free_group_prop_hooks (display);

  g_free (display->screen_name);
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

void
meta_x11_display_increment_event_serial (MetaX11Display *display)
{
  /* We just make some random X request */
  XDeleteProperty (display->xdisplay, display->leader_window,
                   display->atom__MOTIF_WM_HINTS);
}


void
meta_x11_display_set_alarm_filter (MetaX11Display *display,
                                   MetaAlarmFilter filter,
                                   gpointer        data)
{
  g_return_if_fail (filter == NULL || display->alarm_filter == NULL);

  display->alarm_filter = filter;
  display->alarm_filter_data = data;
}

static Bool
find_timestamp_predicate (Display  *xdisplay,
                          XEvent   *ev,
                          XPointer  arg)
{
  MetaX11Display *display = (MetaX11Display *) arg;

  return (ev->type == PropertyNotify &&
          ev->xproperty.atom == display->atom__MUTTER_TIMESTAMP_PING);
}

/* Get a timestamp, even if it means a roundtrip */
guint32
meta_x11_display_get_current_time_roundtrip (MetaX11Display *display)
{
  guint32 timestamp;

  timestamp = meta_display_get_current_time (display->display);
  if (timestamp == CurrentTime)
    {
      XEvent property_event;

      XChangeProperty (display->xdisplay, display->timestamp_pinging_window,
                       display->atom__MUTTER_TIMESTAMP_PING,
                       XA_STRING, 8, PropModeAppend, NULL, 0);
      XIfEvent (display->xdisplay,
                &property_event,
                find_timestamp_predicate,
                (XPointer) display);
      timestamp = property_event.xproperty.time;
    }

  meta_display_sanity_check_timestamps (display->display, timestamp);

  return timestamp;
}

MetaWindow*
meta_x11_display_lookup_x_window (MetaX11Display *display,
                                  Window          xwindow)
{
  return g_hash_table_lookup (display->xids, &xwindow);
}

void
meta_x11_display_register_x_window (MetaX11Display *display,
                                    Window         *xwindowp,
                                    MetaWindow     *window)
{
  g_return_if_fail (g_hash_table_lookup (display->xids, xwindowp) == NULL);

  g_hash_table_insert (display->xids, xwindowp, window);
}

void
meta_x11_display_unregister_x_window (MetaX11Display *display,
                                      Window          xwindow)
{
  g_return_if_fail (g_hash_table_lookup (display->xids, &xwindow) != NULL);

  g_hash_table_remove (display->xids, &xwindow);
}

/* We store sync alarms in the window ID hash table, because they are
 * just more types of XIDs in the same global space, but we have
 * typesafe functions to register/unregister for readability.
 */

MetaWindow*
meta_x11_display_lookup_sync_alarm (MetaX11Display *display,
                                    XSyncAlarm      alarm)
{
  return g_hash_table_lookup (display->xids, &alarm);
}

void
meta_x11_display_register_sync_alarm (MetaX11Display *display,
                                      XSyncAlarm     *alarmp,
                                      MetaWindow     *window)
{
  g_return_if_fail (g_hash_table_lookup (display->xids, alarmp) == NULL);

  g_hash_table_insert (display->xids, alarmp, window);
}

void
meta_x11_display_unregister_sync_alarm (MetaX11Display *display,
                                        XSyncAlarm      alarm)
{
  g_return_if_fail (g_hash_table_lookup (display->xids, &alarm) != NULL);

  g_hash_table_remove (display->xids, &alarm);
}

static void
prefs_changed_callback (MetaPreference pref,
                        void          *data)
{
  MetaX11Display *display = data;

  if (pref == META_PREF_AUDIBLE_BELL)
    {
      meta_bell_set_audible (display, meta_prefs_bell_is_audible ());
    }
  else if (pref == META_PREF_CURSOR_THEME ||
           pref == META_PREF_CURSOR_SIZE)
    {
      update_cursor_theme ();
    }
}

static void
set_cursor_theme (Display *xdisplay)
{
  XcursorSetTheme (xdisplay, meta_prefs_get_cursor_theme ());
  XcursorSetDefaultSize (xdisplay, meta_prefs_get_cursor_size ());
}

static void
update_cursor_theme (void)
{
  {
    MetaDisplay *display = meta_get_display ();
    MetaX11Display *x11_display = display->x11_display;

    set_cursor_theme (x11_display->xdisplay);

    meta_display_update_cursor (display);
  }

  {
    MetaBackend *backend = meta_get_backend ();
    if (META_IS_BACKEND_X11 (backend))
      {
        Display *xdisplay = meta_backend_x11_get_xdisplay (META_BACKEND_X11 (backend));
        set_cursor_theme (xdisplay);
      }
  }
}

gboolean
meta_x11_display_timestamp_too_old (MetaX11Display *display,
                                    guint32        *timestamp)
{
  /* FIXME: If Soeren's suggestion in bug 151984 is implemented, it will allow
   * us to sanity check the timestamp here and ensure it doesn't correspond to
   * a future time (though we would want to rename to
   * timestamp_too_old_or_in_future).
   */

  if (*timestamp == CurrentTime)
    {
      *timestamp = meta_x11_display_get_current_time_roundtrip (display);
      return FALSE;
    }
  else if (XSERVER_TIME_IS_BEFORE (*timestamp, display->last_focus_time))
    {
      if (XSERVER_TIME_IS_BEFORE (*timestamp, display->display->last_user_time))
        return TRUE;
      else
        {
          *timestamp = display->last_focus_time;
          return FALSE;
        }
    }

  return FALSE;
}

static void
request_xserver_input_focus_change (MetaX11Display *display,
                                    MetaScreen     *screen,
                                    MetaWindow     *meta_window,
                                    Window          xwindow,
                                    guint32         timestamp)
{
  gulong serial;

  if (meta_x11_display_timestamp_too_old (display, &timestamp))
    return;

  meta_error_trap_push ();

  /* In order for mutter to know that the focus request succeeded, we track
   * the serial of the "focus request" we made, but if we take the serial
   * of the XSetInputFocus request, then there's no way to determine the
   * difference between focus events as a result of the SetInputFocus and
   * focus events that other clients send around the same time. Ensure that
   * we know which is which by making two requests that the server will
   * process at the same time.
   */
  XGrabServer (display->xdisplay);

  serial = XNextRequest (display->xdisplay);

  XSetInputFocus (display->xdisplay,
                  xwindow,
                  RevertToPointerRoot,
                  timestamp);

  XChangeProperty (display->xdisplay, display->timestamp_pinging_window,
                   display->atom__MUTTER_FOCUS_SET,
                   XA_STRING, 8, PropModeAppend, NULL, 0);

  XUngrabServer (display->xdisplay);
  XFlush (display->xdisplay);

  meta_display_update_focus_window (display->display,
                                    meta_window,
                                    xwindow,
                                    serial,
                                    TRUE);

  meta_error_trap_pop ();

  display->last_focus_time = timestamp;

  if (meta_window == NULL || meta_window != display->display->autoraise_window)
    meta_display_remove_autoraise_callback (display->display);
}

/* TODO: Split into non-X11 functions */
void
meta_display_set_input_focus_window (MetaDisplay *display,
                                     MetaWindow  *window,
                                     gboolean     focus_frame,
                                     guint32      timestamp)
{
  request_xserver_input_focus_change (display->x11_display,
                                      window->screen,
                                      window,
                                      focus_frame ? window->frame->xwindow : window->xwindow,
                                      timestamp);
}

void
meta_display_set_input_focus_xwindow (MetaDisplay *display,
                                      MetaScreen  *screen,
                                      Window       window,
                                      guint32      timestamp)
{
  request_xserver_input_focus_change (display->x11_display,
                                      screen,
                                      NULL,
                                      window,
                                      timestamp);
}

void
meta_display_focus_the_no_focus_window (MetaDisplay *display,
                                        MetaScreen  *screen,
                                        guint32      timestamp)
{
  request_xserver_input_focus_change (display->x11_display,
                                      screen,
                                      NULL,
                                      display->x11_display->no_focus_window,
                                      timestamp);
}

static char*
get_screen_name (Display *xdisplay,
                 int      number)
{
  char *p;
  char *dname;
  char *scr;

  /* DisplayString gives us a sort of canonical display,
   * vs. the user-entered name from XDisplayName()
   */
  dname = g_strdup (DisplayString (xdisplay));

  /* Change display name to specify this screen.
   */
  p = strrchr (dname, ':');
  if (p)
    {
      p = strchr (p, '.');
      if (p)
        *p = '\0';
    }

  scr = g_strdup_printf ("%s.%d", dname, number);

  g_free (dname);

  return scr;
}

Window
meta_create_offscreen_window (Display *xdisplay,
                              Window   parent,
                              long     valuemask)
{
  XSetWindowAttributes attrs;

  /* we want to be override redirect because sometimes we
   * create a window on a screen we aren't managing.
   * (but on a display we are managing at least one screen for)
   */
  attrs.override_redirect = True;
  attrs.event_mask = valuemask;

  return XCreateWindow (xdisplay,
                        parent,
                        -100, -100, 1, 1,
                        0,
                        CopyFromParent,
                        CopyFromParent,
                        (Visual *)CopyFromParent,
                        CWOverrideRedirect | CWEventMask,
                        &attrs);
}

/**
 * meta_x11_display_get_xroot: (skip)
 * @screen: A #MetaX11Display
 *
 */
Window
meta_x11_display_get_xroot (MetaX11Display *x11_display)
{
  return x11_display->xroot;
}

void
meta_x11_display_set_cm_selection (MetaX11Display *x11_display)
{
  char selection[32];
  Atom a;
  guint32 timestamp;

  timestamp = meta_x11_display_get_current_time_roundtrip (x11_display);
  g_snprintf (selection, sizeof (selection), "_NET_WM_CM_S%d",
              meta_ui_get_screen_number ());
  a = XInternAtom (x11_display->xdisplay, selection, False);
  x11_display->wm_cm_selection_window = take_manager_selection (x11_display,
                                                                x11_display->xroot,
							        a, timestamp, TRUE);
}

/**
 * meta_x11_display_xwindow_is_a_no_focus_window:
 * @x11_display: A #MetaX11Display
 * @xwindow: An X11 window
 *
 * Returns: %TRUE iff window is one of mutter's internal "no focus" windows
 * (there is one per screen) which will have the focus when there is no
 * actual client window focused.
 */
gboolean
meta_x11_display_xwindow_is_a_no_focus_window (MetaX11Display *x11_display,
                                               Window xwindow)
{
  return x11_display ? xwindow == x11_display->no_focus_window : FALSE;
}

void
meta_x11_display_create_guard_window (MetaX11Display *x11_display)
{
  if (x11_display->guard_window == None)
    x11_display->guard_window = create_guard_window (x11_display);
}

void
meta_x11_display_update_cursor (MetaX11Display *x11_display)
{
  MetaCursor cursor = x11_display->display->current_cursor;
  Cursor xcursor;

  /* Set a cursor for X11 applications that don't specify their own */
  xcursor = meta_cursor_create_x_cursor (x11_display->xdisplay, cursor);

  XDefineCursor (x11_display->xdisplay, x11_display->xroot, xcursor);
  XFlush (x11_display->xdisplay);
  XFreeCursor (x11_display->xdisplay, xcursor);
}

static void
on_monitors_changed (MetaDisplay    *display,
                     MetaX11Display *x11_display)
{
  set_desktop_geometry_hint (x11_display);

  /* Resize the guard window to fill the screen again. */
  if (x11_display->guard_window != None)
    {
      XWindowChanges changes;

      changes.x = 0;
      changes.y = 0;
      changes.width = display->rect.width;
      changes.height = display->rect.height;

      XConfigureWindow(x11_display->xdisplay,
                       x11_display->guard_window,
                       CWX | CWY | CWWidth | CWHeight,
                       &changes);
    }

  x11_display->has_xinerama_indices = FALSE;
}

static MetaX11DisplayLogicalMonitorData *
get_x11_display_logical_monitor_data (MetaLogicalMonitor *logical_monitor)
{
  return g_object_get_qdata (G_OBJECT (logical_monitor),
                             quark_x11_display_logical_monitor_data);
}

static MetaX11DisplayLogicalMonitorData *
ensure_x11_display_logical_monitor_data (MetaLogicalMonitor *logical_monitor)
{
  MetaX11DisplayLogicalMonitorData *data;

  data = get_x11_display_logical_monitor_data (logical_monitor);
  if (data)
    return data;

  data = g_new0 (MetaX11DisplayLogicalMonitorData, 1);
  g_object_set_qdata_full (G_OBJECT (logical_monitor),
                           quark_x11_display_logical_monitor_data,
                           data,
                           g_free);

  return data;
}

static void
meta_x11_display_ensure_xinerama_indices (MetaX11Display *x11_display)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  GList *logical_monitors, *l;
  XineramaScreenInfo *infos;
  int n_infos, j;

  if (x11_display->has_xinerama_indices)
    return;

  x11_display->has_xinerama_indices = TRUE;

  if (!XineramaIsActive (x11_display->xdisplay))
    return;

  infos = XineramaQueryScreens (x11_display->xdisplay, &n_infos);
  if (n_infos <= 0 || infos == NULL)
    {
      meta_XFree (infos);
      return;
    }

  logical_monitors =
    meta_monitor_manager_get_logical_monitors (monitor_manager);

  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;

      for (j = 0; j < n_infos; ++j)
        {
          if (logical_monitor->rect.x == infos[j].x_org &&
              logical_monitor->rect.y == infos[j].y_org &&
              logical_monitor->rect.width == infos[j].width &&
              logical_monitor->rect.height == infos[j].height)
            {
              MetaX11DisplayLogicalMonitorData *logical_monitor_data;

              logical_monitor_data =
                ensure_x11_display_logical_monitor_data (logical_monitor);
              logical_monitor_data->xinerama_index = j;
            }
        }
    }

  meta_XFree (infos);
}

int
meta_x11_display_logical_monitor_to_xinerama_index (MetaX11Display     *x11_display,
                                                    MetaLogicalMonitor *logical_monitor)
{
  MetaX11DisplayLogicalMonitorData *logical_monitor_data;

  g_return_val_if_fail (logical_monitor, -1);

  meta_x11_display_ensure_xinerama_indices (x11_display);

  logical_monitor_data = get_x11_display_logical_monitor_data (logical_monitor);

  return logical_monitor_data->xinerama_index;
}

MetaLogicalMonitor *
meta_x11_display_xinerama_index_to_logical_monitor (MetaX11Display *x11_display,
                                                    int             xinerama_index)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  GList *logical_monitors, *l;

  meta_x11_display_ensure_xinerama_indices (x11_display);

  logical_monitors =
    meta_monitor_manager_get_logical_monitors (monitor_manager);

  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;
      MetaX11DisplayLogicalMonitorData *logical_monitor_data;

      logical_monitor_data =
        ensure_x11_display_logical_monitor_data (logical_monitor);

      if (logical_monitor_data->xinerama_index == xinerama_index)
        return logical_monitor;
    }

  return NULL;
}

void
meta_x11_display_update_showing_desktop_hint (MetaX11Display *x11_display)
{
  MetaDisplay *display = x11_display->display;
  unsigned long data[1];

  data[0] = display->active_workspace->showing_desktop ? 1 : 0;

  meta_error_trap_push ();
  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_SHOWING_DESKTOP,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 1);
  meta_error_trap_pop ();
}

void
meta_x11_display_update_workspace_names (MetaX11Display *x11_display)
{
  char **names;
  int n_names;
  int i;

  /* this updates names in prefs when the root window property changes,
   * iff the new property contents don't match what's already in prefs
   */

  names = NULL;
  n_names = 0;
  if (!meta_prop_get_utf8_list (x11_display,
                                x11_display->xroot,
                                x11_display->atom__NET_DESKTOP_NAMES,
                                &names, &n_names))
    {
      meta_verbose ("Failed to get workspace names from root window\n");
      return;
    }

  i = 0;
  while (i < n_names)
    {
      meta_topic (META_DEBUG_PREFS,
                  "Setting workspace %d name to \"%s\" due to _NET_DESKTOP_NAMES change\n",
                  i, names[i] ? names[i] : "null");
      meta_prefs_change_workspace_name (i, names[i]);

      ++i;
    }

  g_strfreev (names);
}

void
meta_x11_display_set_active_workspace_hint (MetaX11Display *x11_display)
{
  unsigned long data[1];

  data[0] = meta_workspace_index (x11_display->display->active_workspace);

  meta_verbose ("Setting _NET_CURRENT_DESKTOP to %lu\n", data[0]);

  meta_error_trap_push ();
  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_CURRENT_DESKTOP,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 1);
  meta_error_trap_pop ();
}

void
meta_x11_display_set_number_of_spaces_hint (MetaX11Display *x11_display,
					    int             n_spaces)
{
  unsigned long data[1];

  data[0] = n_spaces;

  meta_verbose ("Setting _NET_NUMBER_OF_DESKTOPS to %lu\n", data[0]);

  meta_error_trap_push ();
  XChangeProperty (x11_display->xdisplay,
                   x11_display->xroot,
                   x11_display->atom__NET_NUMBER_OF_DESKTOPS,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 1);
  meta_error_trap_pop ();
}

gboolean
meta_x11_display_handle_xevent (MetaX11Display *x11_display,
                                XEvent         *xevent)
{
  MetaBackend *backend = meta_get_backend ();
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);

  if (meta_cursor_tracker_handle_xevent (cursor_tracker, xevent))
    return TRUE;

  return FALSE;
}
