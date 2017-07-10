/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001, 2002 Havoc Pennington
 * Copyright (C) 2002, 2003 Red Hat Inc.
 * Some ICCCM manager selection code derived from fvwm2,
 * Copyright (C) 2001 Dominik Vogt, Matthias Clasen, and fvwm2 team
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

/**
 * SECTION:screen
 * @title: MetaScreen
 * @short_description: Mutter X screen handler
 */

#include <config.h>
#include "screen-private.h"
#include <meta/main.h>
#include "util-private.h"
#include <meta/errors.h>
#include "window-private.h"
#include "frame.h"
#include <meta/prefs.h>
#include "workspace-private.h"
#include "keybindings-private.h"
#include "stack.h"
#include <meta/compositor.h>
#include <meta/meta-enum-types.h>
#include "core.h"
#include "meta-cursor-tracker-private.h"
#include "boxes-private.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-logical-monitor.h"

#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xcomposite.h>

#include <X11/Xatom.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "x11/display-x11-private.h"
#include "x11/window-x11.h"
#include "x11/xprops.h"

#include "backends/x11/meta-backend-x11.h"

#define XDISPLAY(x) (x->display->x11_display->xdisplay)
#define XATOM(x, y) (x->display->x11_display->y)
#define XROOT(x) (x->display->x11_display->xroot)

static void set_desktop_geometry_hint (MetaScreen *screen);
static void set_desktop_viewport_hint (MetaScreen *screen);

static void on_monitors_changed (MetaDisplay *display,
                                 MetaScreen  *screen);

enum
{
  WINDOW_ENTERED_MONITOR,
  WINDOW_LEFT_MONITOR,
  STARTUP_SEQUENCE_CHANGED,
  WORKAREAS_CHANGED,

  LAST_SIGNAL
};

static guint screen_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (MetaScreen, meta_screen, G_TYPE_OBJECT);

static void
meta_screen_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
#if 0
  MetaScreen *screen = META_SCREEN (object);
#endif

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_screen_get_property (GObject      *object,
                          guint         prop_id,
                          GValue       *value,
                          GParamSpec   *pspec)
{
#if 0
  MetaScreen *screen = META_SCREEN (object);
#endif

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_screen_finalize (GObject *object)
{
  /* Actual freeing done in meta_screen_free() for now */
}

static void
meta_screen_class_init (MetaScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = meta_screen_get_property;
  object_class->set_property = meta_screen_set_property;
  object_class->finalize = meta_screen_finalize;

  screen_signals[WINDOW_ENTERED_MONITOR] =
    g_signal_new ("window-entered-monitor",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT,
                  META_TYPE_WINDOW);

  screen_signals[WINDOW_LEFT_MONITOR] =
    g_signal_new ("window-left-monitor",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_INT,
                  META_TYPE_WINDOW);

  screen_signals[STARTUP_SEQUENCE_CHANGED] =
    g_signal_new ("startup-sequence-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_POINTER);

  screen_signals[WORKAREAS_CHANGED] =
    g_signal_new ("workareas-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (MetaScreenClass, workareas_changed),
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
meta_screen_init (MetaScreen *screen)
{
}

static int
set_wm_check_hint (MetaScreen *screen)
{
  unsigned long data[1];

  g_return_val_if_fail (screen->display->x11_display->leader_window != None, 0);

  data[0] = screen->display->x11_display->leader_window;

  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom__NET_SUPPORTING_WM_CHECK),
                   XA_WINDOW,
                   32, PropModeReplace, (guchar*) data, 1);

  return Success;
}

static void
unset_wm_check_hint (MetaScreen *screen)
{
  XDeleteProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom__NET_SUPPORTING_WM_CHECK));
}

static int
set_supported_hint (MetaScreen *screen)
{
  Atom atoms[] = {
#define EWMH_ATOMS_ONLY
#define item(x)  XATOM(screen, atom_##x),
#include <x11/atomnames.h>
#undef item
#undef EWMH_ATOMS_ONLY

    XATOM(screen, atom__GTK_FRAME_EXTENTS),
    XATOM(screen, atom__GTK_SHOW_WINDOW_MENU),
  };

  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom__NET_SUPPORTED),
                   XA_ATOM,
                   32, PropModeReplace,
                   (guchar*) atoms, G_N_ELEMENTS(atoms));

  return Success;
}

static int
set_wm_icon_size_hint (MetaScreen *screen)
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

  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom_WM_ICON_SIZE),
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) vals, N_VALS);

  return Success;
