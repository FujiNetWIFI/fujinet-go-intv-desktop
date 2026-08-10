/*
 * intvsession_key_from_keysym -- see intvsession.h for the contract and its
 * known limitation (no simultaneous-key disc combos).
 *
 * Every mapping below mirrors one line of the staged jzIntv tree's
 * src/cfg/mapping.c cfg_key_bind[] table (its column 0, the normal-play
 * action map) -- search that file for the literal action name in each
 * comment (e.g. "PD0L_KP1") to cross-check. Only pad-relevant actions are
 * carried over: jzIntv's own hotkeys (RESET, QUIT, SHOT, MOVIE, ...) and its
 * emulated-ECS-keyboard column are a frontend's concern, not this pure
 * function's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "intvsession.h"

static intvsession_key_mapping key(intvsession_pad_side side,
                                   intvsession_key k)
{
    intvsession_key_mapping m = {INTVSESSION_MAP_KEY, side, k, 0};
    return m;
}

static intvsession_key_mapping disc(intvsession_pad_side side, int dir)
{
    intvsession_key_mapping m = {INTVSESSION_MAP_DISC, side, 0, dir};
    return m;
}

static intvsession_key_mapping none(void)
{
    intvsession_key_mapping m = {INTVSESSION_MAP_NONE, 0, 0, 0};
    return m;
}

/* Clock positions for the 8 primary compass directions, in intv_host.h's
 * disc-position numbering (0 = E, clockwise; even positions only -- the odd
 * ones are half-steps with no single key of their own here). */
enum {
    DIR_E = 0, DIR_NE = 2, DIR_N = 4, DIR_NW = 6,
    DIR_W = 8, DIR_SW = 10, DIR_S = 12, DIR_SE = 14,
};

intvsession_key_mapping intvsession_key_from_keysym(uint32_t keysym)
{
    /* Uppercase ASCII letters, so 'k' and 'K' behave the same -- matches
     * upstream's own key names, which are case-insensitive by construction
     * (they're symbolic names like "K", not literal keysyms). */
    if (keysym >= 'a' && keysym <= 'z')
        keysym -= ('a' - 'A');

    switch (keysym)
    {
    /* ---- the numeric keypad -- mapping.c: "KP_7".."KP_ENTER" ---------- */
    case INTVSESSION_KEYSYM_KP_7: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1);
    case INTVSESSION_KEYSYM_KP_8: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_2);
    case INTVSESSION_KEYSYM_KP_9: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_3);
    case INTVSESSION_KEYSYM_KP_4: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_4);
    case INTVSESSION_KEYSYM_KP_5: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5);
    case INTVSESSION_KEYSYM_KP_6: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_6);
    case INTVSESSION_KEYSYM_KP_1: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_7);
    case INTVSESSION_KEYSYM_KP_2: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_8);
    case INTVSESSION_KEYSYM_KP_3: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_9);
    case INTVSESSION_KEYSYM_KP_0: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_CLEAR);
    case INTVSESSION_KEYSYM_KP_PERIOD: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_0);
    case INTVSESSION_KEYSYM_KP_ENTER: return key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_ENTER);

    /* ---- the number row -- mapping.c: "1".."=" ------------------------ */
    case '1': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_1);
    case '2': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_2);
    case '3': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_3);
    case '4': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_4);
    case '5': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_5);
    case '6': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_6);
    case '7': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_7);
    case '8': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_8);
    case '9': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_9);
    case '-': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_CLEAR);
    case '0': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_0);
    case '=': return key(INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_ENTER);

    /* ---- action buttons -- mapping.c: "RSHIFT".."LCTRL" ---------------
     * Upstream really does cross them this way: the right-hand modifier
     * keys drive the LEFT controller's action buttons, and vice versa. */
    case INTVSESSION_KEYSYM_RSHIFT: return key(INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_TOP);
    case INTVSESSION_KEYSYM_RALT:   return key(INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_LEFT);
    case INTVSESSION_KEYSYM_RCTRL:  return key(INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_RIGHT);
    case INTVSESSION_KEYSYM_LSHIFT: return key(INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_TOP);
    case INTVSESSION_KEYSYM_LALT:   return key(INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_LEFT);
    case INTVSESSION_KEYSYM_LCTRL:  return key(INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_RIGHT);

    /* ---- movement -- mapping.c: "RIGHT"/"UP"/"LEFT"/"DOWN" (left disc) */
    case INTVSESSION_KEYSYM_RIGHT: return disc(INTVSESSION_PAD_LEFT, DIR_E);
    case INTVSESSION_KEYSYM_UP:    return disc(INTVSESSION_PAD_LEFT, DIR_N);
    case INTVSESSION_KEYSYM_LEFT:  return disc(INTVSESSION_PAD_LEFT, DIR_W);
    case INTVSESSION_KEYSYM_DOWN:  return disc(INTVSESSION_PAD_LEFT, DIR_S);

    /* ---- IJKM/O/U/N/, -- mapping.c's second left-disc binding --------- */
    case 'K': return disc(INTVSESSION_PAD_LEFT, DIR_E);
    case 'O': return disc(INTVSESSION_PAD_LEFT, DIR_NE);
    case 'I': return disc(INTVSESSION_PAD_LEFT, DIR_N);
    case 'U': return disc(INTVSESSION_PAD_LEFT, DIR_NW);
    case 'J': return disc(INTVSESSION_PAD_LEFT, DIR_W);
    case 'N': return disc(INTVSESSION_PAD_LEFT, DIR_SW);
    case 'M': return disc(INTVSESSION_PAD_LEFT, DIR_S);
    case ',': return disc(INTVSESSION_PAD_LEFT, DIR_SE);

    /* ---- DRWEASZXC -- mapping.c's right-disc binding ------------------- */
    case 'D': return disc(INTVSESSION_PAD_RIGHT, DIR_E);
    case 'R': return disc(INTVSESSION_PAD_RIGHT, DIR_NE);
    case 'E': return disc(INTVSESSION_PAD_RIGHT, DIR_N);
    case 'W': return disc(INTVSESSION_PAD_RIGHT, DIR_NW);
    case 'S': return disc(INTVSESSION_PAD_RIGHT, DIR_W);
    case 'Z': return disc(INTVSESSION_PAD_RIGHT, DIR_SW);
    case 'X': return disc(INTVSESSION_PAD_RIGHT, DIR_S);
    case 'C': return disc(INTVSESSION_PAD_RIGHT, DIR_SE);

    default: return none();
    }
}
