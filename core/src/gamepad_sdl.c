/*
 * intvsession gamepad backend: SDL3 for enumeration/input only. Runs a
 * background thread that opens/closes SDL gamepads as they connect, and
 * translates their state into input sources (input_merge.h): each pad is its
 * own pair of sources (buttons / D-pad / hat, and the analog stick), merged
 * with the keyboard and the Controllers window's pointer before anything
 * reaches intv_host_pad_key/intv_host_pad_disc. So a pad letting go never
 * cancels another source's hold, and an unplugged pad lets go of exactly what
 * it held.
 *
 * Pads are assigned to sides in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run (matched by its SDL GUID, else its name), gets it back, and
 * every connect / disconnect bumps the generation and is reported as text
 * (intvsession_gamepad_last_event) for the frontend's toast.
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
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "gamepad_sdl.h"
#include "input_merge.h"
#include "intv_host.h"
#include "pad_assign.h"
#include "session_internal.h"

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

    /* The automatic pads fill only the sides no pad has taken explicitly, in
     * order: this side's rank among the free sides picks which of them. */
    int rank = side;
    for (int s = 0; s < side; s++)
        for (int i = 0; i < npads; i++)
            if (bindings[i] == s)
            {
                rank--;
                break;
            }

    int auto_slot = 0;
    for (int i = 0; i < npads; i++)
    {
        if (bindings[i] != -1)
            continue;
        if (auto_slot == rank)
            return i;
        auto_slot++;
    }
    return -1;
}

/* ---- SDL glue ------------------------------------------------------------ */

/* 4 sides: INTVSESSION_PAD_LEFT/_RIGHT (the Master Component's own pair) plus
 * INTVSESSION_PAD_ECS_LEFT/_RIGHT (the ECS's second pair), the last two only
 * with ECS forced on (g->nsides). */
#define NUM_SIDES INTV_SIDES

/* Hats past this are ignored. Real devices have one; the Ultimate PC
 * Interface reports its disc as buttons rather than as a hat at all. */
#define MAX_HATS 4

#define MAX_PADS INTV_MAX_PADS

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
#define TRIGGER_ON     0.5f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* the input_merge.h source number for this pad:
                               * stable while it is connected, whatever its
                               * slot */
    /* Exactly one of these is non-NULL -- see this file's own header on the
     * gamepad/joystick split. `handle` non-NULL means a mapped gamepad. */
    SDL_Gamepad *handle;
    SDL_Joystick *joy;
    int assign;               /* explicit side, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int side;                 /* the side it drove on the last poll, or -1 */
    /* The disc position each of this pad's two sources holds (-1 = none, so
     * the very next poll re-asserts anything held): the buttons / D-pad / hat
     * source and the analog-stick source. */
    int last_pad_dir;
    int last_stick_dir;
    /* What each button's press asserted (key + 1, 0 = nothing), so its release
     * lets go of exactly that even if the binding changed meanwhile, and a pad
     * that goes away drops what it held. */
    int16_t btn_key[256];
    SDL_GUID guid;
    char guidstr[INTV_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[INTV_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS]; /* previous hat bitmask: SDL reports a hat as a
                               * position, so the press/release edges a
                               * binding needs have to be synthesized here */
} pad_slot;

