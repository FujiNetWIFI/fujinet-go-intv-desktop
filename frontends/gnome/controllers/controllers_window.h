/*
 * The Controllers window (F9): the Intellivision II handsets, the ECS pair
 * when the ECS is on, and the console's RESET, drawn with cairo from the
 * core's layout. See controllers_window.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if it is showing. `parent` is the main window; it is
 * used the first time to make the window transient for it, and keys typed
 * here that the window does not take itself go to the machine the way the
 * main window sends them. */
void intv_controllers_window_toggle(GtkWindow *parent, intvsession *session);
/* Shows it (or raises it). */
void intv_controllers_window_show(GtkWindow *parent, intvsession *session);
gboolean intv_controllers_window_is_visible(void);
/* Bindings changed outside the window (Preferences' Reset Controls): redraw
 * it and rebuild what the pad rows offer. */
void intv_controllers_window_refresh(void);

G_END_DECLS
