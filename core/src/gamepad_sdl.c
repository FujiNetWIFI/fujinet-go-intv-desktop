/*
 * intvsession gamepad backend: SDL3 for enumeration/input only. Runs a
 * background thread that opens/closes SDL gamepads as they connect, and
 * translates their state into the same intv_host_pad_key/intv_host_pad_disc
 * calls a clickable keypad window or the keyboard mapping use -- there is no
 * separate "gamepad injection" path in intv_host, by design (see that
 * header's own comment on why direct pad injection is the one mechanism).
 *
 * Left stick/D-pad -> the disc (whichever controller side the pad is bound
 * to), unless some button has been explicitly bound to a disc segment (see
 * poll_sticks), in which case that wins. Every named button (face,
 * shoulders, sticks, D-pad, start/back/guide) is looked up in bindings.c's
 * remappable table instead of being hardwired here -- SOUTH/EAST/WEST -> the
 * three action buttons is only that table's *default*, seeded once in
 * bindings.c and editable from a keypad window's Map mode from then on, same
 * as the keyboard. A D-pad button remapped to a keypad key OR a disc segment
 * stops contributing to the raw D-pad reading too (see poll_sticks' own
 * bindings_button_is_free guard) so the same press can't do both at once.
 *
 * TWO KINDS OF DEVICE. SDL's *gamepad* abstraction only exists for hardware
 * it has a mapping for, and it tops out at 15 named buttons plus two sticks.
 * That is the wrong shape for the reason people plug a controller into this
 * emulator at all: an Intellivision-to-USB adapter. The Ultimate PC
 * Interface, raphnet's adapter and the Classic Game Controller are all plain
 * HID joysticks with no entry in SDL's mapping database -- so SDL_GetGamepads
 * never even listed them, and the app could not see the device, let alone
 * bind it. And they need far more than 15: one Intellivision controller is
 * 12 keypad keys plus 3 action buttons, an adapter carries two of them, and
 * the disc has 16 positions (jzIntv's own HACKFILE.CFG for the Ultimate PC
 * Interface names JS0_BTN_00..JS0_BTN_19).
 *
 * So a slot is either a gamepad (opened with SDL_OpenGamepad, unchanged
 * behaviour: named buttons, D-pad, analog sticks) or a raw joystick (opened
 * with SDL_OpenJoystick, reporting buttons and hats in intvsession.h's
 * INTVSESSION_PAD_BTN_RAW_BASE/_HAT_BASE bands). SDL_IsGamepad decides, once,
 * at open time. Everything downstream -- bindings.c, Map mode, persistence --
 * sees only intvsession_pad_button values and does not care which kind it
 * is; slot_button_pressed and its neighbours below are the only code that
 * does.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "gamepad_sdl.h"
#include "intv_host.h"
#include "session_internal.h"

#define MAX_PADS 8

/* ---- pure functions, unit-tested without any hardware attached --------- */

/* x: -1 (west) .. 1 (east); y: -1 (south) .. 1 (north) -- i.e. already
 * flipped from SDL's own down-positive Y axis convention; the SDL glue
 * below negates SDL_GetGamepadAxis's Y value before calling this, so the
 * function itself stays toolkit-agnostic and intuitive to test.
 *
 * Returns a clock position matching intv_host.h's disc_codes numbering
 * (0 = E, going counter-clockwise numerically -- ENE=1, NE=2, ... -- which
 * is what intv_host.h's own table documents as "clockwise" from the
 * player's perspective looking at the disc face-on; see that file), or -1
 * when the stick is within deadzone of center.
 *
 * Snaps to one of the 8 EVEN positions only (E/NE/N/NW/W/SW/S/SE -- the
 * same 8 intv_keymap.c's own DIR_* enum uses for the keyboard's arrow/IJKM
 * bindings), never one of the 8 odd "half-step" in-between codes. Those
 * half-steps are unreachable from a keyboard at all (see intv_host.h's own
 * comment on why), and an analog stick pushed toward a pure cardinal
 * inevitably wobbles a few degrees off-axis from imprecise centering --
 * with 16-way rounding that wobble was enough to land on a half-step
 * (which OR-combines two adjacent cardinal bits, e.g. SSW=64|32) on some
 * samples and the pure cardinal on others, so a straight "push left" could
 * emit a stray south-bit reading and register as an extra menu move. 8-way
 * rounding gives each cardinal a full +-22.5 degree catch instead of
 * +-11.25, which comfortably absorbs that wobble. */