typedef struct {
    struct intvsession *session;   /* may be NULL (tests) */
    pthread_t thread;
    pthread_mutex_t lock;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    int nsides;               /* 2, or 4 with ECS forced on */
    unsigned generation;
    int quiet_scan;           /* the first scan of a restarted session re-finds
                               * pads without announcing them */
    /* ---- Map-mode capture (intvsession_gamepad_capture_*, intvsession.h) --
     * While cap_active, every gamepad button event is swallowed instead of
     * reaching dispatch_button -- not just the one eventually captured, ALL of
     * them, so a Map dialog waiting for a press can't have some other button
     * on the same pad leak a keystroke into the machine underneath it. The
     * first DOWN latches cap_result and cap_have; capture_poll consumes it and
     * disarms, same as capture_cancel without a result. Any button the device
     * can report qualifies -- a named gamepad button, a trigger, a raw
     * joystick button, or a hat direction. */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for pads
     * that are away as well as those here; guarded by lock */
    intv_pad_table table;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* The persisted generation and quiet-scan flag are kept in the session, so a
 * restart carries them on; with no session (tests) they are process-wide. */
static unsigned s_bare_gen_base;
static int s_bare_restarted;

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

static float axis_norm(Sint16 val)
{
    return val < 0 ? (float)val / 32768.0f : (float)val / 32767.0f;
}

static float trigger_value(const pad_slot *slot, int right)
{
    if (!slot->handle)
        return 0.0f;
    return axis_norm(SDL_GetGamepadAxis(slot->handle,
        right ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
}

/* Is `b` held down on `slot` right now? Handles all four bands; 0 for a
 * button this device does not have, so an unbound-elsewhere value never
 * reads as pressed. */
static int slot_button_pressed(const pad_slot *slot, intvsession_pad_button b)
{
    if (b == INTVSESSION_PAD_BTN_LEFT_TRIGGER ||
        b == INTVSESSION_PAD_BTN_RIGHT_TRIGGER)
        return trigger_value(slot, b == INTVSESSION_PAD_BTN_RIGHT_TRIGGER) >=
               TRIGGER_ON;
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

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                     : slot->joy    ? SDL_GetJoystickName(slot->joy)
                                    : NULL;
    return name ? name : (slot->handle ? "Gamepad" : "Joystick");
}

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int idx;
    for (idx = 0; idx < g->npads; idx++)
        if (g->pads[idx].id == id)
            return idx;
    return -1;
}

/* Lets go of everything the pad holds: its buttons and its stick are sources
 * of their own, cleared here and nowhere else's. */
static void slot_release_all(pad_slot *slot)
{
    memset(slot->btn_key, 0, sizeof slot->btn_key);
    slot->last_pad_dir = -1;
    slot->last_stick_dir = -1;
    input_pad_clear(slot->owner);
}

static void slot_close(pad_slot *slot)
{
    slot_release_all(slot);
    if (slot->handle)
        SDL_CloseGamepad(slot->handle);
    if (slot->joy)
        SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

/* The side a slot drives right now. Locked by the caller. */
static int effective_side_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], pad, side;
    for (pad = 0; pad < MAX_PADS; pad++)
        assign[pad] = -1;
    /* a side the machine lacks (ECS off) is no assignment: Automatic */
    for (pad = 0; pad < g->npads; pad++)
        assign[pad] = g->pads[pad].assign < g->nsides ? g->pads[pad].assign : -1;
    for (side = 0; side < g->nsides; side++)
        if (intv_pad_for_port(assign, g->npads, side) == idx)
            return side;
    return -1;
}

/* Gives the pad in slot idx its remembered side. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g->table, never writes. */
static void restore_assignment(gamepad_state *g, int idx)
{
    pad_slot *slot = &g->pads[idx];
    int ent = intv_pad_table_find(&g->table, slot->guidstr, slot->name);
    int exact, pad;

    /* a side the machine lacks (ECS off): the pad stays automatic, the entry
     * stays in the table for a run that has it */
    if (ent < 0 || g->table.entry[ent].side >= g->nsides)
        return;
    exact = !strcmp(g->table.entry[ent].guid, slot->guidstr);
    for (pad = 0; pad < g->npads; pad++) {
        pad_slot *other = &g->pads[pad];
        if (pad == idx || other->assign < 0 ||
            intv_pad_table_find(&g->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name)
            return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g->table.entry[ent].side;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made --
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int announce = 0;

    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one (a mapping naming an axis the hardware
         * does not report, say). Falling through to the raw joystick keeps
         * it usable -- every button is still bindable, just unnamed. */
        SDL_Joystick *joy = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || joy) {
            pad_slot *slot = &g->pads[g->npads++];
            int hat, owner, used = 0, side;
            for (owner = 0; owner < g->npads - 1; owner++)
                used |= 1 << g->pads[owner].owner;
            memset(slot, 0, sizeof *slot);
            for (owner = 0; owner < MAX_PADS && (used & (1 << owner)); owner++) {}
            slot->owner = owner;
            slot->id = id;
            slot->handle = gp;
            slot->joy = joy;
            slot->assign = -1;
            slot->side = -1;
            slot->last_pad_dir = -1;
            slot->last_stick_dir = -1;
            for (hat = 0; hat < MAX_HATS; hat++)
                slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g, g->npads - 1);
            fprintf(stderr, "intv gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            if (!g->quiet_scan) {
                side = effective_side_locked(g, g->npads - 1);
                if (side >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (%s)", slot->name,
                             intvsession_pad_side_name((intvsession_pad_side)side));
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                g->generation++;
                announce = 1;
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (announce)
        session_gamepad_event(g->session, msg);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int idx, pad, announce = 0;

    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        fprintf(stderr, "intv gamepad: %s disconnected\n", g->pads[idx].name);
        snprintf(msg, sizeof msg, "Disconnected: %s", g->pads[idx].name);
        /* its explicit assignment stays in the table: it comes back with it;
         * what it held is let go */
        slot_close(&g->pads[idx]);
        for (pad = idx; pad < g->npads - 1; pad++)
            g->pads[pad] = g->pads[pad + 1];
        g->npads--;
        g->generation++;
        announce = 1;
    }
    pthread_mutex_unlock(&g->lock);
    if (announce)
        session_gamepad_event(g->session, msg);
}

/* The slot's side as of now. When it has changed -- the pad was assigned
 * elsewhere, or the machine lost the side -- what it held belongs to the old
 * arrangement and is let go. A pad that had no side holds nothing. Called by
 * the poll and by the button dispatch alike, so a button is always pressed on
 * the side the pad has at that moment: a press that lands just before the poll
 * that would have noticed the move is not undone by it. Locked by the caller. */
static void slot_sync_side(pad_slot *slot, int side)
{
    if (side == slot->side)
        return;
    if (slot->side >= 0)
        slot_release_all(slot);
    slot->side = side;
}

/* One button edge from any of the sources, already translated. Latches a
 * Map-mode capture if one is armed, otherwise looks the button up in
 * bindings.c's table for the pad's side and holds / releases what it drives.
 * Shared so a gamepad button, a trigger, a raw joystick button and a
 * synthesized hat edge all behave identically -- including the
 * swallow-everything-while-capturing rule. A button bound to a disc segment
 * (MAP_DISC) is resolved by poll_one instead, which is the single writer of
 * the pad's disc source. */
static void dispatch_button(gamepad_state *g, SDL_JoystickID which,
                            intvsession_pad_button b, int down)
{
    int idx, side = -1, owner = 0, key = -1, sysact = -1;

    if (b == INTVSESSION_PAD_BTN_NONE || (int)b < 0 || (int)b > 255)
        return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        if (down && !g->cap_have) {
            g->cap_result = (int)b;
            g->cap_have = 1;
        }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        side = effective_side_locked(g, idx);
        slot_sync_side(slot, side);
        owner = slot->owner;
        if (side >= 0) {
            if (down) {
                const intvsession_key_mapping map =
                    bindings_target_from_button((intvsession_pad_side)side, b);
                if (map.kind == INTVSESSION_MAP_KEY) {
                    key = (int)map.key;
                    slot->btn_key[b] = (int16_t)(key + 1);
                } else if (map.kind == INTVSESSION_MAP_SYSACT) {
                    sysact = (int)map.sysact;
                }
            } else if (slot->btn_key[b]) {
                /* what the press asserted, whatever the binding says now */
                key = slot->btn_key[b] - 1;
                slot->btn_key[b] = 0;
            }
        }
    }
    pthread_mutex_unlock(&g->lock);

    /* A system action fires on the press only. Reset Game is just a flag
     * write (intv_host_reset), safe to call inline from this thread. The others
     * are NOT: intvsession_reset_to_config -> intvsession_stop ->
     * intv_gamepad_stop() would pthread_join this very thread (EDEADLK,
     * followed by SDL_QuitSubSystem tearing down the subsystem this callback
     * is running under). Queue them for a frontend's UI-thread timer instead
     * -- see intvsession_sysaction_post's own comment. */
    if (sysact >= 0) {
        if (down) {
            if (sysact == INTVSESSION_SYSACT_RESET_GAME)
                intvsession_sysaction_fire(g->session,
                                           (intvsession_sysaction)sysact);
            else
                intvsession_sysaction_post(g->session,
                                           (intvsession_sysaction)sysact);
        }
        return;
    }
    if (key >= 0)
        input_key(INTV_SRC_PAD(owner), side, key, down);
}

/* SDL reports a hat as an absolute position, so the release of the old
 * direction and the press of the new one have to be synthesized from the
 * change. Reads and updates the slot's remembered mask under the lock, then
 * dispatches outside it (dispatch_button takes the same lock). */
static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat,
                              Uint8 mask)
{
    int prev_dir, new_dir, idx;

    if (hat < 0 || hat >= MAX_HATS)
        return;

    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0) {
        pthread_mutex_unlock(&g->lock);
        return;
    }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);

    new_dir = hat_dir_from_mask(mask);
    if (new_dir == prev_dir)
        return;
    if (prev_dir >= 0)
        dispatch_button(g, which, hat_button(hat, prev_dir), 0);
    if (new_dir >= 0)
        dispatch_button(g, which, hat_button(hat, new_dir), 1);
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive
 * (relocking inside the poll loop froze the thread on the first trigger
 * edge). */
