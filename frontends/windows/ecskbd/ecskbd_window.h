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

/* Called from the frontend's message pump before TranslateMessage so
 * keyboard input reaches the ECS matrix regardless of which child control
 * (an ordinary key, Ctrl, Shift) currently holds focus. Returns 1 when the
 * message was consumed. */
int intv_ecskbd_pretranslate(MSG *msg);

#endif /* INTV_WIN_ECSKBD_WINDOW_H */