int intv_disc_from_stick(float x, float y, float deadzone)
{
    const float mag = sqrtf(x * x + y * y);
    if (mag < deadzone)
        return -1;

    float angle = atan2f(y, x); /* (-pi, pi] */
    if (angle < 0)
        angle += 2.0f * (float)M_PI;

    int idx = (int)(angle / ((float)M_PI / 4.0f) + 0.5f) % 8;
    if (idx < 0)
        idx += 8;
    return idx * 2;
}

/* The D-pad: four independent buttons rather than an analog axis, so there
 * is no wobble/deadzone to reason about -- just resolve up to 8 directions
 * (a conflicting pair on one axis, e.g. up+down together, cancels that
 * axis to neutral rather than picking one arbitrarily). Same 8-position
 * output domain as intv_disc_from_stick (0/2/4/.../14, or -1 centered). */
int intv_disc_from_dpad(int up, int down, int left, int right)
{
    if (up && down) { up = 0; down = 0; }
    if (left && right) { left = 0; right = 0; }

    if (up && right)   return 2;  /* NE */
    if (up && left)    return 6;  /* NW */
    if (down && left)  return 10; /* SW */
    if (down && right) return 14; /* SE */
    if (up)    return 4;  /* N */
    if (down)  return 12; /* S */
    if (left)  return 8;  /* W */
    if (right) return 0;  /* E */
    return -1;
}

/* bindings[i] is pad i's explicit side (INTVSESSION_PAD_LEFT/_RIGHT), or -1
 * for automatic assignment. Returns the pad index driving `side`, or -1 if
 * none does. An explicit binding wins; among the rest, the first connected
 * pad takes the left side and the second takes the right (arbitrary but
 * deterministic -- there is no hardware convention to defer to the way the
 * CoCo port's joystick ports have one). Pure function, mirrors
 * coco_pad_for_port. */
int intv_pad_for_port(const int *bindings, int npads, int side)
{
    for (int i = 0; i < npads; i++)
        if (bindings[i] == side)
            return i;

    int auto_slot = 0;
    for (int i = 0; i < npads; i++)
    {
        if (bindings[i] != -1)
            continue;
        if (auto_slot == side)
            return i;
        auto_slot++;
    }
    return -1;
}

/* ---- SDL glue ------------------------------------------------------------ */

/* 4 sides, not 2: INTVSESSION_PAD_LEFT/_RIGHT (the Master Component's own
 * pair) plus INTVSESSION_PAD_ECS_LEFT/_RIGHT (the ECS's second pair, only
 * meaningful when ECS is enabled). Driving pad1 when ECS is off is
 * harmless -- nothing reads it off the peripheral bus -- so this poll loop
 * doesn't need to know ECS's on/off state at all; a gamepad simply never
 * gets auto-assigned to a side 3/4 slot unless a 3rd/4th pad is plugged in
 * (intv_pad_for_port's own automatic-assignment order). */
#define NUM_SIDES 4

/* Hats past this are ignored. Real devices have one; the Ultimate PC
 * Interface reports its disc as buttons rather than as a hat at all. */
#define MAX_HATS 4

typedef struct {
    SDL_JoystickID id;
    /* Exactly one of these is non-NULL -- see this file's own header on the
     * gamepad/joystick split. `handle` non-NULL means a mapped gamepad. */
    SDL_Gamepad *handle;
    SDL_Joystick *joy;
    int bound_side; /* -1 = automatic */
    int last_disc[NUM_SIDES]; /* last direction sent per side this pad could
                              * drive, to avoid spamming intv_host_pad_disc
                              * every poll */
    Uint8 last_hat[MAX_HATS]; /* previous hat bitmask: SDL reports a hat as a
                              * position, so the press/release edges a
                              * binding needs have to be synthesized here */
} pad_slot;

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pad_slot s_pads[MAX_PADS];
static int s_pad_count = 0;
static volatile int s_running = 0;
static volatile int s_stop_requested = 0;