typedef struct {
    SDL_JoystickID id;
    intvsession_pad_button button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

/* One pad's disc and triggers for the side it drives. Priority inside the
 * "buttons" source, highest first:
 *   1. a gamepad button the user has explicitly bound to a disc segment via
 *      Map -- nothing is bound here by default (bindings_disc_buttons returns
 *      0), so this rung is inert until someone opts in, and it is the ONLY way
 *      to reach the 8 odd half-step positions from a gamepad (the D-pad and
 *      the stick both resolve 8-way only -- see intv_disc_from_stick).
 *   2. the D-pad / hat 0: plain digital buttons, no wobble to reason about,
 *      and a player reaching for it clearly wants precise control. Each
 *      direction only counts if bindings.c doesn't have that button claimed
 *      for a keypad key or a disc segment on this side, so the same press
 *      can't do both.
 * The analog stick is a source of its own, ranked below those (input_merge.h):
 * the disc holds one position, and the merge picks it. Where two bound buttons
 * are somehow held together, the lower clock position wins. */
static void poll_one(gamepad_state *g, pad_slot *slot, int side,
                     trigger_edge *edges, int *nedges)
{
    intvsession_pad_button disc_btn[INTVSESSION_DISC_POSITIONS];
    int pad_dir = -1, stick_dir = -1, dis;
    float stick_x, stick_y;

    slot_sync_side(slot, side);   /* moved, or the machine lost the side */
    if (side < 0)
        return;   /* a pad with no side (more pads than sides) plays nothing */

    if (bindings_disc_buttons((intvsession_pad_side)side, disc_btn) > 0) {
        for (dis = 0; dis < INTVSESSION_DISC_POSITIONS; dis++) {
            if (disc_btn[dis] == INTVSESSION_PAD_BTN_NONE)
                continue;
            if (slot_button_pressed(slot, disc_btn[dis])) {
                pad_dir = dis;
                break;
            }
        }
    }

    if (pad_dir == -1) {
        /* A raw joystick has no D-pad; hat 0 is the same four digital
         * directions in the same role, so it takes this rung. Both go through
         * the same bindings_button_is_free gate, on whichever band names the
         * direction. */
        const intvsession_pad_button up =
            slot->handle ? INTVSESSION_PAD_BTN_DPAD_UP    : hat_button(0, 0);
        const intvsession_pad_button rt =
            slot->handle ? INTVSESSION_PAD_BTN_DPAD_RIGHT : hat_button(0, 2);
        const intvsession_pad_button dn =
            slot->handle ? INTVSESSION_PAD_BTN_DPAD_DOWN  : hat_button(0, 4);
        const intvsession_pad_button lf =
            slot->handle ? INTVSESSION_PAD_BTN_DPAD_LEFT  : hat_button(0, 6);
        const intvsession_pad_side ps = (intvsession_pad_side)side;

        pad_dir = intv_disc_from_dpad(
            bindings_button_is_free(ps, up) && slot_button_pressed(slot, up),
            bindings_button_is_free(ps, dn) && slot_button_pressed(slot, dn),
            bindings_button_is_free(ps, lf) && slot_button_pressed(slot, lf),
            bindings_button_is_free(ps, rt) && slot_button_pressed(slot, rt));

        /* A hat only ever reports one position, so the diagonals a gamepad
         * reaches by holding two D-pad buttons need reading off the hat
         * itself -- intv_disc_from_dpad above sees at most one of the four
         * set. */
        if (pad_dir == -1 && !slot->handle) {
            int diag_idx;
            for (diag_idx = 1; diag_idx < INTVSESSION_PAD_HAT_DIRS;
                 diag_idx += 2) {
                const intvsession_pad_button btn = hat_button(0, diag_idx);
                if (bindings_button_is_free(ps, btn) &&
                    slot_button_pressed(slot, btn)) {
                    /* Hat direction (clockwise from N) -> disc clock
                     * position (counter-clockwise from E, see intv_host.h):
                     * NE=1 -> 2, SE=3 -> 14, SW=5 -> 10, NW=7 -> 6. */
                    static const int diag[INTVSESSION_PAD_HAT_DIRS] = {
                        4, 2, 0, 14, 12, 10, 8, 6
                    };
                    pad_dir = diag[diag_idx];
                    break;
                }
            }
        }
    }

    /* The stick: the narrower exit radius while already pushed, the wider
     * entry radius while centered (see DEADZONE_ENTER's comment). */
    slot_stick(slot, &stick_x, &stick_y);
    stick_dir = intv_disc_from_stick(
        stick_x, stick_y, slot->last_stick_dir >= 0 ? DEADZONE_EXIT : DEADZONE_ENTER);

    if (pad_dir != slot->last_pad_dir) {
        slot->last_pad_dir = pad_dir;
        input_disc(INTV_SRC_PAD(slot->owner), side, pad_dir);
    }
    if (stick_dir != slot->last_stick_dir) {
        slot->last_stick_dir = stick_dir;
        input_disc(INTV_SRC_STICK(slot->owner), side, stick_dir);
    }

    /* the analog triggers, as buttons: queued, dispatched after the unlock */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            const int down = trigger_value(slot, trig) >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].id = slot->id;
                    edges[*nedges].button = trig ? INTVSESSION_PAD_BTN_RIGHT_TRIGGER
                                                 : INTVSESSION_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int pad, nedges = 0;
    trigger_edge edges[MAX_EDGES];

    pthread_mutex_lock(&g->lock);
    for (pad = 0; pad < g->npads; pad++)
        poll_one(g, &g->pads[pad], effective_side_locked(g, pad), edges,
                 &nedges);
    pthread_mutex_unlock(&g->lock);
    for (pad = 0; pad < nedges; pad++)
        dispatch_button(g, edges[pad].id, edges[pad].button, edges[pad].down);
}

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, pad;

    /* Every joystick, not just the ones SDL has a gamepad mapping for --
     * SDL_GetGamepads would silently omit an Intellivision-to-USB adapter
     * entirely. open_pad sorts out which kind each one is. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (pad = 0; pad < count; pad++)
            open_pad(g, ids[pad]);
        SDL_free(ids);
    }
    /* only the first scan is quiet: a pad plugged in later is a hot-plug */
    pthread_mutex_lock(&g->lock);
    g->quiet_scan = 0;
    pthread_mutex_unlock(&g->lock);

    while (!g->stop_requested) {
        SDL_Event ev;
        int npads, slice;

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            /* SDL posts the JOYSTICK_ pair for every device and the GAMEPAD_
             * pair as well for a mapped one; open_pad/close_pad are
             * idempotent, so taking all four costs nothing and means neither
             * kind can be missed. */
            case SDL_EVENT_JOYSTICK_ADDED:
                open_pad(g, ev.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                close_pad(g, ev.jdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                open_pad(g, ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                close_pad(g, ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            {
                /* A mapped gamepad also emits these, for the same press it
                 * already reported as a GAMEPAD_BUTTON event -- taking both
                 * would fire every binding twice. The slot's kind decides. */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64)
                    break;
                dispatch_button(g, ev.jbutton.which,
                                (intvsession_pad_button)
                                    (INTVSESSION_PAD_BTN_RAW_BASE +
                                     ev.jbutton.button),
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }

        poll_sticks(g);
        pthread_mutex_lock(&g->lock);
        npads = g->npads;
        pthread_mutex_unlock(&g->lock);
        /* ~60Hz with a pad attached; with none only hot-plug is waited for,
         * twice a second (in short slices, so a stop is not kept waiting).
         * SDL_Delay, never SDL_WaitEventTimeout (it polls at 1 kHz). */
        if (npads > 0) {
            SDL_Delay(16);
        } else {
            for (slice = 0; slice < 5 && !g->stop_requested; slice++)
                SDL_Delay(100);
        }
    }

    pthread_mutex_lock(&g->lock);
    for (pad = 0; pad < g->npads; pad++)
        slot_close(&g->pads[pad]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);

    return NULL;
}

int intv_gamepad_start(struct intvsession *s)
{
    gamepad_state *g;
    pthread_attr_t attr;

    if (g_state)
        return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "intv_gamepad: SDL_InitSubSystem failed: %s\n",
                SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    g->nsides = s && s->ecs_on ? 4 : 2;
    /* the generation carries across a restart, so a frontend's baseline never
     * goes stale; a restarted thread's first scan re-finds the same pads, which
     * is no hot-plug: no toast and no change of generation */
    g->generation = s ? s->pad_gen_base : s_bare_gen_base;
    g->quiet_scan = s ? s->pad_restarted : s_bare_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table.
     * Decoded for all 4 sides so an ECS-only entry survives a run without ECS;
     * restore_assignment keeps to the sides this run has. */
    intv_pad_table_decode(&g->table,
                          s ? intvsession_get_str(s, "pad_assign", "") : "",
                          NUM_SIDES);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g_state = g;
    return 0;
}

void intv_gamepad_stop(void)
{
    gamepad_state *g = g_state;
    if (!g)
        return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    if (g->session) {
        g->session->pad_gen_base = g->generation;
        g->session->pad_restarted = 1;
    } else {
        s_bare_gen_base = g->generation;
        s_bare_restarted = 1;
    }
    g_state = NULL;
    pthread_mutex_destroy(&g->lock);
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void intv_gamepad_forget_disc(void)
{
    gamepad_state *g = g_state;
    int pad;
    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    /* The sources were just cleared (session.c's reset), so the pads' discs
     * hold nothing: -1 is what is true now, and the very next poll sends any
     * stick or D-pad still held. */
    for (pad = 0; pad < g->npads; pad++) {
        g->pads[pad].last_pad_dir = -1;
        g->pads[pad].last_stick_dir = -1;
    }
    pthread_mutex_unlock(&g->lock);
}

/* ---- public API ---------------------------------------------------------- */

int intvsession_gamepad_count(intvsession *s)
{
    gamepad_state *g = g_state;
    int count;
    (void)s;
    if (!g)
        return 0;
    pthread_mutex_lock(&g->lock);
    count = g->npads;
    pthread_mutex_unlock(&g->lock);
    return count;
}

int intvsession_gamepad_name(intvsession *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = g_state;
    int len = 0;
    (void)s;
    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!g)
        return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        len = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return len >= dstsz ? dstsz - 1 : len;
}

void intvsession_gamepad_assign(intvsession *s, int idx, int side)
{
    gamepad_state *g = g_state;
    char text[INTV_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    struct intvsession *sess;

    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    sess = s ? s : g->session;
    if (idx >= 0 && idx < g->npads) {
        pad_slot *slot = &g->pads[idx];
        if (side >= 0 && side < g->nsides) {
            slot->assign = side;
            slot->by_name = 0;
            changed = intv_pad_table_set(&g->table, slot->guidstr, slot->name,
                                         side) > 0;
        } else {
            /* anything else is Automatic, and forgets the remembered side */
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int pad, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (pad = 0; pad < g->npads; pad++)
                if (pad != idx && g->pads[pad].assign >= 0) {
                    kguid[nkeep] = g->pads[pad].guidstr;
                    kname[nkeep++] = g->pads[pad].name;
                }
            changed = intv_pad_table_remove(&g->table, slot->guidstr,
                                            slot->name, nkeep, kguid, kname);
        }
        if (changed &&
            intv_pad_table_encode(&g->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g->lock);
    /* the settings are written here, on the caller's thread, with the pad lock
     * let go: the pad thread must never wait on the settings store */
    if (changed && sess)
        intvsession_set_str(sess, "pad_assign", text);
}

int intvsession_gamepad_assignment(intvsession *s, int idx)
{
    gamepad_state *g = g_state;
    int assign = -1;
    (void)s;
    if (!g)
        return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        assign = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return assign;
}

int intvsession_gamepad_effective_side(intvsession *s, int idx)
{
    gamepad_state *g = g_state;
    int side = -1;
    (void)s;
    if (!g)
        return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        side = effective_side_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return side;
}

unsigned intvsession_gamepad_generation(intvsession *s)
{
    gamepad_state *g = g_state;
    unsigned gen;
    if (!g)
        return s ? s->pad_gen_base : s_bare_gen_base;
    pthread_mutex_lock(&g->lock);
    gen = g->generation;
    pthread_mutex_unlock(&g->lock);
    return gen;
}

void intvsession_gamepad_capture_begin(intvsession *s)
{
    gamepad_state *g = g_state;
    (void)s;
    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1;
    g->cap_have = 0;
    g->cap_result = -1;
    pthread_mutex_unlock(&g->lock);
}

void intvsession_gamepad_capture_cancel(intvsession *s)
{
    gamepad_state *g = g_state;
    (void)s;
    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0;
    g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int intvsession_gamepad_capture_poll(intvsession *s,
                                     intvsession_pad_button *button)
{
    gamepad_state *g = g_state;
    int got = 0;
    (void)s;
    if (!g)
        return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button)
            *button = (intvsession_pad_button)g->cap_result;
        g->cap_have = 0;
        g->cap_active = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
