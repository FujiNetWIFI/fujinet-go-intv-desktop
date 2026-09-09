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
    /* Every one of the 50 PD0L_ and PD0R_ rows in mapping.c's cfg_key_bind[]
     * column 1 (the default one-player map intv_keymap.c mirrors) is checked
     * below -- exhaustively, not by sample, so that a dropped or transposed
     * row cannot pass. The frontends each translate their toolkit's own key
     * values into these keysyms; the whole point of that translation is to
     * land on exactly this table, so it is the one contract worth pinning. */

    /* Numpad -> left controller keypad (mapping.c: KP_7..KP_ENTER). Note
     * upstream's deliberate offset: the numpad's own 7/8/9 row is the
     * Intellivision keypad's 1/2/3 row, KP_0 is Clear and KP_PERIOD is 0. */
    expect_key(INTVSESSION_KEYSYM_KP_7, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1);
    expect_key(INTVSESSION_KEYSYM_KP_8, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_2);
    expect_key(INTVSESSION_KEYSYM_KP_9, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_3);
    expect_key(INTVSESSION_KEYSYM_KP_4, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_4);
    expect_key(INTVSESSION_KEYSYM_KP_5, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5);
    expect_key(INTVSESSION_KEYSYM_KP_6, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_6);
    expect_key(INTVSESSION_KEYSYM_KP_1, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_7);
    expect_key(INTVSESSION_KEYSYM_KP_2, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_8);
    expect_key(INTVSESSION_KEYSYM_KP_3, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_9);
    expect_key(INTVSESSION_KEYSYM_KP_0, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_CLEAR);
    expect_key(INTVSESSION_KEYSYM_KP_PERIOD, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_0);
    expect_key(INTVSESSION_KEYSYM_KP_ENTER, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_ENTER);

    /* Number row -> right controller keypad (mapping.c: "1".."="). */
    expect_key('1', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_1);
    expect_key('2', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_2);
    expect_key('3', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_3);
    expect_key('4', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_4);
    expect_key('5', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_5);
    expect_key('6', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_6);
    expect_key('7', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_7);
    expect_key('8', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_8);
    expect_key('9', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_9);
    expect_key('-', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_CLEAR);
    expect_key('0', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_0);
    expect_key('=', INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_ENTER);

    /* Action buttons -- upstream crosses left/right modifiers with
     * right/left controllers (mapping.c: RSHIFT..LCTRL). */
    expect_key(INTVSESSION_KEYSYM_RSHIFT, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_TOP);
    expect_key(INTVSESSION_KEYSYM_RALT, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_LEFT);
    expect_key(INTVSESSION_KEYSYM_RCTRL, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_RIGHT);
    expect_key(INTVSESSION_KEYSYM_LSHIFT, INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_TOP);
    expect_key(INTVSESSION_KEYSYM_LALT, INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_LEFT);
    expect_key(INTVSESSION_KEYSYM_LCTRL, INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_RIGHT);

    /* Arrows -> left disc (mapping.c: RIGHT/UP/LEFT/DOWN). */
    expect_disc(INTVSESSION_KEYSYM_RIGHT, INTVSESSION_PAD_LEFT, 0);
    expect_disc(INTVSESSION_KEYSYM_UP, INTVSESSION_PAD_LEFT, 4);
    expect_disc(INTVSESSION_KEYSYM_LEFT, INTVSESSION_PAD_LEFT, 8);
    expect_disc(INTVSESSION_KEYSYM_DOWN, INTVSESSION_PAD_LEFT, 12);

    /* IJKM diagonals -> left disc, case-insensitive (mapping.c: K/O/I/U/
     * J/N/M/,). */
    expect_disc('k', INTVSESSION_PAD_LEFT, 0);
    expect_disc('O', INTVSESSION_PAD_LEFT, 2);
    expect_disc('i', INTVSESSION_PAD_LEFT, 4);
    expect_disc('U', INTVSESSION_PAD_LEFT, 6);
    expect_disc('j', INTVSESSION_PAD_LEFT, 8);
    expect_disc('N', INTVSESSION_PAD_LEFT, 10);
    expect_disc('m', INTVSESSION_PAD_LEFT, 12);
    expect_disc(',', INTVSESSION_PAD_LEFT, 14);

    /* DRWEASZXC -> right disc (mapping.c: D/R/E/W/S/Z/X/C). */
    expect_disc('d', INTVSESSION_PAD_RIGHT, 0);
    expect_disc('R', INTVSESSION_PAD_RIGHT, 2);
    expect_disc('e', INTVSESSION_PAD_RIGHT, 4);
    expect_disc('W', INTVSESSION_PAD_RIGHT, 6);
    expect_disc('s', INTVSESSION_PAD_RIGHT, 8);
    expect_disc('Z', INTVSESSION_PAD_RIGHT, 10);
    expect_disc('x', INTVSESSION_PAD_RIGHT, 12);
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

    /* ---- the shifted layer ----------------------------------------------
     * Every character the ECS can only produce with SHIFT held. These are
     * pinned exhaustively rather than sampled because a wrong entry is
     * invisible in play -- it types some OTHER perfectly valid character,
     * and the ECS's shifted layer is unlike a PC's in ways nobody would
     * spot by eye ('%' is SHIFT+LEFT-ARROW, '/' is SHIFT+7). */
    {
        static const struct { uint32_t ch; intvsession_ecs_key key;
                              const char *name; } shifted[] = {
            { '=',  INTVSESSION_ECS_KEY_EQUAL,   "'='"  },
            { '"',  INTVSESSION_ECS_KEY_QUOTE,   "'\"'"  },
            { '#',  INTVSESSION_ECS_KEY_HASH,    "'#'"  },
            { '$',  INTVSESSION_ECS_KEY_DOLLAR,  "'$'"  },
            { '+',  INTVSESSION_ECS_KEY_PLUS,    "'+'"  },
            { '-',  INTVSESSION_ECS_KEY_MINUS,   "'-'"  },
            { '/',  INTVSESSION_ECS_KEY_SLASH,   "'/'"  },
            { '*',  INTVSESSION_ECS_KEY_STAR,    "'*'"  },
            { '(',  INTVSESSION_ECS_KEY_LPAREN,  "'('"  },
            { ')',  INTVSESSION_ECS_KEY_RPAREN,  "')'"  },
            { '^',  INTVSESSION_ECS_KEY_CARET,   "'^'"  },
            { '?',  INTVSESSION_ECS_KEY_QUEST,   "'?'"  },
            { '%',  INTVSESSION_ECS_KEY_PCT,     "'%'"  },
            { '\'', INTVSESSION_ECS_KEY_SQUOTE,  "apostrophe" },
            { ':',  INTVSESSION_ECS_KEY_COLON,   "':'"  },
            { '>',  INTVSESSION_ECS_KEY_GREATER, "'>'"  },
            { '<',  INTVSESSION_ECS_KEY_LESS,    "'<'"  },
        };
        size_t i;
        for (i = 0; i < sizeof(shifted) / sizeof(shifted[0]); i++)
            if (intvsession_ecs_key_from_keysym(shifted[i].ch) !=
                shifted[i].key) {
                fprintf(stderr, "keymap_test: FAILED: ecs %s\n",
                        shifted[i].name);
                failed = 1;
            }
    }

    /* The apostrophe is a deliberate divergence from upstream, which binds
     * both SDL QUOTE and QUOTEDBL to KEYB_QUOTE (mapping.c's "QUOTE"/
     * "QUOTEDBL" rows) -- so typing "'" on upstream jzIntv sends '"'. Here
     * they are separate keys, which is the whole point of resolving by
     * character. Pinned so nobody "corrects" it back to match upstream. */
    if (intvsession_ecs_key_from_keysym('\'') ==
        intvsession_ecs_key_from_keysym('"')) {
        fprintf(stderr, "keymap_test: FAILED: ecs ' and \" collapsed\n");
        failed = 1;
    }

    /* The unshifted base keys must NOT have moved to their shifted
     * partners: ',' is still COMMA (not LESS), '.' still PERIOD, ';' still
     * SEMI, and the digits still themselves. */
    if (intvsession_ecs_key_from_keysym(',') != INTVSESSION_ECS_KEY_COMMA ||
        intvsession_ecs_key_from_keysym('.') != INTVSESSION_ECS_KEY_PERIOD ||
        intvsession_ecs_key_from_keysym(';') != INTVSESSION_ECS_KEY_SEMI ||
        intvsession_ecs_key_from_keysym('5') != INTVSESSION_ECS_KEY_5) {
        fprintf(stderr, "keymap_test: FAILED: ecs unshifted keys moved\n");
        failed = 1;
    }

    /* '!' is the one shifted digit with no ECS home at all (nor '@', '&',
     * '_'). It must resolve to nothing rather than to whatever sits on the
     * "1" cap -- Shift+1 quietly typing '=' is exactly the positional
     * behaviour the character map replaces. */
    if (intvsession_ecs_key_from_keysym('!') != INTVSESSION_ECS_KEY_NONE ||
        intvsession_ecs_key_from_keysym('@') != INTVSESSION_ECS_KEY_NONE ||
        intvsession_ecs_key_from_keysym('&') != INTVSESSION_ECS_KEY_NONE) {
        fprintf(stderr, "keymap_test: FAILED: ecs '!'/'@'/'&' not NONE\n");
        failed = 1;
    }

    /* ---- intvsession_ecs_key_from_char ----------------------------------
     * A printable character outranks the physical key ... */
    if (intvsession_ecs_key_from_char('5', '%') != INTVSESSION_ECS_KEY_PCT) {
        fprintf(stderr, "keymap_test: FAILED: Shift+5 -> ECS '%%'\n");
        failed = 1;
    }
    /* ... including through the HID fallback band, which is where a key
     * with no curated symbol of its own lands. On Windows the "/" key
     * resolves exactly this way, and reaching ECS '/' through it is the
     * whole point. */
    if (intvsession_ecs_key_from_char(INTVSESSION_KEYSYM_HID_BASE + 0x38,
                                      '/') != INTVSESSION_ECS_KEY_SLASH) {
        fprintf(stderr, "keymap_test: FAILED: HID-band '/' -> ECS '/'\n");
        failed = 1;
    }
    /* ... but NOT for a key with a curated symbol. With NumLock on every
     * toolkit reports '7' for the numeric keypad's 7, while the map sends
     * KP_7 to ECS "1" (upstream's keypad-shaped layout). Letting the
     * character win there would silently re-lay the whole numpad. */
    if (intvsession_ecs_key_from_char(INTVSESSION_KEYSYM_KP_7, '7') !=
        INTVSESSION_ECS_KEY_1) {
        fprintf(stderr, "keymap_test: FAILED: numpad 7 must stay ECS '1'\n");
        failed = 1;
    }
    /* Same guard, arrows: '^' is SHIFT+UP on an ECS, and the cursor keys
     * have to stay cursor keys. */
    if (intvsession_ecs_key_from_char(INTVSESSION_KEYSYM_UP, '^') !=
        INTVSESSION_ECS_KEY_UP) {
        fprintf(stderr, "keymap_test: FAILED: Up arrow must stay ECS UP\n");
        failed = 1;
    }
    /* No character (a Ctrl combo, a modifier) falls back to the key. */
    if (intvsession_ecs_key_from_char('a', 0) != INTVSESSION_ECS_KEY_A ||
        intvsession_ecs_key_from_char(INTVSESSION_KEYSYM_LCTRL, 0) !=
            INTVSESSION_ECS_KEY_CTRL) {
        fprintf(stderr, "keymap_test: FAILED: ecs keysym fallback\n");
        failed = 1;
    }
    /* A control code is not a character: Qt reports none for Ctrl+A, GDK
     * unicodes it to 0x01, and either must reach ECS CTRL+A's letter half
     * rather than nothing. */
    if (intvsession_ecs_key_from_char('a', 0x01) != INTVSESSION_ECS_KEY_A) {
        fprintf(stderr, "keymap_test: FAILED: Ctrl+A control code\n");
        failed = 1;
    }
    /* A printable character the ECS has no key for stops there rather than
     * falling back to the physical key (see '!' above). */
    if (intvsession_ecs_key_from_char('1', '!') != INTVSESSION_ECS_KEY_NONE) {
        fprintf(stderr, "keymap_test: FAILED: Shift+1 must type nothing\n");
        failed = 1;
    }

    if (failed) {
        fprintf(stderr, "keymap_test: FAILED\n");
        return 1;
    }
    printf("keymap_test: OK\n");
    return 0;
}