#undef N_VALS
}

MetaScreen*
meta_screen_new (MetaDisplay *display,
                 guint32      timestamp)
{
  MetaScreen *screen;
  int number;
  Screen *xscreen;
  Display *xdisplay;

  number = meta_ui_get_screen_number ();

  /* Only display->name, display->xdisplay, and display->error_traps
   * can really be used in this function, since normally screens are
   * created from the MetaDisplay constructor
   */

  xdisplay = display->x11_display->xdisplay;

  meta_verbose ("Trying screen %d on display '%s'\n",
                number, display->x11_display->name);

  screen = g_object_new (META_TYPE_SCREEN, NULL);
  screen->closing = 0;

  screen->display = display;

  g_signal_connect (display, "monitors-changed",
                    G_CALLBACK (on_monitors_changed), screen);

  xscreen = ScreenOfDisplay (xdisplay, number);
  screen->default_xvisual = DefaultVisualOfScreen (xscreen);
  screen->default_depth = DefaultDepthOfScreen (xscreen);

  screen->work_area_later = 0;

  /* Now that we've gotten taken a reference count on the COW, we
   * can close the helper that is holding on to it */
  meta_restart_finish ();

  set_wm_icon_size_hint (screen);

  set_supported_hint (screen);

  set_wm_check_hint (screen);

  set_desktop_viewport_hint (screen);

  set_desktop_geometry_hint (screen);

  screen->keys_grabbed = FALSE;
  meta_screen_grab_keys (screen);

  screen->ui = meta_ui_new (XDISPLAY(screen));

  screen->tile_preview_timeout_id = 0;

  meta_verbose ("Added screen %d ('%s') root 0x%lx\n",
                number,
                screen->display->x11_display->screen_name,
                XROOT(screen));

  return screen;
}

void
meta_screen_free (MetaScreen *screen,
                  guint32     timestamp)
{
  MetaDisplay *display;

  display = screen->display;

  screen->closing += 1;

  meta_compositor_unmanage (screen->display->compositor);

  meta_display_unmanage_windows_for_screen (display, screen, timestamp);

  meta_screen_ungrab_keys (screen);

  meta_ui_free (screen->ui);

  unset_wm_check_hint (screen);

  if (screen->work_area_later != 0)
    meta_later_remove (screen->work_area_later);

  if (screen->tile_preview_timeout_id)
    g_source_remove (screen->tile_preview_timeout_id);

  g_object_unref (screen);
}

static void
set_desktop_geometry_hint (MetaScreen *screen)
{
  unsigned long data[2];

  if (screen->closing > 0)
    return;

  data[0] = screen->display->rect.width;
  data[1] = screen->display->rect.height;

  meta_verbose ("Setting _NET_DESKTOP_GEOMETRY to %lu, %lu\n", data[0], data[1]);

  meta_error_trap_push ();
  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom__NET_DESKTOP_GEOMETRY),
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop ();
}

static void
set_desktop_viewport_hint (MetaScreen *screen)
{
  unsigned long data[2];

  if (screen->closing > 0)
    return;

  /*
   * Mutter does not implement viewports, so this is a fixed 0,0
   */
  data[0] = 0;
  data[1] = 0;

  meta_verbose ("Setting _NET_DESKTOP_VIEWPORT to 0, 0\n");

  meta_error_trap_push ();
  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
                   XATOM(screen, atom__NET_DESKTOP_VIEWPORT),
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop ();
}

static gboolean
meta_screen_update_tile_preview_timeout (gpointer data)
{
  MetaScreen *screen = data;
  MetaWindow *window = screen->display->grab_window;
  gboolean needs_preview = FALSE;

  screen->tile_preview_timeout_id = 0;

  if (window)
    {
      switch (window->tile_mode)
        {
          case META_TILE_LEFT:
          case META_TILE_RIGHT:
              if (!META_WINDOW_TILED_SIDE_BY_SIDE (window))
                needs_preview = TRUE;
              break;

          case META_TILE_MAXIMIZED:
              if (!META_WINDOW_MAXIMIZED (window))
                needs_preview = TRUE;
              break;

          default:
              needs_preview = FALSE;
              break;
        }
    }

  if (needs_preview)
    {
      MetaRectangle tile_rect;
      int monitor;

      monitor = meta_window_get_current_tile_monitor_number (window);
      meta_window_get_current_tile_area (window, &tile_rect);
      meta_compositor_show_tile_preview (screen->display->compositor,
                                         window, &tile_rect, monitor);
    }
  else
    meta_compositor_hide_tile_preview (screen->display->compositor);

  return FALSE;
}