/* ---- Map-mode capture (intvsession_gamepad_capture_*, intvsession.h) -----
 * While s_cap_active, every gamepad button event is swallowed here instead
 * of reaching handle_button -- not just the one eventually captured, ALL of
 * them, so a Map dialog waiting for a press can't have some other button on
 * the same pad leak a keystroke into the machine underneath it. The first
 * DOWN latches s_cap_result and s_cap_have; capture_poll consumes it and
 * disarms, same as capture_cancel without a result. Any button the device
 * can report qualifies -- a named gamepad button, a raw joystick button, or
 * a hat direction -- so an adapter's 20th button is as capturable as its
 * first. It used to be named gamepad buttons only, which left a Map dialog
 * armed forever in front of a device whose buttons SDL does not name. */
static volatile int s_cap_active = 0;
static volatile int s_cap_have = 0;
static int s_cap_result = -1;

/* Two thresholds, not one: the stick must cross the wider ENTER radius to
 * newly count as pushed in some direction, but once pushed, small in/out
 * wobble near that boundary (an analog stick basically never sits at a
 * perfectly steady raw value even when the player isn't moving it) is not
 * enough to drop back to centered until it falls under the narrower EXIT
 * radius. Without this, a stick held just past ENTER could jitter
 * centered<->pushed every poll, and each such transition is a fresh "just
 * pressed" edge to the CONFIG ROM's menu -- exactly what produced the
 * reported "any small downward nudge rockets the cursor to the bottom"
 * (each spurious re-press ran the menu's move-one-item action again,
 * with none of the typematic repeat-delay a single continuous hold gets). */
#define DEADZONE_ENTER 0.35f
#define DEADZONE_EXIT  0.15f

/* Looks `b` up in bindings.c's remappable table for `side` and injects
 * whatever pad key (if any) it currently drives there -- the table's own
 * defaults reproduce the old hardcoded SOUTH/EAST/WEST switch this replaced,
 * but any intvsession_pad_button value can end up bound to any keypad digit
 * or action button, not just those three. Takes an already-translated value
 * rather than an SDL button index because its three callers speak three
 * different dialects: a gamepad button event, a raw joystick button event,
 * and a synthesized hat edge. */
static void handle_button(intv_pad_side side, intvsession_pad_button b,
                          int pressed)
{
    intvsession_key_mapping m;

    if (b == INTVSESSION_PAD_BTN_NONE)
        return;
    /* Only acts on MAP_KEY -- a button bound to a disc segment (MAP_DISC) is
     * resolved by poll_sticks instead, which is the single writer of the
     * disc (see this file's own header and poll_sticks' own comment on why
     * a second writer here would race it). */
    m = bindings_target_from_button((intvsession_pad_side)side, b);
    if (m.kind == INTVSESSION_MAP_KEY) {
        intv_host_pad_key(side, (intv_pad_key)m.key, pressed);
    } else if (m.kind == INTVSESSION_MAP_SYSACT) {
        /* Fire on press only -- a release is meaningless for either
         * sysaction. RESET_GAME is just a flag write (intv_host_reset), safe
         * to call inline from this thread. RESET_CONFIG is NOT: intvsession_
         * reset_to_config -> intvsession_stop -> intv_gamepad_stop() would
         * pthread_join(s_thread, ...) from inside s_thread itself (EDEADLK,
         * followed by SDL_QuitSubSystem tearing down the subsystem this
         * very callback is running under). Queue it instead and let a
         * frontend's own UI-thread timer drain and fire it from there --
         * see intvsession_sysaction_post's own comment. */
        if (pressed) {
            if (m.sysact == INTVSESSION_SYSACT_RESET_GAME)
                intvsession_sysaction_fire(NULL, m.sysact);
            else
                intvsession_sysaction_post(NULL, m.sysact);
        }
    }
}

