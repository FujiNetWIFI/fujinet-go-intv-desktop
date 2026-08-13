/*
 * EcsKeyboardWindow for the Windows frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_ECSKBD_WINDOW_H
#define INTV_WIN_ECSKBD_WINDOW_H

#include <windows.h>

#include "intvsession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. */
void intv_ecskbd_window_toggle(HWND parent, intvsession *session);

#endif /* INTV_WIN_ECSKBD_WINDOW_H */
