/*
 * gamepad_map_test -- gamepads come and go: an SDL virtual pad is picked up as
 * the left controller when plugged in, drives the disc (D-pad, left stick as
 * an eight-way switch with hysteresis, diagonals) and the three action buttons
 * through the bindings table, is its own input source (a key or the window
 * letting go never drops its hold, and it never drops theirs), is released
 * when unplugged while held, gets its chosen side back when plugged in again
 * and on the next run (settings key pad_assign), a pad plugged in after a
 * restart is announced, assigning a side the machine lacks is Automatic, a
 * button bound to a disc segment moves the disc, and a trigger held past half
 * travel does not freeze the session. SKIPs (77) where the machine has no
 * ROMs, SDL has no virtual joysticks, or real pads are in the way. Waits up to
 * 10 s for each change (slow CI runners).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "intvsession.h"
#include "test_tmpdir.h"

#define PAD_NAME "intvtest virtual pad"
#define KEYBIT(key) (1u << (key))
#define WAIT_TRIES 500   /* x 20 ms = 10 s */

#define LEFT  INTVSESSION_PAD_LEFT
#define RIGHT INTVSESSION_PAD_RIGHT

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass)
        failures++;
}

static void sleep_ms(int msec)
{
    struct timespec ts = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Waits for a side's merged keys and disc to be as wanted. */
static void expect(intvsession *s, intvsession_pad_side side, unsigned want_keys,
                   int want_disc, const char *what)
{
    unsigned held = 0;
    int disc = -1, tries;
    for (tries = 0; tries < WAIT_TRIES; tries++) {
        held = intvsession_buttons_held(s, side);
        disc = intvsession_disc_held(s, side);
        if (held == want_keys && disc == want_disc) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (keys %04X want %04X, disc %d want %d)\n", what, held, want_keys,
           disc, want_disc);
    failures++;
}

/* The disc must STAY as it is for a while (something that must not happen). */
static void expect_still(intvsession *s, intvsession_pad_side side, int want_disc, const char *what)
{
    int tries, ok = 1;
    for (tries = 0; tries < 15; tries++) {
        if (intvsession_disc_held(s, side) != want_disc)
            ok = 0;
        sleep_ms(20);
    }
    check(ok, what);
}

static SDL_Joystick *plug(SDL_JoystickID *vid)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    *vid = SDL_AttachVirtualJoystick(&desc);
    return *vid ? SDL_OpenJoystick(*vid) : NULL;
}

static int find_pad(intvsession *s)
{
    int idx;
    for (idx = 0; idx < intvsession_gamepad_count(s); idx++) {
        char name[64];
        intvsession_gamepad_name(s, idx, name, sizeof name);
        if (!strcmp(name, PAD_NAME))
            return idx;
    }
    return -1;
}

static int wait_pad(intvsession *s)
{
    int tries, idx = -1;
    for (tries = 0; tries < WAIT_TRIES && (idx = find_pad(s)) < 0; tries++)
        sleep_ms(20);
    return idx;
}

static void axis(SDL_Joystick *vj, SDL_GamepadAxis which, int value)
{
    SDL_SetJoystickVirtualAxis(vj, which, (Sint16)value);
}

static void button(SDL_Joystick *vj, SDL_GamepadButton which, int down)
{
    SDL_SetJoystickVirtualButton(vj, which, down != 0);
}

