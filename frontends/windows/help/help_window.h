/*
 * The Win32 Help window (Ctrl+Shift+H): the topics of data/help/help.md,
 * built in (intvhelp.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "../win_common.h"

/* Shows the Help at id: a heading's slug or a context ("debugger/cpu",
 * "prefs/Machine"), or "?words" to search; NULL stays where it was. owner
 * is any of the app's windows. */
void intv_help_show(HWND owner, const char *id);

/* What Ctrl+Shift+H opens for a window, and with f1 F1 too (not where F1
 * is the PC's). */
void intv_help_set_context(HWND window, const char *context, int f1);

/* The Help for a window (in the Help, its search). */
void intv_help_for(HWND window);

/* Ctrl+Shift+H and F1 in every window, and the Help window's own keys: first
 * in the main loop. 1 if consumed. */
int intv_help_pretranslate(MSG *msg);