intvsession_pad_button pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return INTVSESSION_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST:           return INTVSESSION_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST:           return INTVSESSION_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH:          return INTVSESSION_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK:           return INTVSESSION_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return INTVSESSION_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START:          return INTVSESSION_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return INTVSESSION_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return INTVSESSION_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return INTVSESSION_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return INTVSESSION_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return INTVSESSION_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return INTVSESSION_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return INTVSESSION_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return INTVSESSION_PAD_BTN_DPAD_RIGHT;
    default:                               return INTVSESSION_PAD_BTN_NONE;
    }
}

/* Inverse of the table above -- see gamepad_sdl.h's own comment. */
int sdl_button_from_pad(intvsession_pad_button button)
{
    switch (button) {
    case INTVSESSION_PAD_BTN_SOUTH:          return SDL_GAMEPAD_BUTTON_SOUTH;
    case INTVSESSION_PAD_BTN_EAST:           return SDL_GAMEPAD_BUTTON_EAST;
    case INTVSESSION_PAD_BTN_WEST:           return SDL_GAMEPAD_BUTTON_WEST;
    case INTVSESSION_PAD_BTN_NORTH:          return SDL_GAMEPAD_BUTTON_NORTH;
    case INTVSESSION_PAD_BTN_BACK:           return SDL_GAMEPAD_BUTTON_BACK;
    case INTVSESSION_PAD_BTN_GUIDE:          return SDL_GAMEPAD_BUTTON_GUIDE;
    case INTVSESSION_PAD_BTN_START:          return SDL_GAMEPAD_BUTTON_START;
    case INTVSESSION_PAD_BTN_LEFT_STICK:     return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case INTVSESSION_PAD_BTN_RIGHT_STICK:    return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case INTVSESSION_PAD_BTN_LEFT_SHOULDER:  return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case INTVSESSION_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case INTVSESSION_PAD_BTN_DPAD_UP:        return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case INTVSESSION_PAD_BTN_DPAD_DOWN:      return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case INTVSESSION_PAD_BTN_DPAD_LEFT:      return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case INTVSESSION_PAD_BTN_DPAD_RIGHT:     return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default:                                return -1;
    }
}

/* ---- device-kind helpers --------------------------------------------------
 * The only code that knows whether a slot is a gamepad or a raw joystick.
 * Everything above and below deals in intvsession_pad_button values. */

/* SDL's hat bitmask -> the 8 clockwise-from-North directions
 * INTVSESSION_PAD_HAT_BASE numbers. -1 for centered or for a diagonal SDL
 * does not name (it names all four, so only centered returns -1). */
static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP:        return 0;
    case SDL_HAT_RIGHTUP:   return 1;
    case SDL_HAT_RIGHT:     return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN:      return 4;
    case SDL_HAT_LEFTDOWN:  return 5;
    case SDL_HAT_LEFT:      return 6;
    case SDL_HAT_LEFTUP:    return 7;
    default:                return -1;
    }
}

static intvsession_pad_button hat_button(int hat, int dir)
{
    return (intvsession_pad_button)(INTVSESSION_PAD_HAT_BASE +
                                    hat * INTVSESSION_PAD_HAT_DIRS + dir);
}

/* Is `b` held down on `slot` right now? Handles all three bands; 0 for a
 * button this device does not have, so an unbound-elsewhere value never
 * reads as pressed. */
static int slot_button_pressed(const pad_slot *slot, intvsession_pad_button b)
{
    if (slot->handle) {
        const int sdl = sdl_button_from_pad(b);
        return sdl >= 0 &&
               SDL_GetGamepadButton(slot->handle, (SDL_GamepadButton)sdl);
    }
    if (!slot->joy)
        return 0;
    if (INTVSESSION_PAD_BTN_IS_RAW(b)) {
        const int idx = (int)(b - INTVSESSION_PAD_BTN_RAW_BASE);
        return idx < SDL_GetNumJoystickButtons(slot->joy) &&
               SDL_GetJoystickButton(slot->joy, idx);
    }
    if (INTVSESSION_PAD_BTN_IS_HAT(b)) {
        const int idx = (int)(b - INTVSESSION_PAD_HAT_BASE);
        const int hat = idx / INTVSESSION_PAD_HAT_DIRS;
        const int dir = idx % INTVSESSION_PAD_HAT_DIRS;
        return hat < SDL_GetNumJoystickHats(slot->joy) &&
               hat_dir_from_mask(SDL_GetJoystickHat(slot->joy, hat)) == dir;
    }
    return 0; /* a *named* gamepad button on a device with no mapping */
}

