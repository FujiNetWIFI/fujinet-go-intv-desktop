/*
 * Importing the BIOS and cartridges -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_IMPORT_H
#define INTV_WIN_IMPORT_H

#include <windows.h>

#include "intvsession.h"

/* Asks for BIOS files (several, or one when single) or a folder, imports
 * each, and says in one message box what came of it. Returns how many paths
 * imported something, or -1 when the chooser was cancelled. */
int intv_import_bios(HWND owner, intvsession *session, int folder, int single);

/* Asks for cartridge images or zips (several) and copies them onto FujiNet's
 * SD host, then says so in one message box. Returns how many were copied, or
 * -1 when the chooser was cancelled. */
int intv_import_carts(HWND owner, intvsession *session);

/* Opens the SD host's folder in Explorer. */
void intv_open_sd_folder(HWND owner, intvsession *session);

#endif
