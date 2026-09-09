/*
 * bindings_test -- exercises core/src/bindings.c's remappable-bindings layer
 * through intvsession's public API: defaults derived from
 * intvsession_key_from_keysym, steal-and-describe semantics for both the
 * keyboard and gamepad slots (keypad/action buttons AND disc segments alike),
 * intvsession_key_from_keysym_bound's "moved away" behaviour, the disc's
 * secondary-default rule, settings persistence, and reset.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bindings.h" /* private -- see this test's own CMakeLists entry */
#include "intvsession.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "bindings_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static intvsession *open_session(char *config_dir, char *data_dir)
{
    intvsession_paths paths = { .config_dir = config_dir, .data_dir = data_dir };
    return intvsession_new(&paths);
}

int main(void)
{
    char config_dir[1024], data_dir[1024];
    test_tmp_template(config_dir, sizeof(config_dir), "intv-bindings-test-cfg-");
    test_tmp_template(data_dir, sizeof(data_dir), "intv-bindings-test-data-");
    if (!mkdtemp(config_dir) || !mkdtemp(data_dir)) {
        perror("mkdtemp");
        return 1;
    }

    intvsession *s = open_session(config_dir, data_dir);
    check("intvsession_new", s != NULL);
    if (!s)
        return 1;

    /* ---- defaults are exactly what intvsession_key_from_keysym says ---- */
    {
        intvsession_key_mapping pure =
            intvsession_key_from_keysym(INTVSESSION_KEYSYM_KP_7);
        intvsession_binding b =
            intvsession_binding_get(s, pure.side, pure.key);
        check("default keyboard binding matches the pure table",
              pure.kind == INTVSESSION_MAP_KEY &&
                  b.keysym == INTVSESSION_KEYSYM_KP_7);
    }
    {
        /* RSHIFT's default target is Left/Top -- see intv_keymap.c. */
        intvsession_binding b = intvsession_binding_get(
            s, INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_TOP);
        check("default keyboard binding on Left/Top is RSHIFT",
              b.keysym == INTVSESSION_KEYSYM_RSHIFT);
        check("default gamepad binding on Left/Top is SOUTH",
              b.button == INTVSESSION_PAD_BTN_SOUTH);
    }
    check("a never-mapped digit has no default gamepad binding",
          intvsession_binding_get(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5)
                  .button == INTVSESSION_PAD_BTN_NONE);

    /* ---- the disc's 16 segments are now targets too --------------------- */
    {
        /* Left disc East's *displayed* default is the Right arrow, not 'K'
         * -- compute_defaults_locked walks special_keysyms (the arrows)
         * before the printable-ASCII sweep, so the arrow wins the
         * first-wins race onto the segment's one table slot. 'K' still
         * works (see the "secondary default" block further down) but is
         * not what a fresh install shows. */
        intvsession_binding east = intvsession_target_binding_get(
            s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 0));
        intvsession_binding north = intvsession_target_binding_get(
            s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 4));
        intvsession_binding right_east = intvsession_target_binding_get(
            s, intvsession_target_disc(INTVSESSION_PAD_RIGHT, 0));
        check("Left disc East's default is the Right arrow, not 'K'",
              east.keysym == INTVSESSION_KEYSYM_RIGHT);
        check("Left disc North's default is the Up arrow, not 'I'",
              north.keysym == INTVSESSION_KEYSYM_UP);
        check("Right disc East's default is 'D' (no arrow contender there)",
              right_east.keysym == 'D');
    }
    {
        /* Half-steps (odd clock positions) have no keyboard OR gamepad
         * default at all -- widening compute_defaults_locked's
         * PAD_BTN_NONE-seeding loop to the disc slots is what keeps this
         * INTVSESSION_PAD_BTN_NONE rather than a stray SOUTH (button 0),
         * which is what a memset-zeroed slot would otherwise read as. */
        intvsession_binding ene = intvsession_target_binding_get(
            s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 1));
        check("a half-step direction has no default keyboard binding",
              ene.keysym == 0);
        check("a half-step direction has no default gamepad binding",
              ene.button == INTVSESSION_PAD_BTN_NONE);
    }

    /* ---- bound keysym lookup matches the pure table before any remap ---- */
    {
        intvsession_key_mapping bound =
            intvsession_key_from_keysym_bound(s, INTVSESSION_KEYSYM_KP_7);
        intvsession_key_mapping pure =
            intvsession_key_from_keysym(INTVSESSION_KEYSYM_KP_7);
        check("bound lookup matches pure lookup pre-remap",
              bound.kind == pure.kind && bound.side == pure.side &&
                  bound.key == pure.key);
    }
    /* Disc keysyms pass through unchanged -- never remappable. */
    {
        intvsession_key_mapping m = intvsession_key_from_keysym_bound(s, 'K');
        check("disc keysym still resolves to MAP_DISC",
              m.kind == INTVSESSION_MAP_DISC && m.side == INTVSESSION_PAD_LEFT &&
                  m.direction == 0 /* DIR_E */);
    }
    check("a nonsense keysym is unbound",
          intvsession_key_from_keysym_bound(s, 0x9999).kind ==
              INTVSESSION_MAP_NONE);

    /* ---- remap steals the input from its old target, and says so ------- */
    {
        char stolen[128] = "unset";
        /* KP_7's default target is Left/1 (see intv_keymap.c) -- rebind it
         * onto Left/5 instead. */
        intvsession_binding_set_key(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5,
                                    INTVSESSION_KEYSYM_KP_7, stolen,
                                    sizeof(stolen));
        check("steal description names the old target",
              strstr(stolen, "Left") && strstr(stolen, "1"));
        check("KP_7 now drives Left/5",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_KEY_5)
                      .keysym == INTVSESSION_KEYSYM_KP_7);
        check("Left/1 no longer holds KP_7",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_KEY_1)
                      .keysym != INTVSESSION_KEYSYM_KP_7);
        /* KP_7's ORIGINAL default target (Left/1) must not still claim it
         * -- the whole point of intvsession_key_from_keysym_bound existing
         * separately from the pure function. */
        intvsession_key_mapping bound =
            intvsession_key_from_keysym_bound(s, INTVSESSION_KEYSYM_KP_7);
        check("bound lookup now finds KP_7 at its new target",
              bound.kind == INTVSESSION_MAP_KEY &&
                  bound.side == INTVSESSION_PAD_LEFT &&
                  bound.key == INTVSESSION_KEY_5);
    }

    /* ---- stealing a disc keysym frees it from the disc ------------------ */
    {
        char stolen[128] = "unset";
        /* 'K' is Left disc East by default (see intv_keymap.c). */
        intvsession_binding_set_key(s, INTVSESSION_PAD_RIGHT,
                                    INTVSESSION_KEY_ENTER, 'K', stolen,
                                    sizeof(stolen));
        check("stealing a disc key names the disc direction",
              strstr(stolen, "disc") && strstr(stolen, "East"));
        check("'K' no longer resolves to the disc",
              intvsession_key_from_keysym_bound(s, 'K').kind ==
                  INTVSESSION_MAP_KEY);
    }

    /* ---- re-binding an input to the slot it already occupies is a no-op
     * steal (empty description, not "stolen from itself") ---------------- */
    {
        char stolen[128] = "unset";
        intvsession_binding_set_key(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5,
                                    INTVSESSION_KEYSYM_KP_7, stolen,
                                    sizeof(stolen));
        check("re-binding to the same slot reports nothing stolen",
              stolen[0] == '\0');
    }

    /* ---- gamepad steal is scoped per side, not global ------------------- */
    {
        char stolen[128] = "unset";
        /* SOUTH already drives Left/Top by default; also give it Left/5 --
         * that should steal it from Left/Top. */
        intvsession_binding_set_button(s, INTVSESSION_PAD_LEFT,
                                       INTVSESSION_KEY_5,
                                       INTVSESSION_PAD_BTN_SOUTH, stolen,
                                       sizeof(stolen));
        check("gamepad steal description names the old key",
              strstr(stolen, "Left") && strstr(stolen, "Top"));
        check("Left/Top lost its SOUTH binding",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_ACTION_TOP)
                      .button == INTVSESSION_PAD_BTN_NONE);

        /* Right's own SOUTH default is untouched -- same button index, a
         * different side, no cross-side steal. */
        check("Right/Top's default SOUTH binding survives a Left-side steal",
              intvsession_binding_get(s, INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_ACTION_TOP)
                      .button == INTVSESSION_PAD_BTN_SOUTH);

        /* gamepad_sdl.c's own reverse-lookup entry points (bindings.h,
         * private) should agree with the forward table above -- this is
         * exactly what handle_button and poll_sticks' D-pad gate call on
         * their own polling thread. */
        {
            intvsession_key_mapping m = bindings_target_from_button(
                INTVSESSION_PAD_LEFT, INTVSESSION_PAD_BTN_SOUTH);
            check("reverse lookup: Left SOUTH now resolves to Left/5",
                  m.kind == INTVSESSION_MAP_KEY &&
                      m.side == INTVSESSION_PAD_LEFT &&
                      m.key == INTVSESSION_KEY_5);
        }
        check("reverse lookup: SOUTH is still claimed on Left (moved to "
              "Left/5, not unbound)",
              bindings_button_is_free(INTVSESSION_PAD_LEFT,
                                      INTVSESSION_PAD_BTN_SOUTH) == 0);
        check("D-pad-up is free on a side with no D-pad binding at all",
              bindings_button_is_free(INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_PAD_BTN_DPAD_UP) == 1);

        /* Bind D-pad Up to a keypad digit: gamepad_sdl.c's poll_sticks
         * must then treat that side's D-pad-up as claimed, not free, so it
         * stops also nudging the disc. */
        intvsession_binding_set_button(s, INTVSESSION_PAD_RIGHT,
                                       INTVSESSION_KEY_9,
                                       INTVSESSION_PAD_BTN_DPAD_UP, NULL, 0);
        check("D-pad-up is no longer free once bound to a keypad digit",
              bindings_button_is_free(INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_PAD_BTN_DPAD_UP) == 0);
        check("...and the other three D-pad directions on that side stay "
              "free",
              bindings_button_is_free(INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_PAD_BTN_DPAD_DOWN) == 1);
    }

    /* ---- separate slots: binding a key doesn't disturb the other slot --- */
    {
        intvsession_binding b = intvsession_binding_get(
            s, INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_ENTER);
        check("Right/Enter's keyboard slot ('K', bound above) survived "
              "with no gamepad binding touched",
              b.keysym == 'K' && b.button == INTVSESSION_PAD_BTN_NONE);
    }

    /* ---- disc segments are map targets too ------------------------------ */
    {
        char stolen[128] = "unset";
        /* Direction 3 is NNE, a half-step with no default at all -- binding
         * 'Q' onto it steals nothing. */
        intvsession_target_set_key(s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 3),
                                   'Q', stolen, sizeof(stolen));
        check("binding a half-step steals nothing", stolen[0] == '\0');
        check("half-step now resolves 'Q' as MAP_DISC direction 3",
              intvsession_key_from_keysym_bound(s, 'Q').kind ==
                  INTVSESSION_MAP_DISC &&
              intvsession_key_from_keysym_bound(s, 'Q').direction == 3);
        check("half-step's slot reads back 'Q'",
              intvsession_target_binding_get(
                  s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 3))
                      .keysym == 'Q');
    }
    {
        char stolen[128] = "unset";
        /* Re-bind 'Q' onto the SAME disc segment it already occupies --
         * "nothing stolen", the disc-target analogue of the existing
         * same-slot check for keypad targets above. */
        intvsession_target_set_key(s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 3),
                                   'Q', stolen, sizeof(stolen));
        check("re-binding a disc target to itself reports nothing stolen",
              stolen[0] == '\0');
    }
    {
        char stolen[128] = "unset";
        /* '3' still drives its own untouched default, Right/3 -- move it
         * onto a disc segment instead; Right/3 should read back as
         * stolen, and lose the keysym that used to drive it. Picked
         * because nothing else in this file touches Right/3 or '3'. */
        intvsession_target_set_key(s, intvsession_target_disc(INTVSESSION_PAD_RIGHT, 7),
                                   '3', stolen, sizeof(stolen));
        check("stealing a keypad key onto a disc target names the old key",
              strstr(stolen, "Right") && strstr(stolen, "3") &&
                  !strstr(stolen, "disc"));
        check("Right/3's keyboard slot lost '3'",
              intvsession_binding_get(s, INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_3)
                      .keysym != (uint32_t)'3');
    }
    {
        /* A gamepad button can drive a disc segment too -- bind D-pad Left
         * on the right side onto a half-step direction. */
        char stolen[128] = "unset";
        intvsession_pad_button btns[INTVSESSION_DISC_POSITIONS];

        intvsession_target_set_button(
            s, intvsession_target_disc(INTVSESSION_PAD_RIGHT, 6),
            INTVSESSION_PAD_BTN_DPAD_LEFT, stolen, sizeof(stolen));
        check("binding D-pad Left onto a disc segment steals nothing",
              stolen[0] == '\0');
        check("D-pad Left is no longer free on the right side",
              bindings_button_is_free(INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_PAD_BTN_DPAD_LEFT) == 0);
        {
            intvsession_key_mapping m = bindings_target_from_button(
                INTVSESSION_PAD_RIGHT, INTVSESSION_PAD_BTN_DPAD_LEFT);
            check("reverse lookup finds the disc segment, not a keypad key",
                  m.kind == INTVSESSION_MAP_DISC && m.direction == 6);
        }
        check("bindings_disc_buttons reports exactly one bound position",
              bindings_disc_buttons(INTVSESSION_PAD_RIGHT, btns) == 1 &&
                  btns[6] == INTVSESSION_PAD_BTN_DPAD_LEFT &&
                  btns[0] == INTVSESSION_PAD_BTN_NONE);
        check("a side with no disc-button bindings reports zero",
              bindings_disc_buttons(INTVSESSION_PAD_ECS_LEFT, btns) == 0);
    }

    /* ---- persistence: a second session on the same config dir sees it -- */
    intvsession_free(s);
    s = open_session(config_dir, data_dir);
    check("re-open", s != NULL);
    if (s) {
        check("remap persisted across reopen",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_KEY_5)
                      .keysym == INTVSESSION_KEYSYM_KP_7);
        check("gamepad remap persisted across reopen",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_KEY_5)
                      .button == INTVSESSION_PAD_BTN_SOUTH);
    }

    /* ---- reset restores every default -------------------------------- */
    intvsession_bindings_reset(s);
    check("reset restores Left/1's default KP_7",
          intvsession_binding_get(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1)
                  .keysym == INTVSESSION_KEYSYM_KP_7);
    /* Left/5's own default is KP_5 (see intv_keymap.c) -- reset should
     * restore THAT, not leave it unbound, undoing both the KP_7 steal onto
     * it and the SOUTH steal onto it from earlier. */
    check("reset restores Left/5's own default KP_5, undoing the steals "
          "onto it",
          intvsession_binding_get(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5)
                  .keysym == INTVSESSION_KEYSYM_KP_5);
    check("reset restores Left/Top's default SOUTH",
          intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                  INTVSESSION_ACTION_TOP)
                  .button == INTVSESSION_PAD_BTN_SOUTH);
    check("'K' resolves to the disc again after reset",
          intvsession_key_from_keysym_bound(s, 'K').kind ==
              INTVSESSION_MAP_DISC);

    /* Reset persists too. */
    intvsession_free(s);
    s = open_session(config_dir, data_dir);
    check("re-open after reset", s != NULL);
    if (s)
        check("reset survived reopen",
              intvsession_binding_get(s, INTVSESSION_PAD_LEFT,
                                      INTVSESSION_KEY_1)
                      .keysym == INTVSESSION_KEYSYM_KP_7);

    /* ---- secondary defaults: 'K' survives losing the first-wins race ----
     * From here on the table is back at defaults (the reopen above proved
     * it), which is exactly the state this rule needs to be exercised from. */
    {
        char stolen[128] = "unset";
        /* Steal the Right arrow -- Left disc East's *displayed* default --
         * onto Right/9. Left disc East's own slot is now empty. */
        intvsession_binding_set_key(s, INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_9,
                                    INTVSESSION_KEYSYM_RIGHT, stolen,
                                    sizeof(stolen));
        check("stealing the Right arrow names Left disc East",
              strstr(stolen, "disc") && strstr(stolen, "East"));
        check("Left disc East's slot is now empty",
              intvsession_target_binding_get(
                  s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 0))
                      .keysym == 0);
        check("'K' still resolves to Left disc East -- the slot being "
              "emptied by a steal is not a deliberate remap of the segment",
              intvsession_key_from_keysym_bound(s, 'K').kind ==
                  INTVSESSION_MAP_DISC &&
              intvsession_key_from_keysym_bound(s, 'K').direction == 0);
    }
    {
        /* NOW deliberately remap the segment itself, away from 'K' -- this
         * is the case that should finally silence the secondary default. */
        intvsession_target_set_key(s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 0),
                                   'G', NULL, 0);
        check("'K' no longer resolves to anything once its segment is "
              "explicitly remapped",
              intvsession_key_from_keysym_bound(s, 'K').kind ==
                  INTVSESSION_MAP_NONE);
        check("'G' now drives Left disc East",
              intvsession_key_from_keysym_bound(s, 'G').kind ==
                  INTVSESSION_MAP_DISC &&
              intvsession_key_from_keysym_bound(s, 'G').direction == 0);
    }
    {
        /* Reset brings back both the Right arrow AND 'K' at once -- there
         * was never a real second binding to restore, just the one slot
         * and the fallback rule that reads it. */
        intvsession_bindings_reset(s);
        check("reset restores 'K' as Left disc East's secondary default",
              intvsession_key_from_keysym_bound(s, 'K').kind ==
                  INTVSESSION_MAP_DISC);
        check("reset restores the Right arrow as Left disc East's slot",
              intvsession_target_binding_get(
                  s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 0))
                      .keysym == INTVSESSION_KEYSYM_RIGHT);
    }
    {
        /* Regression guard for intvsession_target_set_key's conditional
         * clear: stealing 'K' (a secondary default, resolved only via the
         * pure-table fallback) onto some other target must NOT blow away
         * the Right arrow actually sitting in Left disc East's slot --
         * only a slot that genuinely holds the stolen keysym gets cleared. */
        char stolen[128] = "unset";
        intvsession_binding_set_key(s, INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_6,
                                    'K', stolen, sizeof(stolen));
        check("stealing 'K' still names Left disc East",
              strstr(stolen, "disc") && strstr(stolen, "East"));
        check("...but leaves the Right arrow sitting in Left disc East's "
              "slot untouched",
              intvsession_target_binding_get(
                  s, intvsession_target_disc(INTVSESSION_PAD_LEFT, 0))
                      .keysym == INTVSESSION_KEYSYM_RIGHT);
        check("...so the Right arrow still drives Left disc East",
              intvsession_key_from_keysym_bound(s, INTVSESSION_KEYSYM_RIGHT)
                      .kind == INTVSESSION_MAP_DISC);
        intvsession_bindings_reset(s); /* leave a clean table behind */
    }

    /* ---- system actions (Reset Game / Reset to CONFIG) -------------------
     * From here on the table is back at defaults (the reset two blocks up
     * left it clean) -- exactly the state these need to be exercised from. */
    {
        intvsession_binding backspace = intvsession_target_binding_get(
            s, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME));
        intvsession_binding escape = intvsession_target_binding_get(
            s, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_CONFIG));
        check("Reset Game defaults to Backspace, no gamepad default",
              backspace.keysym == INTVSESSION_KEYSYM_BACKSPACE &&
                  backspace.button == INTVSESSION_PAD_BTN_NONE);
        check("Reset to CONFIG defaults to Escape, no gamepad default",
              escape.keysym == INTVSESSION_KEYSYM_ESCAPE &&
                  escape.button == INTVSESSION_PAD_BTN_NONE);

        {
            intvsession_key_mapping m = intvsession_key_from_keysym_bound(
                s, INTVSESSION_KEYSYM_BACKSPACE);
            check("Backspace resolves to MAP_SYSACT Reset Game",
                  m.kind == INTVSESSION_MAP_SYSACT &&
                      m.sysact == INTVSESSION_SYSACT_RESET_GAME);
        }
        {
            intvsession_key_mapping m = intvsession_key_from_keysym_bound(
                s, INTVSESSION_KEYSYM_ESCAPE);
            check("Escape resolves to MAP_SYSACT Reset to CONFIG",
                  m.kind == INTVSESSION_MAP_SYSACT &&
                      m.sysact == INTVSESSION_SYSACT_RESET_CONFIG);
        }
    }
    {
        char name[64];
        int n = intvsession_target_name(
            intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME), name,
            sizeof(name));
        check("target_name(Reset Game) has no side prefix",
              n == (int)strlen(name) && strcmp(name, "Reset Game") == 0);
        n = intvsession_target_name(
            intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_CONFIG),
            name, sizeof(name));
        check("target_name(Reset to CONFIG) has no side prefix",
              strcmp(name, "Reset to CONFIG") == 0);
        check("sysaction_name(RESET_GAME) matches",
              strcmp(intvsession_sysaction_name(INTVSESSION_SYSACT_RESET_GAME),
                    "Reset Game") == 0);
        check("sysaction_name out of range is \"?\"",
              strcmp(intvsession_sysaction_name(
                        (intvsession_sysaction)INTVSESSION_SYSACT_COUNT),
                    "?") == 0);
    }
    {
        /* Steal Backspace off Reset Game onto Left/5 -- Reset Game should
         * read back as stolen, and Backspace should stop resolving to any
         * sysaction. */
        char stolen[128] = "unset";
        intvsession_binding_set_key(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5,
                                    INTVSESSION_KEYSYM_BACKSPACE, stolen,
                                    sizeof(stolen));
        check("stealing Backspace names Reset Game",
              strcmp(stolen, "Reset Game") == 0);
        check("Backspace no longer resolves to a sysaction",
              intvsession_key_from_keysym_bound(s, INTVSESSION_KEYSYM_BACKSPACE)
                      .kind != INTVSESSION_MAP_SYSACT);
        check("Reset Game's slot is now empty",
              intvsession_target_binding_get(
                  s, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME))
                      .keysym == 0);
    }
    {
        /* Now bind Reset Game onto 'F' -- unbound by any default (see
         * intv_keymap.c: not one of the IJKM/DRWEASZXC disc letters), so
         * this steals nothing. */
        char stolen[128] = "unset";
        intvsession_target_set_key(
            s, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME),
            'F', stolen, sizeof(stolen));
        check("binding Reset Game onto an unclaimed key steals nothing",
              stolen[0] == '\0');
        check("Reset Game now drives on 'F'",
              intvsession_key_from_keysym_bound(s, 'F').kind ==
                  INTVSESSION_MAP_SYSACT &&
              intvsession_key_from_keysym_bound(s, 'F').sysact ==
                  INTVSESSION_SYSACT_RESET_GAME);
    }
    {
        /* Gamepad steal for a sysaction crosses every side -- unlike a
         * keypad/disc target, whose steal is scoped to one side (see the
         * "gamepad steal is scoped per side" block above). SOUTH already
         * drives every side's own Top action button by default
         * (compute_defaults_locked seeds it on all four) -- binding it to
         * Reset Game must clear it from EVERY side's own action-button
         * slot, or e.g. Right would still fire its own Top action on the
         * same physical press, alongside the sysaction. */
        char stolen[128] = "unset";
        intvsession_target_set_button(
            s, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME),
            INTVSESSION_PAD_BTN_SOUTH, stolen, sizeof(stolen));

        /* bindings_button_is_free is correctly 0 (NOT free) on every side
         * now, not just LEFT: bindings_target_from_button's cross-side
         * fallback (see its own comment) makes SOUTH resolve to the
         * sysaction from ANY side's pad, which is the whole point --
         * pressing SOUTH fires Reset Game regardless of which side that
         * physical pad happens to be assigned to. */
        check("SOUTH reads not-free on every side once bound to a "
              "sysaction (it resolves there via the cross-side fallback)",
              bindings_button_is_free(INTVSESSION_PAD_LEFT,
                                      INTVSESSION_PAD_BTN_SOUTH) == 0 &&
              bindings_button_is_free(INTVSESSION_PAD_RIGHT,
                                      INTVSESSION_PAD_BTN_SOUTH) == 0 &&
              bindings_button_is_free(INTVSESSION_PAD_ECS_LEFT,
                                      INTVSESSION_PAD_BTN_SOUTH) == 0 &&
              bindings_button_is_free(INTVSESSION_PAD_ECS_RIGHT,
                                      INTVSESSION_PAD_BTN_SOUTH) == 0);
        /* And every side's reverse lookup resolves to the sysaction, not
         * its old per-side Top action -- the old claim is genuinely gone
         * (cleared), not just shadowed by the fallback. */
        {
            static const intvsession_pad_side sides[] = {
                INTVSESSION_PAD_LEFT, INTVSESSION_PAD_RIGHT,
                INTVSESSION_PAD_ECS_LEFT, INTVSESSION_PAD_ECS_RIGHT,
            };
            int i, ok = 1;
            for (i = 0; i < 4; i++) {
                intvsession_key_mapping m =
                    bindings_target_from_button(sides[i],
                                                INTVSESSION_PAD_BTN_SOUTH);
                if (m.kind != INTVSESSION_MAP_SYSACT ||
                    m.sysact != INTVSESSION_SYSACT_RESET_GAME)
                    ok = 0;
            }
            check("every side's reverse lookup finds the sysaction, not "
                  "its old Top action binding",
                  ok);
        }
    }
    intvsession_bindings_reset(s); /* leave a clean table behind */

    /* ---- slot-numbering regression guard ---------------------------------
     * System actions are appended AFTER the disc's 16 slots (SYSACT_SLOT),
     * so a legacy persisted string naming a disc slot by number must still
     * land on the same disc direction -- "appended, not inserted", see
     * bindings.c's own file header. Slot 15 is DISC_SLOT(0), Left disc
     * East. */
    {
        intvsession_set_str(s, "bindings", "0.15.k:70"); /* 70 = 'F' */
        intvsession_settings_flush(s);
        intvsession_free(s);
        s = open_session(config_dir, data_dir);
        check("re-open after a legacy disc-slot string", s != NULL);
        if (s) {
            check("slot 15 still lands on Left disc East, not a sysaction",
                  intvsession_key_from_keysym_bound(s, 'F').kind ==
                      INTVSESSION_MAP_DISC &&
                  intvsession_key_from_keysym_bound(s, 'F').direction == 0);
            intvsession_bindings_reset(s);
        }
    }

    /* ---- system-action latch (post/take) ---------------------------------
     * Pure cross-thread queue, no session state involved -- see
     * intvsession_sysaction_post's own comment on why gamepad_sdl.c's SDL
     * thread needs it instead of firing RESET_CONFIG inline. */
    {
        intvsession_sysaction a;
        check("take on an empty latch returns 0",
              intvsession_sysaction_take(s, &a) == 0);

        intvsession_sysaction_post(s, INTVSESSION_SYSACT_RESET_CONFIG);
        check("take returns the posted action",
              intvsession_sysaction_take(s, &a) == 1 &&
                  a == INTVSESSION_SYSACT_RESET_CONFIG);
        check("a second take on a single post returns 0",
              intvsession_sysaction_take(s, &a) == 0);

        /* RESET_GAME is 0 -- the bitmask, not a bare enum, is what keeps
         * this from being indistinguishable from "nothing pending" (see
         * session.c's own comment on s_sysact_pending). */
        intvsession_sysaction_post(s, INTVSESSION_SYSACT_RESET_GAME);
        intvsession_sysaction_post(s, INTVSESSION_SYSACT_RESET_CONFIG);
        {
            int got_game = 0, got_config = 0, n;
            for (n = 0; n < 2; n++) {
                check("take succeeds while the latch has pending actions",
                      intvsession_sysaction_take(s, &a) == 1);
                if (a == INTVSESSION_SYSACT_RESET_GAME) got_game = 1;
                if (a == INTVSESSION_SYSACT_RESET_CONFIG) got_config = 1;
            }
            check("both distinct posted actions were each taken once",
                  got_game && got_config);
        }
        check("take on a drained latch returns 0 again",
              intvsession_sysaction_take(s, &a) == 0);
    }

    /* ---- name tables ---------------------------------------------------- */
    check("pad_key_name(5) is \"5\"",
          strcmp(intvsession_pad_key_name(INTVSESSION_KEY_5), "5") == 0);
    check("pad_key_name(Top) is \"Top\"",
          strcmp(intvsession_pad_key_name(INTVSESSION_ACTION_TOP), "Top") == 0);
    check("pad_side_name(Left) is \"Left\"",
          strcmp(intvsession_pad_side_name(INTVSESSION_PAD_LEFT), "Left") == 0);
    check("pad_button_name(SOUTH) is \"A\"",
          strcmp(intvsession_pad_button_name(INTVSESSION_PAD_BTN_SOUTH), "A") ==
              0);
    {
        char name[64];
        int n = intvsession_keysym_name(INTVSESSION_KEYSYM_KP_7, name,
                                        sizeof(name));
        check("keysym_name(KP_7) is non-empty and matches its length",
              n > 0 && (int)strlen(name) == n);
        check("keysym_name('a') is \"A\" (uppercased)",
              intvsession_keysym_name('a', name, sizeof(name)) == 1 &&
                  strcmp(name, "A") == 0);
        /* The contract is now "dst is ALWAYS written" -- it used to be left
         * untouched for an unnamed keysym, and every caller in every
         * frontend then printed its uninitialised stack buffer. */
        check("keysym_name of a nonsense value is 0, dst emptied",
              intvsession_keysym_name(0x9999, name, sizeof(name)) == 0 &&
                  name[0] == '\0');
        check("keysym_name(0) is 0, dst emptied",
              intvsession_keysym_name(0, name, sizeof(name)) == 0 &&
                  name[0] == '\0');

        /* ---- fallback bands (intvsession.h) ------------------------------
         * The reason they exist: an Intellivision-to-USB adapter in keyboard
         * mode emits HID usages a normal keyboard has no cap for, and every
         * one of them used to translate to keysym 0 and be unmappable. */
        check("keysym_name names a HID usage (F16)",
              intvsession_keysym_name(INTVSESSION_KEYSYM_HID_BASE + 0x6B, name,
                                      sizeof(name)) > 0 &&
                  strcmp(name, "F16") == 0);
        check("keysym_name names a keypad-only HID usage (Keypad A)",
              intvsession_keysym_name(INTVSESSION_KEYSYM_HID_BASE + 0xBC, name,
                                      sizeof(name)) > 0 &&
                  strcmp(name, "Keypad A") == 0);
        check("keysym_name synthesizes a name for an unnamed HID usage",
              intvsession_keysym_name(INTVSESSION_KEYSYM_HID_BASE + 0xAF, name,
                                      sizeof(name)) > 0 &&
                  strcmp(name, "HID 0xAF") == 0);
        check("keysym_name synthesizes a name for the native band",
              intvsession_keysym_name(INTVSESSION_KEYSYM_NATIVE_BASE + 0xFF,
                                      name, sizeof(name)) > 0 &&
                  strcmp(name, "Key 0x00FF") == 0);
    }
    {
        /* Raw joystick bands: an adapter's 20th button and its hat have no
         * names of their own, but must still describe rather than print "?".
         * See intvsession_pad_button_name's contract on the buffer. */
        char buf[64];
        snprintf(buf, sizeof(buf), "%s",
                intvsession_pad_button_name(
                    (intvsession_pad_button)(INTVSESSION_PAD_BTN_RAW_BASE + 17)));
        check("pad_button_name(raw 17) is \"Button 17\"",
              strcmp(buf, "Button 17") == 0);
        snprintf(buf, sizeof(buf), "%s",
                intvsession_pad_button_name(
                    (intvsession_pad_button)(INTVSESSION_PAD_HAT_BASE + 8 + 1)));
        check("pad_button_name(hat 1 NE) is \"Hat 1 NE\"",
              strcmp(buf, "Hat 1 NE") == 0);
    }
    {
        char desc[128];
        intvsession_binding both = { 'A', INTVSESSION_PAD_BTN_SOUTH };
        intvsession_binding key_only = { 'A', INTVSESSION_PAD_BTN_NONE };
        intvsession_binding none = { 0, INTVSESSION_PAD_BTN_NONE };

        intvsession_binding_describe(both, desc, sizeof(desc));
        check("describe(both) mentions the key and the pad button",
              strstr(desc, "A") && strstr(desc, "Gamepad A"));
        intvsession_binding_describe(key_only, desc, sizeof(desc));
        check("describe(key only) is just \"A\"", strcmp(desc, "A") == 0);
        intvsession_binding_describe(none, desc, sizeof(desc));
        check("describe(none) is \"nothing\"",
              strcmp(desc, "nothing") == 0);
    }
    {
        /* All 16 clock positions, replacing the old 8-entry table that was
         * indexed by direction/2 -- direction 1 used to print "East"
         * (the same name as direction 0) instead of its own half-step
         * name. */
        int i, ok = 1;
        for (i = 0; i < INTVSESSION_DISC_POSITIONS; i++) {
            const char *n = intvsession_disc_dir_name(i);
            int j;
            if (!n[0] || strcmp(n, "?") == 0)
                ok = 0;
            for (j = 0; j < i; j++)
                if (strcmp(n, intvsession_disc_dir_name(j)) == 0)
                    ok = 0;
        }
        check("all 16 disc_dir_names are non-empty and pairwise distinct", ok);
        check("disc_dir_name(0) is \"East\"",
              strcmp(intvsession_disc_dir_name(0), "East") == 0);
        check("disc_dir_name(1) is \"ENE\", not \"East\" (the old /2 bug)",
              strcmp(intvsession_disc_dir_name(1), "ENE") == 0);
        check("disc_dir_name(2) is \"Northeast\" (matches the old table's "
              "value at its new index)",
              strcmp(intvsession_disc_dir_name(2), "Northeast") == 0);
        check("disc_dir_name(14) is \"Southeast\" (matches the old table's "
              "value at its new index)",
              strcmp(intvsession_disc_dir_name(14), "Southeast") == 0);
        check("disc_dir_name out of range is \"?\"",
              strcmp(intvsession_disc_dir_name(16), "?") == 0 &&
                  strcmp(intvsession_disc_dir_name(-1), "?") == 0);
    }
    {
        char name[64];
        int n;

        n = intvsession_target_name(
            intvsession_target_key(INTVSESSION_PAD_LEFT, INTVSESSION_KEY_5),
            name, sizeof(name));
        check("target_name(Left 5) is \"Left 5\"",
              n == (int)strlen(name) && strcmp(name, "Left 5") == 0);

        n = intvsession_target_name(
            intvsession_target_disc(INTVSESSION_PAD_LEFT, 0), name,
            sizeof(name));
        check("target_name(Left disc East) is \"Left disc East\"",
              n == (int)strlen(name) && strcmp(name, "Left disc East") == 0);

        n = intvsession_target_name(
            intvsession_target_disc(INTVSESSION_PAD_RIGHT, 1), name,
            sizeof(name));
        check("target_name of a half-step is \"Right disc ENE\"",
              strcmp(name, "Right disc ENE") == 0);

        name[0] = 'x';
        n = intvsession_target_name(
            (intvsession_key_mapping){ .kind = INTVSESSION_MAP_NONE }, name,
            sizeof(name));
        check("target_name(MAP_NONE) returns 0 and an empty string",
              n == 0 && name[0] == '\0');
    }

    /* ---- what an Intellivision-to-USB adapter actually needs ------------
     * The two fallback bands and the settings line length are one story, so
     * they are tested as one: map a whole two-controller adapter -- both
     * sides' 15 keypad/action buttons AND all 16 disc positions, each to a
     * keysym and a button only reachable through a band -- then reopen and
     * check that ALL of it came back.
     *
     * The volume matters as much as the values. This packs a "bindings="
     * line of a couple of kilobytes, and settings.c used to read lines with
     * a 1024-byte fgets buffer: the line was split, the continuation had no
     * '=' so the parser dropped it, and every mapping past the first ~1015
     * bytes vanished on the next launch. Mapping a full adapter is exactly
     * how a user would hit that, so the test has to map a full one -- a
     * smaller sample stays under the old limit and proves nothing. */
    {
        const intvsession_pad_side sides[2] = { INTVSESSION_PAD_LEFT,
                                                INTVSESSION_PAD_RIGHT };
        /* One target per (side, slot), flattened so the keysym and button
         * handed to each are trivially distinct -- a binding steals its
         * input from wherever it was, so a repeat would silently undo an
         * earlier one and the round-trip check below would be vacuous. */
        intvsession_key_mapping targets[2 *
            (INTVSESSION_KEY_COUNT + INTVSESSION_DISC_POSITIONS)];
        int n = 0, i, side_i, all_back = 1;

        for (side_i = 0; side_i < 2; side_i++) {
            for (i = 0; i < INTVSESSION_KEY_COUNT; i++)
                targets[n++] = intvsession_target_key(sides[side_i],
                                                      (intvsession_key)i);
            for (i = 0; i < INTVSESSION_DISC_POSITIONS; i++)
                targets[n++] = intvsession_target_disc(sides[side_i], i);
        }
        check("the adapter mapping covers every slot on both sides",
              n == 2 * (INTVSESSION_KEY_COUNT + INTVSESSION_DISC_POSITIONS));

        for (i = 0; i < n; i++) {
            /* HID usages from 0xA5 up run through the keypad-only keys
             * ("Keypad A", "Keypad Mem Store", ...) that no ordinary
             * keyboard can produce at all -- the class of key that used to
             * translate to 0 and be unmappable. */
            intvsession_target_set_key(
                s, targets[i],
                INTVSESSION_KEYSYM_HID_BASE + 0xA5 + (uint32_t)i, NULL, 0);
            intvsession_target_set_button(
                s, targets[i],
                (intvsession_pad_button)(INTVSESSION_PAD_BTN_RAW_BASE + i),
                NULL, 0);
        }

        check("a fallback-band keysym resolves to its target",
              intvsession_key_from_keysym_bound(
                  s, INTVSESSION_KEYSYM_HID_BASE + 0xA5).kind ==
                  INTVSESSION_MAP_KEY);
        check("a fallback-band keysym has no DEFAULT mapping",
              intvsession_key_from_keysym(
                  INTVSESSION_KEYSYM_HID_BASE + 0xA5).kind ==
                  INTVSESSION_MAP_NONE);
        check("a raw joystick button resolves to its target",
              bindings_target_from_button(
                  INTVSESSION_PAD_LEFT,
                  (intvsession_pad_button)INTVSESSION_PAD_BTN_RAW_BASE).kind ==
                  INTVSESSION_MAP_KEY);
        {
            /* A bound exotic input must describe as itself. Before the name
             * tables covered the bands, keysym_name returned 0 here and
             * describe() reported a fully-bound slot as "nothing" -- so
             * check the worst case, a usage with no name of its own, which
             * has to fall back to a synthesized one rather than nothing. */
            char desc[128];
            intvsession_binding_describe(
                intvsession_target_binding_get(s, targets[0]), desc,
                sizeof(desc));
            check("describe of an adapter binding names both halves, not "
                  "\"nothing\"",
                  strstr(desc, "HID 0xA5") && strstr(desc, "Button 0"));
        }

        intvsession_free(s);
        s = open_session(config_dir, data_dir);
        check("re-open after mapping a full adapter", s != NULL);
        for (i = 0; s && i < n; i++) {
            const intvsession_binding b =
                intvsession_target_binding_get(s, targets[i]);
            if (b.keysym != (uint32_t)(INTVSESSION_KEYSYM_HID_BASE + 0xA5 + i) ||
                b.button != (intvsession_pad_button)
                                (INTVSESSION_PAD_BTN_RAW_BASE + i))
                all_back = 0;
        }
        check("every one of a full adapter's bindings survived a reopen",
              all_back);
    }

    intvsession_free(s);

    if (failed) {
        fprintf(stderr, "bindings_test: FAILED\n");
        return 1;
    }
    printf("bindings_test: OK\n");
    return 0;
}