/* The two axes the disc falls back to, normalized to -1..1 with up positive
 * (intv_disc_from_stick's convention -- SDL's Y is down-positive). A raw
 * joystick has no notion of a "left stick": axes 0/1 are the X/Y every HID
 * gamepad-shaped device puts first, which is what an adapter's disc lands on
 * when it reports one at all. */
static void slot_stick(const pad_slot *slot, float *x, float *y)
{
    *x = 0.0f;
    *y = 0.0f;
    if (slot->handle) {
        *x = SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        *y = -SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    } else if (slot->joy && SDL_GetNumJoystickAxes(slot->joy) >= 2) {
        *x = SDL_GetJoystickAxis(slot->joy, 0) / 32767.0f;
        *y = -SDL_GetJoystickAxis(slot->joy, 1) / 32767.0f;
    }
}

static void slot_close(pad_slot *slot)
{
    if (slot->handle)
        SDL_CloseGamepad(slot->handle);
    else if (slot->joy)
        SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                     : slot->joy    ? SDL_GetJoystickName(slot->joy)
                                    : NULL;
    return name ? name : (slot->handle ? "Gamepad" : "Joystick");
}

static int slot_for_id(SDL_JoystickID id)
{
    for (int i = 0; i < s_pad_count; i++)
        if (s_pads[i].id == id)
            return i;
    return -1;
}

