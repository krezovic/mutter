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
#include "util-private.h"

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

static const char *gnome_wm_keybindings = "Mutter";
static const char *net_wm_name = "Mutter";

static void update_cursor_theme (void);

static void prefs_changed_callback (MetaPreference pref,
				    void          *data);

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

  meta_bell_init (x11_display);

  meta_prefs_add_listener (prefs_changed_callback, x11_display);

  x11_display->prop_hooks = NULL;
  meta_display_init_window_prop_hooks (x11_display);
  x11_display->group_prop_hooks = NULL;
  meta_display_init_group_prop_hooks (x11_display);

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

  return TRUE;
}

void
meta_x11_display_close (MetaX11Display  *display,
                        guint32          timestamp)
{
  g_assert (display != NULL);

  display->display->x11_display = NULL;

  meta_prefs_remove_listener (prefs_changed_callback, display);

  meta_bell_shutdown (display);

  /* Stop caring about events */
  meta_display_free_events_x11 (display);

  /* Must be after all calls to meta_window_unmanage() since they
   * unregister windows
   */
  g_hash_table_destroy (display->xids);

  if (display->leader_window != None)
    XDestroyWindow (display->xdisplay, display->leader_window);

  XFlush (display->xdisplay);

  meta_display_free_window_prop_hooks (display);
  meta_display_free_group_prop_hooks (display);

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

    if (display->screen)
      meta_screen_update_cursor (display->screen);
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
                                      screen->no_focus_window,
                                      timestamp);
}
