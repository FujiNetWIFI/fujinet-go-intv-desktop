/*
 * bindings_test -- exercises core/src/bindings.c's remappable-bindings layer
 * through intvsession's public API: defaults derived from
 * intvsession_key_from_keysym, steal-and-describe semantics for both the
 * keyboard and gamepad slots, intvsession_key_from_keysym_bound's "moved
 * away" behaviour, settings persistence, and reset.
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
            intvsession_key_mapping m = bindings_key_from_button(
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
        check("keysym_name of a nonsense value is 0, dst untouched",
              intvsession_keysym_name(0x9999, name, sizeof(name)) == 0 &&
                  strcmp(name, "A") == 0);
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

    intvsession_free(s);

    if (failed) {
        fprintf(stderr, "bindings_test: FAILED\n");
        return 1;
    }
    printf("bindings_test: OK\n");
    return 0;
}