int main(void)
{
    char cfg[512], data[512], ev[160];
    intvsession_paths paths;
    intvsession_start_opts opts;
    intvsession *s;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int tries, idx;
    unsigned gen;

    test_tmp_template(cfg, sizeof cfg, "intv-pad-cfg-");
    test_tmp_template(data, sizeof data, "intv-pad-data-");
    if (!mkdtemp(cfg) || !mkdtemp(data)) {
        perror("mkdtemp");
        return 1;
    }
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    s = intvsession_new(&paths);
    if (!s)
        return 1;
    if (!intvsession_has_system_roms(s)) {
        printf("SKIP: no system ROMs (build -DWITH_INTV_ROMS=ON)\n");
        intvsession_free(s);
        return 77;
    }
    intvsession_default_opts(s, &opts);
    opts.external_fujinet = 1;                 /* no FujiNet runtime of our own */
    opts.fujinet_addr = "127.0.0.1:65499";
    opts.enable_gamepad = 1;
    check(opts.ecs == INTVSESSION_HW_AUTO, "ECS is off by default");
    check(intvsession_start(s, &opts) == 0, "start");
    sleep_ms(700);
    if (intvsession_gamepad_count(s) > 0) {
        printf("SKIP: real pads attached\n");
        intvsession_free(s);
        return 77;
    }
    gen = intvsession_gamepad_generation(s);
    vj = plug(&vid);
    if (!vj) {
        printf("SKIP: no virtual joysticks: %s\n", SDL_GetError());
        intvsession_free(s);
        return 77;
    }
    idx = wait_pad(s);
    check(idx >= 0 && intvsession_gamepad_effective_side(s, idx) == 0, "plugged in: the left controller");
    check(intvsession_gamepad_generation(s) != gen, "generation moved");
    intvsession_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Connected") != NULL && strstr(ev, PAD_NAME) != NULL && strstr(ev, "Left") != NULL,
          "connect event names the pad and its side");

    /* defaults: South/East/West are the action buttons */
    button(vj, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    expect(s, LEFT, KEYBIT(INTVSESSION_ACTION_TOP), -1, "South is Top");
    button(vj, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    button(vj, SDL_GAMEPAD_BUTTON_EAST, 1);
    expect(s, LEFT, KEYBIT(INTVSESSION_ACTION_LOWER_RIGHT), -1, "East is the lower right");
    button(vj, SDL_GAMEPAD_BUTTON_EAST, 0);
    button(vj, SDL_GAMEPAD_BUTTON_WEST, 1);
    expect(s, LEFT, KEYBIT(INTVSESSION_ACTION_LOWER_LEFT), -1, "West is the lower left");
    button(vj, SDL_GAMEPAD_BUTTON_WEST, 0);
    expect(s, LEFT, 0, -1, "released");

    /* the D-pad: eight directions, diagonals included */
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    expect(s, LEFT, 0, 4, "D-pad up is North");
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1);
    expect(s, LEFT, 0, 2, "D-pad up-right is the Northeast diagonal");
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0);
    expect(s, LEFT, 0, -1, "D-pad released");

    /* the left stick: 8-way with hysteresis (enter 0.35, exit 0.15) */
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 8000);    /* 0.24 */
    expect_still(s, LEFT, -1, "a stick under 0.35 does not engage");
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    expect(s, LEFT, 0, 0, "the stick right is East");
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 8000);    /* 0.24: past the exit radius, still pushed */
    expect_still(s, LEFT, 0, "and stays engaged above 0.15");
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 2000);    /* 0.06 */
    expect(s, LEFT, 0, -1, "and lets go below it");
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    axis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    expect(s, LEFT, 0, 2, "the stick up-right is Northeast");
    axis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    axis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, LEFT, 0, -1, "the stick centred");

    /* sources: the pad, a key and the window are independent */
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1);
    expect(s, LEFT, 0, 8, "the pad holds West");
    intvsession_pad_disc(s, LEFT, 12);
    expect(s, LEFT, 0, 8, "a key's South does not beat the pad's West");
    intvsession_pad_disc(s, LEFT, -1);
    expect(s, LEFT, 0, 8, "a key letting go leaves the pad's West");
    intvsession_pointer_set(s, LEFT, -1, 4);
    expect(s, LEFT, 0, 4, "the window's mouse beats the pad");
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0);
    expect(s, LEFT, 0, 4, "the pad letting go leaves the window's North");
    button(vj, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    intvsession_pointer_set(s, LEFT, INTVSESSION_ACTION_TOP, -1);
    intvsession_pointer_set(s, LEFT, -1, -1);
    expect(s, LEFT, KEYBIT(INTVSESSION_ACTION_TOP), -1, "the window letting go leaves the pad's Top");
    intvsession_pad_key(s, LEFT, INTVSESSION_KEY_7, 1);
    intvsession_release_all(s);
    expect(s, LEFT, KEYBIT(INTVSESSION_ACTION_TOP), -1, "release_all drops the key and keeps the pad's hold");
    button(vj, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    expect(s, LEFT, 0, -1, "the pad's button is gone");

    /* a button bound to a disc segment moves the disc (the only way to a half-step) */
    intvsession_target_set_button(s, intvsession_target_disc(LEFT, 3), INTVSESSION_PAD_BTN_NORTH, NULL, 0);
    button(vj, SDL_GAMEPAD_BUTTON_NORTH, 1);
    expect(s, LEFT, 0, 3, "North bound to the NNE half-step moves the disc there");
    button(vj, SDL_GAMEPAD_BUTTON_NORTH, 0);
    expect(s, LEFT, 0, -1, "and releases it");
    intvsession_bindings_reset(s);

    /* a D-pad direction bound to a keypad key stops moving the disc */
    intvsession_target_set_button(s, intvsession_target_key(LEFT, INTVSESSION_KEY_5),
                                  INTVSESSION_PAD_BTN_DPAD_UP, NULL, 0);
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    expect(s, LEFT, KEYBIT(INTVSESSION_KEY_5), -1, "D-pad up bound to 5 presses 5 and not North");
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
    expect(s, LEFT, 0, -1, "released");
    intvsession_bindings_reset(s);

    /* A trigger past half travel once deadlocked the gamepad thread on its own
     * lock; the Controllers window's timers froze with it. A hang here is a
     * ctest timeout. */
    intvsession_target_set_button(s, intvsession_target_key(LEFT, INTVSESSION_KEY_3),
                                  INTVSESSION_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    axis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, LEFT, KEYBIT(INTVSESSION_KEY_3), -1, "the left trigger presses its binding");
    (void)intvsession_gamepad_generation(s);
    (void)intvsession_gamepad_effective_side(s, idx);
    printf("ok: the session is not frozen with a trigger held\n");
    axis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, LEFT, 0, -1, "the left trigger released");
    intvsession_gamepad_capture_begin(s);
    axis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        intvsession_pad_button btn = INTVSESSION_PAD_BTN_NONE;
        int got = 0;
        for (tries = 0; tries < WAIT_TRIES && !got; tries++) {
            got = intvsession_gamepad_capture_poll(s, &btn);
            if (!got)
                sleep_ms(20);
        }
        check(got && btn == INTVSESSION_PAD_BTN_RIGHT_TRIGGER, "map mode captures a trigger");
    }
    axis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    intvsession_bindings_reset(s);

    /* an unavailable side is Automatic; a real one is remembered */
    intvsession_gamepad_assign(s, idx, 2);
    check(intvsession_gamepad_assignment(s, idx) == -1, "the ECS sides do not exist with the ECS off");
    intvsession_gamepad_assign(s, idx, 7);
    check(intvsession_gamepad_assignment(s, idx) == -1, "side 7 does not exist");
    intvsession_gamepad_assign(s, idx, 1);
    check(strstr(intvsession_get_str(s, "pad_assign", ""), PAD_NAME) != NULL &&
          strstr(intvsession_get_str(s, "pad_assign", ""), "=1") != NULL, "the assignment is saved in pad_assign");
    button(vj, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 1);
    expect(s, RIGHT, 0, 12, "assigned to the right controller");
    expect(s, LEFT, 0, -1, "and the left one is free");
    button(vj, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    expect(s, RIGHT, KEYBIT(INTVSESSION_ACTION_TOP), 12, "the right controller's pad holds South and Top");
    intvsession_pad_key(s, LEFT, INTVSESSION_KEY_9, 1);
    expect(s, LEFT, KEYBIT(INTVSESSION_KEY_9), -1, "the keyboard holds the left controller's 9");
    gen = intvsession_gamepad_generation(s);
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    expect(s, RIGHT, 0, -1, "unplugged while held: released");
    expect(s, LEFT, KEYBIT(INTVSESSION_KEY_9), -1, "while the keyboard's hold stays");
    intvsession_pad_key(s, LEFT, INTVSESSION_KEY_9, 0);
    expect(s, LEFT, 0, -1, "and lets go itself");
    for (tries = 0; tries < WAIT_TRIES && intvsession_gamepad_generation(s) == gen; tries++)
        sleep_ms(20);
    intvsession_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Disconnected") != NULL && find_pad(s) < 0, "unplug event");

    /* plugged in again: the right controller again */
    vj = plug(&vid);
    idx = wait_pad(s);
    check(idx >= 0 && intvsession_gamepad_effective_side(s, idx) == 1, "replugged: the right controller again");
    check(intvsession_gamepad_assignment(s, idx) == 1, "as an explicit assignment");

    /* and on the next run, from the saved setting (the virtual joystick does
     * not survive SDL's joystick subsystem restarting: plug it in again after) */
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    intvsession_stop(s);
    check(intvsession_start(s, &opts) == 0, "restart");
    vj = plug(&vid);
    check(vj != NULL, "plug after restart");
    idx = wait_pad(s);
    check(idx >= 0 && intvsession_gamepad_effective_side(s, idx) == 1, "a new run: the right controller again");
    intvsession_gamepad_last_event(s, ev, sizeof ev);
    check(strstr(ev, "Connected") != NULL, "a pad plugged in after a restart is announced");

    /* Automatic forgets it */
    intvsession_gamepad_assign(s, idx, -1);
    check(intvsession_gamepad_effective_side(s, idx) == 0 && intvsession_gamepad_assignment(s, idx) == -1,
          "Automatic: the left controller");
    check(strstr(intvsession_get_str(s, "pad_assign", ""), PAD_NAME) == NULL, "and the setting is cleared");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);

    /* enable_gamepad = 0: the thread never starts, so nothing is picked up */
    intvsession_stop(s);
    opts.enable_gamepad = 0;
    check(intvsession_start(s, &opts) == 0, "start without gamepads");
    vj = plug(&vid);
    sleep_ms(1200);
    check(intvsession_gamepad_count(s) == 0, "enable_gamepad 0: no pad is opened");
    if (vj)
        SDL_CloseJoystick(vj);
    if (vid)
        SDL_DetachVirtualJoystick(vid);

    /* ECS on: sides 2 and 3 exist (needs ecs.bin) */
    intvsession_stop(s);
    if (intvsession_has_ecs_rom(s)) {
        opts.enable_gamepad = 1;
        opts.ecs = INTVSESSION_HW_ON;
        check(intvsession_start(s, &opts) == 0, "start with the ECS");
        check(intvsession_ecs_on(s) == 1, "ecs_on reports it");
        vj = plug(&vid);
        idx = wait_pad(s);
        intvsession_gamepad_assign(s, idx, 3);
        check(idx >= 0 && intvsession_gamepad_assignment(s, idx) == 3 &&
              intvsession_gamepad_effective_side(s, idx) == 3, "with the ECS, side 3 can be assigned");
        button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
        expect(s, INTVSESSION_PAD_ECS_RIGHT, 0, 4, "the pad drives the ECS right controller");
        button(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
        intvsession_gamepad_assign(s, idx, -1);
        SDL_CloseJoystick(vj);
        SDL_DetachVirtualJoystick(vid);
        intvsession_stop(s);
    } else {
        printf("note: no ecs.bin -- the ECS sides were not run\n");
    }

    intvsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
