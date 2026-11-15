/*
 * debugger_internal -- what the prompt (prompt.c) and the I/O getters
 * (iostate.c) share with the engine (debugger.c). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_DEBUGGER_INTERNAL_H
#define INTV_DEBUGGER_INTERNAL_H

#include "intvdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Marks something a window should redraw for (a command ran). */
void intvdebug_touch(intvdebug *dbg);

/* The Video tab's text hook, or NULL (see intvdebug_set_video_text). */
const char *(*intvdebug_video_text_hook(void))(const char *section);

/* Register names the expressions know, for completion. NULL-terminated. */
extern const char *const intvdebug_reg_names[];

#ifdef __cplusplus
}
#endif

#endif /* INTV_DEBUGGER_INTERNAL_H */
