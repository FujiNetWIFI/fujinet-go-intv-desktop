/*
 * The System ROMs window -- see roms.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_ROMS_H
#define INTV_WIN_ROMS_H

#include <windows.h>

#include "intvsession.h"

/* imported: a BIOS import succeeded from this window (the main window shows
 * its picture again, the settings refresh the ECS option). */
typedef struct {
    void (*imported)(void);
} intv_roms_hooks;

/* Opens (or raises) the window. */
void intv_roms_show(HWND parent, intvsession *session, const intv_roms_hooks *hooks);

#endif
