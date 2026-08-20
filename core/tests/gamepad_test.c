/*
 * gamepad_test -- exercises the pure functions (intv_disc_from_stick,
 * intv_pad_for_port) deterministically, and confirms the SDL gamepad
 * subsystem starts and stops cleanly with no hardware attached (this
 * machine has none, which is itself the common case in CI).
 */
#include <math.h>
#include <stdio.h>

#include "gamepad_sdl.h"
#include "intvsession.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "gamepad_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    /* ---- intv_disc_from_stick ---- */
    check("centered", intv_disc_from_stick(0.0f, 0.0f, 0.35f) == -1);
    check("within deadzone",
          intv_disc_from_stick(0.1f, 0.1f, 0.35f) == -1);
    check("east", intv_disc_from_stick(1.0f, 0.0f, 0.35f) == 0);
    check("north", intv_disc_from_stick(0.0f, 1.0f, 0.35f) == 4);
    check("west", intv_disc_from_stick(-1.0f, 0.0f, 0.35f) == 8);
    check("south", intv_disc_from_stick(0.0f, -1.0f, 0.35f) == 12);
    check("northeast", intv_disc_from_stick(0.7071f, 0.7071f, 0.35f) == 2);
    check("southwest", intv_disc_from_stick(-0.7071f, -0.7071f, 0.35f) == 10);

    /* A stick pushed toward "west" rarely lands EXACTLY on the x-axis --
     * real analog sticks wobble a few degrees off-centre. 16-way rounding
     * used to let a ~15-degree wobble tip this into a half-step (SSW/WSW,
     * which OR-combine the west AND south bits -- see intv_host.h's
     * disc_codes), spuriously registering a south-ish reading for a
     * player who only pushed left. 8-way rounding gives "west" a full
     * +-22.5 degree catch, so this must still resolve to pure west (8),
     * not any south-flavoured half-step. */
    check("west, 15 degrees off-axis toward south still reads pure west",
          intv_disc_from_stick(-0.9659f, -0.2588f, 0.35f) == 8);
    check("west, 15 degrees off-axis toward north still reads pure west",
          intv_disc_from_stick(-0.9659f, 0.2588f, 0.35f) == 8);

    /* No angle can produce an odd (half-step) code at all -- those are
     * unreachable from the keyboard too (intv_keymap.c's DIR_* enum only
     * uses even indices). The keypad window's clickable disc DOES reach
     * all 16 (intvsession_disc_from_point, disc_test.c): a pointer is
     * exactly where its owner put it, whereas a stick has to survive the
     * wobble reasoned about above. */
    {
        int all_even = 1;
        for (int deg = 0; deg < 360; deg += 3) {
            float rad = (float)deg * 3.14159265f / 180.0f;
            int d = intv_disc_from_stick(cosf(rad), sinf(rad), 0.35f);
            if (d != -1 && (d % 2) != 0)
                all_even = 0;
        }
        check("every angle resolves to an even (non-half-step) code",
              all_even);
    }

    /* ---- intv_disc_from_dpad ---- */
    check("dpad centered", intv_disc_from_dpad(0, 0, 0, 0) == -1);
    check("dpad up", intv_disc_from_dpad(1, 0, 0, 0) == 4);
    check("dpad down", intv_disc_from_dpad(0, 1, 0, 0) == 12);
    check("dpad left", intv_disc_from_dpad(0, 0, 1, 0) == 8);
    check("dpad right", intv_disc_from_dpad(0, 0, 0, 1) == 0);
    check("dpad up+right", intv_disc_from_dpad(1, 0, 0, 1) == 2);
    check("dpad up+left", intv_disc_from_dpad(1, 0, 1, 0) == 6);
    check("dpad down+left", intv_disc_from_dpad(0, 1, 1, 0) == 10);
    check("dpad down+right", intv_disc_from_dpad(0, 1, 0, 1) == 14);
    check("dpad up+down cancels to centered",
          intv_disc_from_dpad(1, 1, 0, 0) == -1);
    check("dpad left+right cancels to centered",
          intv_disc_from_dpad(0, 0, 1, 1) == -1);

    /* ---- intv_pad_for_port ---- */
    {
        /* Two pads, both automatic: first -> left(0), second -> right(1). */
        int bindings[2] = {-1, -1};
        check("auto: pad 0 drives left",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_LEFT) == 0);
        check("auto: pad 1 drives right",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_RIGHT) == 1);
    }
    {
        /* Explicit binding wins even out of connection order. */
        int bindings[2] = {INTVSESSION_PAD_RIGHT, -1};
        check("explicit binding: pad 0 drives right",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_RIGHT) == 0);
        check("remaining pad falls into the one free side",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_LEFT) == 1);
    }
    {
        int bindings[1] = {-1};
        check("no pad drives an unconnected side",
              intv_pad_for_port(bindings, 1, INTVSESSION_PAD_RIGHT) == -1);
    }
    {
        /* Four pads, all automatic: with ECS enabled, sides 2/3
         * (INTVSESSION_PAD_ECS_LEFT/_RIGHT) are driven by the 3rd/4th
         * connected pad, same automatic-assignment order as the base
         * pair -- this is the widening gamepad_sdl.c's NUM_SIDES == 4
         * depends on. */
        int bindings[4] = {-1, -1, -1, -1};
        check("auto: pad 2 drives ECS-left",
              intv_pad_for_port(bindings, 4, INTVSESSION_PAD_ECS_LEFT) == 2);
        check("auto: pad 3 drives ECS-right",
              intv_pad_for_port(bindings, 4, INTVSESSION_PAD_ECS_RIGHT) == 3);
    }
    {
        /* Only 2 pads connected: nothing drives the ECS pair yet, and the
         * base pair is unaffected by ECS sides existing at all. */
        int bindings[2] = {-1, -1};
        check("only 2 pads: nothing drives ECS-left",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_ECS_LEFT) == -1);
        check("only 2 pads: base pair still assigns normally",
              intv_pad_for_port(bindings, 2, INTVSESSION_PAD_LEFT) == 0);
    }

    /* ---- pad_button_from_sdl ----
     * Raw values below are SDL3's own SDL_GamepadButton numbering
     * (SDL_GAMEPAD_BUTTON_SOUTH=0, EAST=1, ... DPAD_RIGHT=14) -- written as
     * literals rather than pulling in <SDL3/SDL.h> here, so this test (like
     * the rest of this file) stays buildable without the SDL3 include path
     * intv_session only exposes privately (see core/CMakeLists.txt: SDL3 is
     * PRIVATE to intv_session, by design -- jzintv_core stays SDL-free).
     * pad_button_from_sdl is an explicit translation table for exactly this
     * reason: intvsession_pad_button's own numbering happens to match SDL's
     * today, but nothing here relies on that beyond this one function. */
    check("SDL SOUTH -> INTVSESSION_PAD_BTN_SOUTH",
          pad_button_from_sdl(0) == INTVSESSION_PAD_BTN_SOUTH);
    check("SDL EAST -> INTVSESSION_PAD_BTN_EAST",
          pad_button_from_sdl(1) == INTVSESSION_PAD_BTN_EAST);
    check("SDL WEST -> INTVSESSION_PAD_BTN_WEST",
          pad_button_from_sdl(2) == INTVSESSION_PAD_BTN_WEST);
    check("SDL NORTH -> INTVSESSION_PAD_BTN_NORTH",
          pad_button_from_sdl(3) == INTVSESSION_PAD_BTN_NORTH);
    check("SDL DPAD_UP -> INTVSESSION_PAD_BTN_DPAD_UP",
          pad_button_from_sdl(11) == INTVSESSION_PAD_BTN_DPAD_UP);
    check("SDL DPAD_RIGHT -> INTVSESSION_PAD_BTN_DPAD_RIGHT",
          pad_button_from_sdl(14) == INTVSESSION_PAD_BTN_DPAD_RIGHT);
    check("an SDL button this project doesn't name maps to NONE",
          pad_button_from_sdl(255) == INTVSESSION_PAD_BTN_NONE);

    /* ---- SDL subsystem lifecycle, no hardware required ---- */
    check("gamepad subsystem starts", intv_gamepad_start() == 0);
    check("no pads on a CI machine with none attached",
          intvsession_gamepad_count(NULL) == 0);
    intv_gamepad_stop();
    /* Starting and stopping again should be equally clean. */
    check("restart is clean", intv_gamepad_start() == 0);
    intv_gamepad_stop();

    if (failed) {
        fprintf(stderr, "gamepad_test: FAILED\n");
        return 1;
    }
    printf("gamepad_test: OK\n");
    return 0;
}