#define TILE_PREVIEW_TIMEOUT_MS 200

void
meta_screen_update_tile_preview (MetaScreen *screen,
                                 gboolean    delay)
{
  if (delay)
    {
      if (screen->tile_preview_timeout_id > 0)
        return;

      screen->tile_preview_timeout_id =
        g_timeout_add (TILE_PREVIEW_TIMEOUT_MS,
                       meta_screen_update_tile_preview_timeout,
                       screen);
      g_source_set_name_by_id (screen->tile_preview_timeout_id,
                               "[mutter] meta_screen_update_tile_preview_timeout");
    }
  else
    {
      if (screen->tile_preview_timeout_id > 0)
        g_source_remove (screen->tile_preview_timeout_id);

      meta_screen_update_tile_preview_timeout ((gpointer)screen);
    }
}

void
meta_screen_hide_tile_preview (MetaScreen *screen)
{
  if (screen->tile_preview_timeout_id > 0)
    g_source_remove (screen->tile_preview_timeout_id);

  meta_compositor_hide_tile_preview (screen->display->compositor);
}

MetaWindow*
meta_screen_get_mouse_window (MetaScreen  *screen,
                              MetaWindow  *not_this_one)
{
  MetaBackend *backend = meta_get_backend ();
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);
  MetaWindow *window;
  int x, y;

  if (not_this_one)
    meta_topic (META_DEBUG_FOCUS,
                "Focusing mouse window excluding %s\n", not_this_one->desc);

  meta_cursor_tracker_get_pointer (cursor_tracker, &x, &y, NULL);

  window = meta_stack_get_default_focus_window_at_point (screen->display->stack,
                                                         screen->display->active_workspace,
                                                         not_this_one,
                                                         x, y);

  return window;
}

int
meta_screen_get_monitor_index_for_rect (MetaScreen    *screen,
                                        MetaRectangle *rect)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;

  logical_monitor =
    meta_monitor_manager_get_logical_monitor_from_rect (monitor_manager, rect);
  return logical_monitor->number;
}

int
meta_screen_get_monitor_neighbor_index (MetaScreen         *screen,
                                        int                 which_monitor,
                                        MetaScreenDirection direction)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;
  MetaLogicalMonitor *neighbor;

  logical_monitor =
    meta_monitor_manager_get_logical_monitor_from_number (monitor_manager,
                                                          which_monitor);
  neighbor = meta_monitor_manager_get_logical_monitor_neighbor (monitor_manager,
                                                                logical_monitor,
                                                                direction);
  return neighbor ? neighbor->number : -1;
}

/**
 * meta_screen_get_current_monitor:
 * @screen: a #MetaScreen
 *
 * Gets the index of the monitor that currently has the mouse pointer.
 *
 * Return value: a monitor index
 */
int
meta_screen_get_current_monitor (MetaScreen *screen)
{
  MetaBackend *backend = meta_get_backend ();
  MetaLogicalMonitor *logical_monitor;

  logical_monitor = meta_backend_get_current_logical_monitor (backend);

  /* Pretend its the first when there is no actual current monitor. */
  if (!logical_monitor)
    return 0;

  return logical_monitor->number;
}

/**
 * meta_screen_get_n_monitors:
 * @screen: a #MetaScreen
 *
 * Gets the number of monitors that are joined together to form @screen.
 *
 * Return value: the number of monitors
 */
int
meta_screen_get_n_monitors (MetaScreen *screen)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);

  g_return_val_if_fail (META_IS_SCREEN (screen), 0);

  return meta_monitor_manager_get_num_logical_monitors (monitor_manager);
}

/**
 * meta_screen_get_primary_monitor:
 * @screen: a #MetaScreen
 *
 * Gets the index of the primary monitor on this @screen.
 *
 * Return value: a monitor index
 */
int
meta_screen_get_primary_monitor (MetaScreen *screen)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;

  g_return_val_if_fail (META_IS_SCREEN (screen), 0);

  logical_monitor =
    meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (logical_monitor)
    return logical_monitor->number;
  else
    return 0;
}

/**
 * meta_screen_get_monitor_geometry:
 * @screen: a #MetaScreen
 * @monitor: the monitor number
 * @geometry: (out): location to store the monitor geometry
 *
 * Stores the location and size of the indicated monitor in @geometry.
 */
