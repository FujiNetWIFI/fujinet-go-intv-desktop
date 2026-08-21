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
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_KEY, .side = side,
                                  .key = k };
    return m;
}

static intvsession_key_mapping disc(intvsession_pad_side side, int dir)
{
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_DISC, .side = side,
                                  .direction = dir };
    return m;
}

static intvsession_key_mapping none(void)
{
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_NONE };
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

/* Every mapping below mirrors one row of cfg_key_bind[]'s column 3 ("ECS
 * Keyboard setup") in the same block of the staged tree's mapping.c that
 * intvsession_key_from_keysym's column-0 mappings come from (its
 * "ESCAPE".."COMMA"/"D".."C"/"Q".."LESS" rows) -- search that file for the
 * literal "KEYB_*" action name in each comment to cross-check. The
 * secondary "column 2" alternate-keypad block (mapping.c's "Q".."P" ->
 * KEYB_1..KEYB_0 rows further down the file) is deliberately not
 * transcribed, matching the column-0 mapping's own choice to only use the
 * table's first (default) block.
 *
 * Several host keys in that block bind ONLY to one of upstream's shifted-
 * symbol convenience actions (KEYB_MINUS, KEYB_EQUAL, KEYB_QUOTE,
 * KEYB_HASH, KEYB_DOLLAR, KEYB_PLUS, KEYB_SLASH, KEYB_STAR, KEYB_LPAREN,
 * KEYB_RPAREN, KEYB_CARET, KEYB_QUEST, KEYB_COLON, KEYB_GREATER,
 * KEYB_LESS) -- intv_host.h's ecs_key_codes table deliberately omits those
 * (see its own comment): the real shifted character reaches the emulated
 * ECS the same way it does on real hardware, by holding
 * INTVSESSION_ECS_KEY_SHIFT with the unshifted base key, so those host keys
 * map to no ECS key here (INTVSESSION_ECS_KEY_NONE) rather than silently
 * doing nothing useful. */