/* Idempotent, and the single place the gamepad/joystick decision is made --
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(SDL_JoystickID id)
{
    pthread_mutex_lock(&s_lock);
    if (s_pad_count < MAX_PADS && slot_for_id(id) < 0)
    {
        SDL_Gamepad *g = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one (a mapping naming an axis the hardware
         * does not report, say). Falling through to the raw joystick keeps
         * it usable -- every button is still bindable, just unnamed. */
        SDL_Joystick *j = g ? NULL : SDL_OpenJoystick(id);
        if (g || j)
        {
            pad_slot *slot = &s_pads[s_pad_count++];
            memset(slot, 0, sizeof(*slot));
            slot->id = id;
            slot->handle = g;
            slot->joy = j;
            slot->bound_side = -1;
            for (int side = 0; side < NUM_SIDES; side++)
                slot->last_disc[side] = -1;
            for (int h = 0; h < MAX_HATS; h++)
                slot->last_hat[h] = SDL_HAT_CENTERED;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static void close_pad(SDL_JoystickID id)
{
    pthread_mutex_lock(&s_lock);
    int idx = slot_for_id(id);
    if (idx >= 0)
    {
        slot_close(&s_pads[idx]);
        for (int i = idx; i < s_pad_count - 1; i++)
            s_pads[i] = s_pads[i + 1];
        s_pad_count--;
    }
    pthread_mutex_unlock(&s_lock);
}

static void poll_sticks(void)
{
    pthread_mutex_lock(&s_lock);
    int bindings[MAX_PADS];
    for (int i = 0; i < s_pad_count; i++)
        bindings[i] = s_pads[i].bound_side;

    for (int side = 0; side < NUM_SIDES; side++)
    {
        int idx = intv_pad_for_port(bindings, s_pad_count, side);
        if (idx < 0)
            continue;

        pad_slot *slot = &s_pads[idx];
        int dir = -1;

        /* Priority, highest first:
         *   1. a gamepad button the user has explicitly bound to a disc
         *      segment via a keypad window's Map mode -- nothing is bound
         *      here by default (bindings_disc_buttons returns 0), so this
         *      rung is inert until someone opts in, and it is the ONLY way
         *      to reach the 8 odd half-step positions from a gamepad (the
         *      D-pad and the stick both resolve 8-way only -- see
         *      intv_disc_from_stick's own comment on why).
         *   2. the D-pad: plain digital buttons, no wobble to reason about,
         *      and a player reaching for it clearly wants precise control.
         *   3. the left analog stick.
         * The disc holds exactly one position at a time
         * (intv_host_pad_disc overwrites, it never ORs), so if two bound
         * buttons are somehow held together, the lower clock position wins
         * -- deterministic and documented rather than whichever SDL happens
         * to poll first.
         *
         * Rung 2's own bindings_button_is_free gate already covers a D-pad
         * button remapped to a disc segment (bindings_button_is_free counts
         * both kinds of claim, see bindings.h), so rungs 1 and 2 can never
         * both act on the same physical press. */
        intvsession_pad_button disc_btn[INTVSESSION_DISC_POSITIONS];
        if (bindings_disc_buttons((intvsession_pad_side)side, disc_btn) > 0)
        {
            for (int d = 0; d < INTVSESSION_DISC_POSITIONS; d++)
            {
                if (disc_btn[d] == INTVSESSION_PAD_BTN_NONE)
                    continue;
                if (slot_button_pressed(slot, disc_btn[d]))
                {
                    dir = d;
                    break;
                }
            }
        }

        /* The D-pad wins over the stick when pressed: it is a set of plain
         * digital buttons, no wobble to reason about, and a player reaching
         * for it clearly wants precise control. Falls through to the analog
         * stick when the D-pad is neutral.
         *
         * Each direction only counts if bindings.c doesn't have that D-pad
         * button claimed for a keypad key or a disc segment on this side --
         * a button remapped to drive one of those stops also nudging the
         * disc from its own raw D-pad state every poll, so the same
         * physical press can't do both at once (see this file's own
         * header). */
        if (dir == -1)
        {
            /* A raw joystick has no D-pad; hat 0 is the same four digital
             * directions in the same role, so it takes this rung. Both go
             * through the same bindings_button_is_free gate, on whichever
             * band names the direction. */
            const intvsession_pad_button up =
                slot->handle ? INTVSESSION_PAD_BTN_DPAD_UP    : hat_button(0, 0);
            const intvsession_pad_button rt =
                slot->handle ? INTVSESSION_PAD_BTN_DPAD_RIGHT : hat_button(0, 2);
            const intvsession_pad_button dn =
                slot->handle ? INTVSESSION_PAD_BTN_DPAD_DOWN  : hat_button(0, 4);
            const intvsession_pad_button lf =
                slot->handle ? INTVSESSION_PAD_BTN_DPAD_LEFT  : hat_button(0, 6);
            const intvsession_pad_side ps = (intvsession_pad_side)side;

            dir = intv_disc_from_dpad(
                bindings_button_is_free(ps, up) && slot_button_pressed(slot, up),
                bindings_button_is_free(ps, dn) && slot_button_pressed(slot, dn),
                bindings_button_is_free(ps, lf) && slot_button_pressed(slot, lf),
                bindings_button_is_free(ps, rt) && slot_button_pressed(slot, rt));

            /* A hat only ever reports one position, so the diagonals a
             * gamepad reaches by holding two D-pad buttons need reading off
             * the hat itself -- intv_disc_from_dpad above sees at most one
             * of the four set. */
            if (dir == -1 && !slot->handle)
                for (int d = 1; d < INTVSESSION_PAD_HAT_DIRS; d += 2)
                {
                    const intvsession_pad_button b = hat_button(0, d);
                    if (bindings_button_is_free(ps, b) &&
                        slot_button_pressed(slot, b))
                    {
                        /* Hat direction (clockwise from N) -> disc clock
                         * position (counter-clockwise from E, see
                         * intv_host.h): NE=1 -> 2, SE=3 -> 14, SW=5 -> 10,
                         * NW=7 -> 6. */
                        static const int diag[INTVSESSION_PAD_HAT_DIRS] = {
                            4, 2, 0, 14, 12, 10, 8, 6
                        };
                        dir = diag[d];
                        break;
                    }
                }
        }

        if (dir == -1)
        {
            float x, y;
            /* See DEADZONE_ENTER/_EXIT's own comment: use the narrower
             * exit radius while already pushed, the wider entry radius
             * while centered, so boundary wobble can't rapid-fire
             * centered/pushed transitions. */
            const int was_active = slot->last_disc[side] != -1;
            slot_stick(slot, &x, &y);
            dir = intv_disc_from_stick(
                x, y, was_active ? DEADZONE_EXIT : DEADZONE_ENTER);
        }

        if (dir != slot->last_disc[side])
        {
            intv_host_pad_disc((intv_pad_side)side, dir);
            slot->last_disc[side] = dir;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* One button edge from any of the three sources, already translated. Latches
 * a Map-mode capture if one is armed, otherwise injects. Shared so a gamepad
 * button, a raw joystick button and a synthesized hat edge all behave
 * identically -- including the swallow-everything-while-capturing rule (see
 * this file's own header on why ALL of them, not just the one eventually
 * recorded). */
static void dispatch_button(SDL_JoystickID which, intvsession_pad_button b,
                            int down)
{
    int idx;
    int bindings[MAX_PADS];
    int capturing;

    if (b == INTVSESSION_PAD_BTN_NONE)
        return;

    pthread_mutex_lock(&s_lock);
    capturing = s_cap_active;
    if (capturing && down && !s_cap_have) {
        s_cap_result = (int)b;
        s_cap_have = 1;
    }
    idx = slot_for_id(which);
    for (int i = 0; i < s_pad_count; i++)
        bindings[i] = s_pads[i].bound_side;
    pthread_mutex_unlock(&s_lock);

    if (capturing || idx < 0)
        return;
    for (int side = 0; side < NUM_SIDES; side++)
        if (intv_pad_for_port(bindings, s_pad_count, side) == idx)
            handle_button((intv_pad_side)side, b, down);
}

/* SDL reports a hat as an absolute position, so the release of the old
 * direction and the press of the new one have to be synthesized from the
 * change. Reads and updates the slot's remembered mask under the lock, then
 * dispatches outside it (dispatch_button takes the same lock). */
static void handle_hat_motion(SDL_JoystickID which, int hat, Uint8 mask)
{
    int prev_dir, new_dir, idx;

    if (hat < 0 || hat >= MAX_HATS)
        return;

    pthread_mutex_lock(&s_lock);
    idx = slot_for_id(which);
    if (idx < 0) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    prev_dir = hat_dir_from_mask(s_pads[idx].last_hat[hat]);
    s_pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&s_lock);

    new_dir = hat_dir_from_mask(mask);
    if (new_dir == prev_dir)
        return;
    if (prev_dir >= 0)
        dispatch_button(which, hat_button(hat, prev_dir), 0);
    if (new_dir >= 0)
        dispatch_button(which, hat_button(hat, new_dir), 1);
}

static void *thread_main(void *arg)
{
    (void)arg;

    /* Every joystick, not just the ones SDL has a gamepad mapping for --
     * SDL_GetGamepads would silently omit an Intellivision-to-USB adapter
     * entirely. open_pad sorts out which kind each one is. */
    int js_count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&js_count);
    if (ids)
    {
        for (int i = 0; i < js_count; i++)
            open_pad(ids[i]);
        SDL_free(ids);
    }

    while (!s_stop_requested)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            /* SDL posts the JOYSTICK_ pair for every device and the GAMEPAD_
             * pair as well for a mapped one; open_pad/close_pad are
             * idempotent, so taking all four costs nothing and means neither
             * kind can be missed. */
            case SDL_EVENT_JOYSTICK_ADDED:
                open_pad(ev.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                close_pad(ev.jdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                open_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                close_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(ev.gbutton.which,
                                pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            {
                /* A mapped gamepad also emits these, for the same press it
                 * already reported as a GAMEPAD_BUTTON event -- taking both
                 * would fire every binding twice. The slot's kind decides. */
                int gamepad;
                pthread_mutex_lock(&s_lock);
                const int idx = slot_for_id(ev.jbutton.which);
                gamepad = idx >= 0 && s_pads[idx].handle != NULL;
                pthread_mutex_unlock(&s_lock);
                if (gamepad || ev.jbutton.button >= 64)
                    break;
                dispatch_button(ev.jbutton.which,
                                (intvsession_pad_button)
                                    (INTVSESSION_PAD_BTN_RAW_BASE +
                                     ev.jbutton.button),
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }

        poll_sticks();
        SDL_Delay(16); /* ~60Hz poll, matching the video frame rate */
    }

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_pad_count; i++)
        slot_close(&s_pads[i]);
    s_pad_count = 0;
    pthread_mutex_unlock(&s_lock);

    return NULL;
}

int intv_gamepad_start(void)
{
    if (s_running)
        return 0;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "intv_gamepad: SDL_InitSubSystem failed: %s\n",
                SDL_GetError());
        return -1;
    }
    s_stop_requested = 0;
    if (pthread_create(&s_thread, NULL, thread_main, NULL) != 0)
    {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    s_running = 1;
    return 0;
}

void intv_gamepad_stop(void)
{
    if (!s_running)
        return;
    s_stop_requested = 1;
    pthread_join(s_thread, NULL);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    s_running = 0;
}

void intv_gamepad_forget_disc(void)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_pad_count; i++)
        for (int side = 0; side < NUM_SIDES; side++)
            /* -2, not -1: -1 is "centered", a value poll_sticks legitimately
             * sends and would then treat as unchanged if the stick is
             * already resting there. -2 is outside intv_disc_from_stick/
             * _dpad's -1..15 range, so the very next poll's dir != last_disc
             * check is guaranteed true regardless of what dir turns out to
             * be, even -1 itself. */
            s_pads[i].last_disc[side] = -2;
    pthread_mutex_unlock(&s_lock);
}

int intvsession_gamepad_count(intvsession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    const int n = s_pad_count;
    pthread_mutex_unlock(&s_lock);
    return n;
}

int intvsession_gamepad_name(intvsession *s, int idx, char *dst, int dstsz)
{
    (void)s;
    int len = 0;
    pthread_mutex_lock(&s_lock);
    if (idx >= 0 && idx < s_pad_count)
    {
        const char *name = slot_name(&s_pads[idx]);
        len = (int)strlen(name);
        if (dst && dstsz > 0)
            snprintf(dst, (size_t)dstsz, "%s", name);
    }
    pthread_mutex_unlock(&s_lock);
    return len;
}

void intvsession_gamepad_assign(intvsession *s, int idx, int side)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    if (idx >= 0 && idx < s_pad_count)
        s_pads[idx].bound_side = side;
    pthread_mutex_unlock(&s_lock);
}