void
meta_screen_get_monitor_geometry (MetaScreen    *screen,
                                  int            monitor,
                                  MetaRectangle *geometry)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;
#ifndef G_DISABLE_CHECKS
  int n_logical_monitors =
    meta_monitor_manager_get_num_logical_monitors (monitor_manager);
#endif

  g_return_if_fail (META_IS_SCREEN (screen));
  g_return_if_fail (monitor >= 0 && monitor < n_logical_monitors);
  g_return_if_fail (geometry != NULL);

  logical_monitor =
    meta_monitor_manager_get_logical_monitor_from_number (monitor_manager,
                                                          monitor);
  *geometry = logical_monitor->rect;
}

static void
set_work_area_hint (MetaScreen *screen)
{
  int num_workspaces;
  GList *l;
  unsigned long *data, *tmp;
  MetaRectangle area;

  num_workspaces = meta_display_get_n_workspaces (screen->display);
  data = g_new (unsigned long, num_workspaces * 4);
  tmp = data;

  for (l = screen->display->workspaces; l != NULL; l = l->next)
    {
      MetaWorkspace *workspace = l->data;

      meta_workspace_get_work_area_all_monitors (workspace, &area);
      tmp[0] = area.x;
      tmp[1] = area.y;
      tmp[2] = area.width;
      tmp[3] = area.height;

      tmp += 4;
    }

  meta_error_trap_push ();
  XChangeProperty (XDISPLAY(screen),
                   XROOT(screen),
		   XATOM(screen, atom__NET_WORKAREA),
		   XA_CARDINAL, 32, PropModeReplace,
		   (guchar*) data, num_workspaces*4);
  g_free (data);
  meta_error_trap_pop ();

  g_signal_emit (screen, screen_signals[WORKAREAS_CHANGED], 0);
}

static gboolean
set_work_area_later_func (MetaScreen *screen)
{
  meta_topic (META_DEBUG_WORKAREA,
              "Running work area hint computation function\n");

  screen->work_area_later = 0;

  set_work_area_hint (screen);

  return FALSE;
}

void
meta_screen_queue_workarea_recalc (MetaScreen *screen)
{
  /* Recompute work area later before redrawing */
  if (screen->work_area_later == 0)
    {
      meta_topic (META_DEBUG_WORKAREA,
                  "Adding work area hint computation function\n");
      screen->work_area_later =
        meta_later_add (META_LATER_BEFORE_REDRAW,
                        (GSourceFunc) set_work_area_later_func,
                        screen,
                        NULL);
    }
}

static void
on_monitors_changed (MetaDisplay *display,
                     MetaScreen  *screen)
{
  set_desktop_geometry_hint (screen);
}

/**
 * meta_screen_get_startup_sequences: (skip)
 * @screen:
 *
 * Return value: (transfer none): Currently active #SnStartupSequence items
 */
GSList *
meta_screen_get_startup_sequences (MetaScreen *screen)
{
  return screen->startup_sequences;
}

/* Sets the initial_timestamp and initial_workspace properties
 * of a window according to information given us by the
 * startup-notification library.
 *
 * Returns TRUE if startup properties have been applied, and
 * FALSE if they have not (for example, if they had already
 * been applied.)
 */
