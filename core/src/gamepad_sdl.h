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

#include <stdint.h>

#include "intvsession.h"

#ifdef __cplusplus
extern "C" {
#endif

struct intvsession;

/* Starts SDL's gamepad subsystem and the polling thread. `s` supplies the
 * settings store (pad_assign), the hot-plug toast text and the ECS state (pad
 * sides 2 and 3); NULL works without them (tests). Returns 0 on success. Safe
 * to call more than once (a no-op if already running). */
int intv_gamepad_start(struct intvsession *s);
void intv_gamepad_stop(void);

/* Resets every pad slot's "last disc position sent" cache to "none", so the
 * very next poll re-asserts a stick or D-pad still held. Call right after
 * input_clear_all (session.c's intvsession_reset_game does, around
 * intv_host_reset): otherwise a stick or D-pad still held at the moment of
 * reset reads as "unchanged" on every subsequent poll and the disc stays
 * stuck centered in the machine until the player physically releases and
 * re-presses it. */
void intv_gamepad_forget_disc(void);

/* Pure functions -- see gamepad_sdl.c for the full contract of each;
 * unit-tested in core/tests/gamepad_test.c without any hardware attached. */
int intv_disc_from_stick(float x, float y, float deadzone);
int intv_disc_from_dpad(int up, int down, int left, int right);
int intv_pad_for_port(const int *bindings, int npads, int side);

/* Translates one of SDL3's SDL_GamepadButton values (its underlying type is
 * Uint8, i.e. uint8_t -- taken as a plain uint8_t here so this header stays
 * SDL-free, same reasoning as intvsession_pad_button itself) to the public
 * intvsession_pad_button numbering. An explicit table rather than a cast, so
 * a future SDL header reordering can't silently desync the two (see
 * intvsession.h's own comment on this enum). INTVSESSION_PAD_BTN_NONE for
 * any SDL button this project doesn't name (paddles, misc, touchpad, etc.).
 * Pure function; unit-tested in gamepad_test.c. */
intvsession_pad_button pad_button_from_sdl(uint8_t button);

/* The inverse of pad_button_from_sdl -- poll_sticks needs it to turn a
 * bindings.c disc-segment button (an intvsession_pad_button) back into the
 * SDL_GamepadButton index SDL_GetGamepadButton wants. Returns -1 for
 * INTVSESSION_PAD_BTN_NONE and for anything this project doesn't name; the
 * two tables are kept in sync by gamepad_test.c round-tripping every named
 * button through both. Pure function. */
int sdl_button_from_pad(intvsession_pad_button button);

#ifdef __cplusplus
}
#endif

#endif /* INTV_GAMEPAD_SDL_H */
