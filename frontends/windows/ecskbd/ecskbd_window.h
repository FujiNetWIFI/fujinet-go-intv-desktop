/*
 * The Win32 ECS Keyboard window (F10): the Computer Keyboard drawn from the
 * session's layout, in a fixed-size tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_ECSKBD_WINDOW_H
#define INTV_WIN_ECSKBD_WINDOW_H

#include <windows.h>

#include "intvsession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. Hidden at startup. */
void intv_ecskbd_window_toggle(HWND parent, intvsession *session);
int  intv_ecskbd_window_visible(void);

#endif /* INTV_WIN_ECSKBD_WINDOW_H */
