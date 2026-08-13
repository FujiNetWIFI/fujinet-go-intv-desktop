/*
 * keymap_test -- checks intvsession_key_from_keysym against a representative
 * sample of upstream jzIntv's own default keyboard bindings (see
 * core/src/intv_keymap.c's header). Pure function, no emulator boot needed.
 */
#include <stdint.h>
#include <stdio.h>

#include "intvsession.h"

static int failed = 0;

static void expect_key(uint32_t keysym, intvsession_pad_side side,
                       intvsession_key key)
{
    intvsession_key_mapping m = intvsession_key_from_keysym(keysym);
    if (m.kind != INTVSESSION_MAP_KEY || m.side != side || m.key != key) {
        fprintf(stderr, "keymap_test: FAILED: keysym 0x%x\n", keysym);
        failed = 1;
    }
}

static void expect_disc(uint32_t keysym, intvsession_pad_side side,
                        int direction)
{
    intvsession_key_mapping m = intvsession_key_from_keysym(keysym);
    if (m.kind != INTVSESSION_MAP_DISC || m.side != side ||
        m.direction != direction) {
        fprintf(stderr, "keymap_test: FAILED: keysym 0x%x\n", keysym);
        failed = 1;
    }
}

static void expect_none(uint32_t keysym)
{
    intvsession_key_mapping m = intvsession_key_from_keysym(keysym);
    if (m.kind != INTVSESSION_MAP_NONE) {
        fprintf(stderr, "keymap_test: FAILED: keysym 0x%x should be "
                        "unmapped\n", keysym);
        failed = 1;
    }
}

int main(void)
{
    /* Numpad -> left controller keypad (mapping.c: KP_7..KP_ENTER). */
    expect_key(INTVSESSION_KEYSYM_KP_7, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1);
    expect_key(INTVSESSION_KEYSYM_KP_0, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_CLEAR);
    expect_key(INTVSESSION_KEYSYM_KP_PERIOD, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_0);
    expect_key(INTVSESSION_KEYSYM_KP_ENTER, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_ENTER);

    /* Number row -> right controller keypad (mapping.c: "1".."="). */
    expect_key('1', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_1);
    expect_key('9', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_9);
    expect_key('-', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_CLEAR);
    expect_key('0', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_0);
    expect_key('=', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_ENTER);

    /* Action buttons -- upstream crosses left/right modifiers with
     * right/left controllers (mapping.c: RSHIFT..LCTRL). */
    expect_key(INTVSESSION_KEYSYM_RSHIFT, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_TOP);
    expect_key(INTVSESSION_KEYSYM_LSHIFT, INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_TOP);
    expect_key(INTVSESSION_KEYSYM_RCTRL, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_RIGHT);

    /* Arrows -> left disc (mapping.c: RIGHT/UP/LEFT/DOWN). */
    expect_disc(INTVSESSION_KEYSYM_UP, INTVSESSION_PAD_LEFT, 4);
    expect_disc(INTVSESSION_KEYSYM_RIGHT, INTVSESSION_PAD_LEFT, 0);
    expect_disc(INTVSESSION_KEYSYM_DOWN, INTVSESSION_PAD_LEFT, 12);
    expect_disc(INTVSESSION_KEYSYM_LEFT, INTVSESSION_PAD_LEFT, 8);

    /* IJKM diagonals -> left disc, case-insensitive (mapping.c: K/O/I/U/
     * J/N/M/,). */
    expect_disc('k', INTVSESSION_PAD_LEFT, 0);
    expect_disc('O', INTVSESSION_PAD_LEFT, 2);
    expect_disc('i', INTVSESSION_PAD_LEFT, 4);
    expect_disc(',', INTVSESSION_PAD_LEFT, 14);

    /* DRWEASZXC -> right disc (mapping.c: D/R/E/W/S/Z/X/C). */
    expect_disc('d', INTVSESSION_PAD_RIGHT, 0);
    expect_disc('W', INTVSESSION_PAD_RIGHT, 6);
    expect_disc('c', INTVSESSION_PAD_RIGHT, 14);

    /* Reserved-for-frontends and non-pad keys must not map. */
    expect_none('Q');   /* upstream's QUIT hotkey, not a pad action */
    expect_none('F');   /* ECS-keyboard-only in upstream, no pad action */
    expect_none(0x9999); /* nonsense keysym */

    /* ---- ECS keyboard map (intvsession_ecs_key_from_keysym) -------------
     * A separate function/table from the pad map above -- 'Q'/'F' are
     * unmapped there (upstream hotkeys/no pad action) but ARE mapped here
     * (mapping.c's ECS Keyboard column). */
    if (intvsession_ecs_key_from_keysym('Q') != INTVSESSION_ECS_KEY_Q) {
        fprintf(stderr, "keymap_test: FAILED: ecs 'Q'\n");
        failed = 1;
    }
    if (intvsession_ecs_key_from_keysym('f') != INTVSESSION_ECS_KEY_F) {
        fprintf(stderr, "keymap_test: FAILED: ecs 'f' (case-insensitive)\n");
        failed = 1;
    }
    if (intvsession_ecs_key_from_keysym(INTVSESSION_KEYSYM_ESCAPE) !=
        INTVSESSION_ECS_KEY_ESC) {
        fprintf(stderr, "keymap_test: FAILED: ecs ESCAPE\n");
        failed = 1;
    }
    if (intvsession_ecs_key_from_keysym(INTVSESSION_KEYSYM_RSHIFT) !=
        INTVSESSION_ECS_KEY_SHIFT) {
        fprintf(stderr, "keymap_test: FAILED: ecs RSHIFT\n");
        failed = 1;
    }
    if (intvsession_ecs_key_from_keysym(INTVSESSION_KEYSYM_LSHIFT) !=
        INTVSESSION_ECS_KEY_SHIFT) {
        fprintf(stderr, "keymap_test: FAILED: ecs LSHIFT\n");
        failed = 1;
    }
    /* F10/F11/F12 stay reserved for the frontends in the ECS map too --
     * there is no INTVSESSION_KEYSYM_F10/11/12 to even pass in, so this is
     * really just confirming an unrelated/garbage keysym is INTVSESSION_
     * ECS_KEY_NONE, same contract as the pad map's expect_none. */
    if (intvsession_ecs_key_from_keysym(0x9999) != INTVSESSION_ECS_KEY_NONE) {
        fprintf(stderr, "keymap_test: FAILED: ecs nonsense keysym\n");
        failed = 1;
    }

    if (failed) {
        fprintf(stderr, "keymap_test: FAILED\n");
        return 1;
    }
    printf("keymap_test: OK\n");
    return 0;
}