intvsession_ecs_key intvsession_ecs_key_from_keysym(uint32_t keysym)
{
    if (keysym >= 'a' && keysym <= 'z')
        keysym -= ('a' - 'A');

    switch (keysym)
    {
    case INTVSESSION_KEYSYM_ESCAPE: return INTVSESSION_ECS_KEY_ESC;

    /* ---- the numeric keypad -- mapping.c: "KP_7".."KP_ENTER" ---------- */
    case INTVSESSION_KEYSYM_KP_7: return INTVSESSION_ECS_KEY_1;
    case INTVSESSION_KEYSYM_KP_8: return INTVSESSION_ECS_KEY_2;
    case INTVSESSION_KEYSYM_KP_9: return INTVSESSION_ECS_KEY_3;
    case INTVSESSION_KEYSYM_KP_4: return INTVSESSION_ECS_KEY_4;
    case INTVSESSION_KEYSYM_KP_5: return INTVSESSION_ECS_KEY_5;
    case INTVSESSION_KEYSYM_KP_6: return INTVSESSION_ECS_KEY_6;
    case INTVSESSION_KEYSYM_KP_1: return INTVSESSION_ECS_KEY_7;
    case INTVSESSION_KEYSYM_KP_2: return INTVSESSION_ECS_KEY_8;
    case INTVSESSION_KEYSYM_KP_3: return INTVSESSION_ECS_KEY_9;
    case INTVSESSION_KEYSYM_KP_0: return INTVSESSION_ECS_KEY_0;
    case INTVSESSION_KEYSYM_KP_PERIOD: return INTVSESSION_ECS_KEY_PERIOD;
    case INTVSESSION_KEYSYM_KP_ENTER:  return INTVSESSION_ECS_KEY_ENTER;

    /* ---- the number row -- mapping.c: "1".."0" (its "-"/"=" are
     * shifted-symbol-only, see the function comment) ---------------------- */
    case '1': return INTVSESSION_ECS_KEY_1;
    case '2': return INTVSESSION_ECS_KEY_2;
    case '3': return INTVSESSION_ECS_KEY_3;
    case '4': return INTVSESSION_ECS_KEY_4;
    case '5': return INTVSESSION_ECS_KEY_5;
    case '6': return INTVSESSION_ECS_KEY_6;
    case '7': return INTVSESSION_ECS_KEY_7;
    case '8': return INTVSESSION_ECS_KEY_8;
    case '9': return INTVSESSION_ECS_KEY_9;
    case '0': return INTVSESSION_ECS_KEY_0;

    /* ---- modifiers -- mapping.c: "RSHIFT"/"LSHIFT" -> KEYB_SHIFT,
     * "RCTRL"/"LCTRL" -> KEYB_CTRL (both sides collapse to ECS's single
     * physical SHIFT/CTRL key, unlike the crossed action-button mapping
     * above). ---------------------------------------------------------- */
    case INTVSESSION_KEYSYM_RSHIFT:
    case INTVSESSION_KEYSYM_LSHIFT: return INTVSESSION_ECS_KEY_SHIFT;
    case INTVSESSION_KEYSYM_RCTRL:
    case INTVSESSION_KEYSYM_LCTRL:  return INTVSESSION_ECS_KEY_CTRL;

    /* ---- arrows -- mapping.c: "RIGHT"/"UP"/"LEFT"/"DOWN" --------------- */
    case INTVSESSION_KEYSYM_RIGHT: return INTVSESSION_ECS_KEY_RIGHT;
    case INTVSESSION_KEYSYM_UP:    return INTVSESSION_ECS_KEY_UP;
    case INTVSESSION_KEYSYM_LEFT:  return INTVSESSION_ECS_KEY_LEFT;
    case INTVSESSION_KEYSYM_DOWN:  return INTVSESSION_ECS_KEY_DOWN;

    /* ---- letters -- mapping.c: "K".."C" (IJKM/O/U/N/, block plus
     * DRWEASZXC block, both KEYB_<letter> 1:1) --------------------------- */
    case 'K': return INTVSESSION_ECS_KEY_K;
    case 'O': return INTVSESSION_ECS_KEY_O;
    case 'I': return INTVSESSION_ECS_KEY_I;
    case 'U': return INTVSESSION_ECS_KEY_U;
    case 'J': return INTVSESSION_ECS_KEY_J;
    case 'N': return INTVSESSION_ECS_KEY_N;
    case 'M': return INTVSESSION_ECS_KEY_M;
    case ',': return INTVSESSION_ECS_KEY_COMMA;
    case 'D': return INTVSESSION_ECS_KEY_D;
    case 'R': return INTVSESSION_ECS_KEY_R;
    case 'E': return INTVSESSION_ECS_KEY_E;
    case 'W': return INTVSESSION_ECS_KEY_W;
    case 'S': return INTVSESSION_ECS_KEY_S;
    case 'Z': return INTVSESSION_ECS_KEY_Z;
    case 'X': return INTVSESSION_ECS_KEY_X;
    case 'C': return INTVSESSION_ECS_KEY_C;

    /* ---- the rest of the alphabet -- mapping.c: "Q".."B" --------------- */
    case 'Q': return INTVSESSION_ECS_KEY_Q;
    case 'T': return INTVSESSION_ECS_KEY_T;
    case 'Y': return INTVSESSION_ECS_KEY_Y;
    case 'P': return INTVSESSION_ECS_KEY_P;
    case 'A': return INTVSESSION_ECS_KEY_A;
    case 'F': return INTVSESSION_ECS_KEY_F;
    case 'G': return INTVSESSION_ECS_KEY_G;
    case 'H': return INTVSESSION_ECS_KEY_H;
    case 'L': return INTVSESSION_ECS_KEY_L;
    case 'V': return INTVSESSION_ECS_KEY_V;
    case 'B': return INTVSESSION_ECS_KEY_B;

    /* ---- punctuation/whitespace -- mapping.c: "."/";"/"SPACE"/"RETURN"/
     * "BACKSPACE" (upstream really does bind Backspace to KEYB_LEFT --
     * ECS's own keyboard has no dedicated Backspace key). --------------- */
    case '.': return INTVSESSION_ECS_KEY_PERIOD;
    case ';': return INTVSESSION_ECS_KEY_SEMI;
    case ' ': return INTVSESSION_ECS_KEY_SPACE;
    case INTVSESSION_KEYSYM_RETURN:    return INTVSESSION_ECS_KEY_ENTER;
    case INTVSESSION_KEYSYM_BACKSPACE: return INTVSESSION_ECS_KEY_LEFT;

    default: return INTVSESSION_ECS_KEY_NONE;
    }
}
