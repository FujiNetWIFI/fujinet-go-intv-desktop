/*
 * gamepad_sdl -- private interface. intvsession_gamepad_{count,name,assign}
 * (see intvsession.h for those) are implemented in gamepad_sdl.c too, but
 * are public API and declared there, not here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_GAMEPAD_SDL_H
#define INTV_GAMEPAD_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts SDL's gamepad subsystem and the polling thread. Returns 0 on
 * success. Safe to call more than once (a no-op if already running). */
int intv_gamepad_start(void);
void intv_gamepad_stop(void);

/* Pure functions -- see gamepad_sdl.c for the full contract of each;
 * unit-tested in core/tests/gamepad_test.c without any hardware attached. */
int intv_disc_from_stick(float x, float y, float deadzone);
int intv_pad_for_port(const int *bindings, int npads, int side);

#ifdef __cplusplus
}
#endif

#endif /* INTV_GAMEPAD_SDL_H */
