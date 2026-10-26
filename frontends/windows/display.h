/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_DISPLAY_H
#define INTV_WIN_DISPLAY_H

#include <windows.h>

#include "intvsession.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND intv_display_create(HWND parent, intvsession *session);
/* Stops the render thread; call before the parent is destroyed. */
void intv_display_destroy(void);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *intv_display_renderer(void);

#endif
