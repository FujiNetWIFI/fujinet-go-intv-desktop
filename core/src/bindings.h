/*
 * bindings -- private interface to core/src/bindings.c, for the two other
 * translators that need to see inside the remappable-bindings table:
 * session.c (to seed/reload it at session start) and gamepad_sdl.c (to
 * resolve a gamepad button against it on its own polling thread, and to
 * suppress a D-pad direction the user has remapped away from the disc).
 * intvsession.h carries the frontend-facing subset of this API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_BINDINGS_H
#define INTV_BINDINGS_H

#include "intvsession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reloads the bindings table: every slot to its upstream default, then
 * overridden by whatever "bindings" holds in s's settings store. Called once
 * per intvsession_new (see session.c) -- process-global storage, like
 * gamepad_sdl.c's own pad table, because jzIntv's machine is a process
 * singleton (core/jzintv/intv_host.h) and there is only ever one session's
 * settings store live at a time. */
void bindings_init(struct intvsession *s);

/* Gamepad-button lookup, scoped to one side only -- unlike the keyboard
 * (intvsession_key_from_keysym_bound), a physical pad drives at most one
 * side at a time (intvsession_gamepad_assign), so the same button index is
 * free to mean different things on different sides; nothing to steal across
 * them. MAP_NONE if `button` drives nothing on `side`. */
intvsession_key_mapping bindings_key_from_button(intvsession_pad_side side,
                                                  intvsession_pad_button button);

/* True iff `button` is NOT currently bound to any keypad key on `side` --
 * gamepad_sdl.c's poll_sticks gates each D-pad direction it reads for the
 * disc on this, so a D-pad button the user remapped to a keypad digit stops
 * also nudging the disc every poll. */
int bindings_button_is_free(intvsession_pad_side side,
                            intvsession_pad_button button);

#ifdef __cplusplus
}
#endif

#endif /* INTV_BINDINGS_H */