int intvsession_gamepad_assignment(intvsession *s, int idx)
{
    (void)s;
    int side = -1;
    pthread_mutex_lock(&s_lock);
    if (idx >= 0 && idx < s_pad_count)
        side = s_pads[idx].bound_side;
    pthread_mutex_unlock(&s_lock);
    return side;
}

int intvsession_gamepad_effective_side(intvsession *s, int idx)
{
    (void)s;
    int bindings[MAX_PADS];
    int found = -1;

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_pad_count; i++)
        bindings[i] = s_pads[i].bound_side;
    for (int side = 0; side < NUM_SIDES && found < 0; side++)
        if (intv_pad_for_port(bindings, s_pad_count, side) == idx)
            found = side;
    pthread_mutex_unlock(&s_lock);
    return found;
}

void intvsession_gamepad_capture_begin(intvsession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    s_cap_active = 1;
    s_cap_have = 0;
    s_cap_result = -1;
    pthread_mutex_unlock(&s_lock);
}

void intvsession_gamepad_capture_cancel(intvsession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    s_cap_active = 0;
    s_cap_have = 0;
    pthread_mutex_unlock(&s_lock);
}

int intvsession_gamepad_capture_poll(intvsession *s,
                                     intvsession_pad_button *button)
{
    int got;
    (void)s;
    pthread_mutex_lock(&s_lock);
    got = s_cap_have;
    if (got) {
        if (button)
            *button = (intvsession_pad_button)s_cap_result;
        s_cap_have = 0;
        s_cap_active = 0;
    }
    pthread_mutex_unlock(&s_lock);
    return got;
}
