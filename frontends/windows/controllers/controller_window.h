/*
 * The Win32 Controllers window (F9): the Intellivision II handsets (and the
 * ECS pair when the ECS is on) and the console's RESET as the real parts look,
 * drawn from the session's layout, with Map, the system buttons and the
 * gamepad rows, in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "intvsession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void intv_controller_window_toggle(HWND parent, intvsession *session);
/* Show without toggling (Settings' Open Controllers). */
void intv_controller_window_show(HWND parent, intvsession *session);
int  intv_controller_window_visible(void);

/* Called when the gamepad set or the bindings changed outside the window
 * (Settings), so it refreshes. */
void intv_controller_window_gamepads_changed(void);
