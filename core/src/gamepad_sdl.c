/*
 * intvsession gamepad backend: SDL3 for enumeration/input only. Runs a
 * background thread that opens/closes SDL gamepads as they connect, and
 * translates their state into the same intv_host_pad_key/intv_host_pad_disc
 * calls a clickable keypad window or the keyboard mapping use -- there is no
 * separate "gamepad injection" path in intv_host, by design (see that
 * header's own comment on why direct pad injection is the one mechanism).
 *
 * Left stick -> the disc (whichever controller side the pad is bound to);
 * SOUTH/EAST/WEST face buttons -> the three action buttons (top/lower-right/
 * lower-left). No keypad digit mapping -- a physical gamepad has nowhere
 * near 12 buttons to spare for that, and the keypad window covers it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

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

typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad *handle;
    int bound_side; /* -1 = automatic */
    int last_disc[2]; /* last direction sent per side this pad could drive,
                       * to avoid spamming intv_host_pad_disc every poll */
} pad_slot;

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pad_slot s_pads[MAX_PADS];
static int s_pad_count = 0;
static volatile int s_running = 0;
static volatile int s_stop_requested = 0;

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

static void handle_button(intv_pad_side side, Uint8 button, int pressed)
{
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        intv_host_pad_key(side, INTV_PAD_ACTION_TOP, pressed);
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
        intv_host_pad_key(side, INTV_PAD_ACTION_LOWER_RIGHT, pressed);
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        intv_host_pad_key(side, INTV_PAD_ACTION_LOWER_LEFT, pressed);
        break;
    default:
        break;
    }
}

static int slot_for_id(SDL_JoystickID id)
{
    for (int i = 0; i < s_pad_count; i++)
        if (s_pads[i].id == id)
            return i;
    return -1;
}

static void open_pad(SDL_JoystickID id)
{
    pthread_mutex_lock(&s_lock);
    if (s_pad_count < MAX_PADS && slot_for_id(id) < 0)
    {
        SDL_Gamepad *h = SDL_OpenGamepad(id);
        if (h)
        {
            pad_slot *slot = &s_pads[s_pad_count++];
            slot->id = id;
            slot->handle = h;
            slot->bound_side = -1;
            slot->last_disc[0] = -1;
            slot->last_disc[1] = -1;
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
        SDL_CloseGamepad(s_pads[idx].handle);
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

    for (int side = 0; side < 2; side++)
    {
        int idx = intv_pad_for_port(bindings, s_pad_count, side);
        if (idx < 0)
            continue;

        pad_slot *slot = &s_pads[idx];

        /* The D-pad wins when pressed: it is a set of plain digital
         * buttons, no wobble to reason about, and a player reaching for
         * it clearly wants precise control. Falls through to the analog
         * stick when the D-pad is neutral. */
        int dir = intv_disc_from_dpad(
            SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP),
            SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN),
            SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT),
            SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));

        if (dir == -1)
        {
            const float x =
                SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX) /
                32767.0f;
            const float y =
                -SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY) /
                32767.0f; /* SDL: down is positive; disc math wants up
                          * positive, see intv_disc_from_stick. */
            /* See DEADZONE_ENTER/_EXIT's own comment: use the narrower
             * exit radius while already pushed, the wider entry radius
             * while centered, so boundary wobble can't rapid-fire
             * centered/pushed transitions. */
            const int was_active = slot->last_disc[side] != -1;
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

static void *thread_main(void *arg)
{
    (void)arg;

    int gp_count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&gp_count);
    if (ids)
    {
        for (int i = 0; i < gp_count; i++)
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
            case SDL_EVENT_GAMEPAD_ADDED:
                open_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                close_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            {
                pthread_mutex_lock(&s_lock);
                int idx = slot_for_id(ev.gbutton.which);
                int bindings[MAX_PADS];
                for (int i = 0; i < s_pad_count; i++)
                    bindings[i] = s_pads[i].bound_side;
                pthread_mutex_unlock(&s_lock);
                if (idx < 0)
                    break;
                for (int side = 0; side < 2; side++)
                    if (intv_pad_for_port(bindings, s_pad_count, side) == idx)
                        handle_button((intv_pad_side)side, ev.gbutton.button,
                                     ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            }
            default:
                break;
            }
        }

        poll_sticks();
        SDL_Delay(16); /* ~60Hz poll, matching the video frame rate */
    }

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_pad_count; i++)
        SDL_CloseGamepad(s_pads[i].handle);
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
        const char *name = SDL_GetGamepadName(s_pads[idx].handle);
        if (!name)
            name = "Gamepad";
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
