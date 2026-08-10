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
 * when the stick is within deadzone of center. */
int intv_disc_from_stick(float x, float y, float deadzone)
{
    const float mag = sqrtf(x * x + y * y);
    if (mag < deadzone)
        return -1;

    float angle = atan2f(y, x); /* (-pi, pi] */
    if (angle < 0)
        angle += 2.0f * (float)M_PI;

    int idx = (int)(angle / ((float)M_PI / 8.0f) + 0.5f) % 16;
    if (idx < 0)
        idx += 16;
    return idx;
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

#define DEADZONE 0.35f

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
        const float x = SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX)
                       / 32767.0f;
        const float y = -SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY)
                       / 32767.0f; /* SDL: down is positive; disc math wants
                                   * up positive, see intv_disc_from_stick. */
        const int dir = intv_disc_from_stick(x, y, DEADZONE);
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