gboolean
meta_screen_apply_startup_properties (MetaScreen *screen,
                                      MetaWindow *window)
{
#ifdef HAVE_STARTUP_NOTIFICATION
  const char *startup_id;
  GSList *l;
  SnStartupSequence *sequence;

  /* Does the window have a startup ID stored? */
  startup_id = meta_window_get_startup_id (window);

  meta_topic (META_DEBUG_STARTUP,
              "Applying startup props to %s id \"%s\"\n",
              window->desc,
              startup_id ? startup_id : "(none)");

  sequence = NULL;
  if (startup_id == NULL)
    {
      /* No startup ID stored for the window. Let's ask the
       * startup-notification library whether there's anything
       * stored for the resource name or resource class hints.
       */
      for (l = screen->startup_sequences; l != NULL; l = l->next)
        {
          const char *wmclass;
          SnStartupSequence *seq = l->data;

          wmclass = sn_startup_sequence_get_wmclass (seq);

          if (wmclass != NULL &&
              ((window->res_class &&
                strcmp (wmclass, window->res_class) == 0) ||
               (window->res_name &&
                strcmp (wmclass, window->res_name) == 0)))
            {
              sequence = seq;

              g_assert (window->startup_id == NULL);
              window->startup_id = g_strdup (sn_startup_sequence_get_id (sequence));
              startup_id = window->startup_id;

              meta_topic (META_DEBUG_STARTUP,
                          "Ending legacy sequence %s due to window %s\n",
                          sn_startup_sequence_get_id (sequence),
                          window->desc);

              sn_startup_sequence_complete (sequence);
              break;
            }
        }
    }

  /* Still no startup ID? Bail. */
  if (startup_id == NULL)
    return FALSE;

  /* We might get this far and not know the sequence ID (if the window
   * already had a startup ID stored), so let's look for one if we don't
   * already know it.
   */
  if (sequence == NULL)
    {
      for (l = screen->startup_sequences; l != NULL; l = l->next)
        {
          SnStartupSequence *seq = l->data;
          const char *id;

          id = sn_startup_sequence_get_id (seq);

          if (strcmp (id, startup_id) == 0)
            {
              sequence = seq;
              break;
            }
        }
    }

  if (sequence != NULL)
    {
      gboolean changed_something = FALSE;

      meta_topic (META_DEBUG_STARTUP,
                  "Found startup sequence for window %s ID \"%s\"\n",
                  window->desc, startup_id);

      if (!window->initial_workspace_set)
        {
          int space = sn_startup_sequence_get_workspace (sequence);
          if (space >= 0)
            {
              meta_topic (META_DEBUG_STARTUP,
                          "Setting initial window workspace to %d based on startup info\n",
                          space);

              window->initial_workspace_set = TRUE;
              window->initial_workspace = space;
              changed_something = TRUE;
            }
        }

      if (!window->initial_timestamp_set)
        {
          guint32 timestamp = sn_startup_sequence_get_timestamp (sequence);
          meta_topic (META_DEBUG_STARTUP,
                      "Setting initial window timestamp to %u based on startup info\n",
                      timestamp);

          window->initial_timestamp_set = TRUE;
          window->initial_timestamp = timestamp;
          changed_something = TRUE;
        }

      return changed_something;
    }
  else
    {
      meta_topic (META_DEBUG_STARTUP,
                  "Did not find startup sequence for window %s ID \"%s\"\n",
                  window->desc, startup_id);
    }

#endif /* HAVE_STARTUP_NOTIFICATION */

  return FALSE;
}

int
meta_screen_get_screen_number (MetaScreen *screen)
{
  return meta_ui_get_screen_number ();
}

/**
 * meta_screen_get_display:
 * @screen: A #MetaScreen
 *
 * Retrieve the display associated with screen.
 *
 * Returns: (transfer none): Display
 */
MetaDisplay *
meta_screen_get_display (MetaScreen *screen)
{
  return screen->display;
}

/**
 * meta_screen_get_monitor_in_fullscreen:
 * @screen: a #MetaScreen
 * @monitor: the monitor number
 *
 * Determines whether there is a fullscreen window obscuring the specified
 * monitor. If there is a fullscreen window, the desktop environment will
 * typically hide any controls that might obscure the fullscreen window.
 *
 * You can get notification when this changes by connecting to
 * MetaScreen::in-fullscreen-changed.
 *
 * Returns: %TRUE if there is a fullscreen window covering the specified monitor.
 */
gboolean
meta_screen_get_monitor_in_fullscreen (MetaScreen  *screen,
                                       int          monitor)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);
  MetaLogicalMonitor *logical_monitor;
#ifndef G_DISABLE_CHECKS
  int n_logical_monitors =
    meta_monitor_manager_get_num_logical_monitors (monitor_manager);
#endif

  g_return_val_if_fail (META_IS_SCREEN (screen), FALSE);
  g_return_val_if_fail (monitor >= 0 &&
                        monitor < n_logical_monitors, FALSE);

  logical_monitor =
    meta_monitor_manager_get_logical_monitor_from_number (monitor_manager,
                                                          monitor);

  /* We use -1 as a flag to mean "not known yet" for notification purposes */
  return logical_monitor->in_fullscreen == TRUE;
}

gboolean
meta_screen_handle_xevent (MetaScreen *screen,
                           XEvent     *xevent)
{
  MetaBackend *backend = meta_get_backend ();
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);

  if (meta_cursor_tracker_handle_xevent (cursor_tracker, xevent))
    return TRUE;

  return FALSE;
}
