/*
 * input.c -- the hand controllers' input sources and their merge (see
 * input_merge.h), the public input API built on it (pointer, release, held
 * queries), and the held-key table of the ECS keyboard.
 *
 * The merged state is forwarded to jzIntv's pad_t through intv_host_pad_key /
 * intv_host_pad_disc under the input mutex (a leaf lock: nothing here takes
 * another), so the order in which sources changed is the order the machine
 * hears it. Held state is read back from the SOURCE table, never from pad_t:
 * a keypad press also grounds disc and button lines on real hardware.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>

#include "input_merge.h"
#include "intv_host.h"

#define NUM_KEYS INTVSESSION_KEY_COUNT

typedef struct {
    uint16_t keys[INTV_SRC_COUNT];   /* bit (1 << key) */
    int16_t  disc[INTV_SRC_COUNT];   /* -1 = none */
    uint32_t stamp[INTV_SRC_COUNT];  /* when the disc position was set */
} side_state;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static side_state s_side[INTV_SIDES];
static uint16_t s_fwd_keys[INTV_SIDES];
static int s_fwd_disc[INTV_SIDES] = { -1, -1, -1, -1 };
static uint32_t s_stamp;
static uint8_t s_ecs_down[INTVSESSION_ECS_KEY_NONE];
static int s_inited;

static void init_locked(void)
{
    int side, src;
    if (s_inited)
        return;
    for (side = 0; side < INTV_SIDES; side++)
        for (src = 0; src < INTV_SRC_COUNT; src++)
            s_side[side].disc[src] = -1;
    s_inited = 1;
}

/* Rank of a source for the disc: the pointer first, then a pad's buttons,
 * D-pad and hat, then a pad's stick, then the keyboard. */
static int rank(int src)
{
    if (src == INTV_SRC_POINTER) return 4;
    if (src >= INTV_SRC_PAD0 && src < INTV_SRC_STICK0) return 3;
    if (src >= INTV_SRC_STICK0) return 2;
    return 1;
}

static int disc_merged_locked(int side)
{
    const side_state *st = &s_side[side];
    int best = -1, src, best_rank = 0;
    uint32_t best_stamp = 0;

    for (src = 0; src < INTV_SRC_COUNT; src++) {
        const int rnk = rank(src);
        if (st->disc[src] < 0)
            continue;
        if (best < 0 || rnk > best_rank ||
            (rnk == best_rank && st->stamp[src] > best_stamp)) {
            best = src;
            best_rank = rnk;
            best_stamp = st->stamp[src];
        }
    }
    return best < 0 ? -1 : st->disc[best];
}

static unsigned keys_merged_locked(int side)
{
    unsigned bits = 0;
    int src;
    for (src = 0; src < INTV_SRC_COUNT; src++)
        bits |= s_side[side].keys[src];
    return bits;
}

/* Tell the machine what changed. input mutex held. */
static void forward_locked(void)
{
    int side, key;
    for (side = 0; side < INTV_SIDES; side++) {
        const unsigned now = keys_merged_locked(side);
        const unsigned diff = now ^ s_fwd_keys[side];
        const int disc = disc_merged_locked(side);
        for (key = 0; key < NUM_KEYS; key++)
            if (diff & (1u << key))
                intv_host_pad_key((intv_pad_side)side, (intv_pad_key)key,
                                  (now >> key) & 1u);
        s_fwd_keys[side] = (uint16_t)now;
        if (disc != s_fwd_disc[side]) {
            intv_host_pad_disc((intv_pad_side)side, disc);
            s_fwd_disc[side] = disc;
        }
    }
}

static int valid(int src, int side)
{
    return src >= 0 && src < INTV_SRC_COUNT && side >= 0 && side < INTV_SIDES;
}

void input_key(int src, int side, int key, int down)
{
    if (!valid(src, side) || key < 0 || key >= NUM_KEYS)
        return;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    if (down)
        s_side[side].keys[src] |= (uint16_t)(1u << key);
    else
        s_side[side].keys[src] &= (uint16_t)~(1u << key);
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

void input_keys_set(int src, int side, unsigned mask)
{
    if (!valid(src, side))
        return;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    s_side[side].keys[src] = (uint16_t)(mask & ((1u << NUM_KEYS) - 1));
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

void input_disc(int src, int side, int dir)
{
    if (!valid(src, side))
        return;
    if (dir < 0 || dir >= INTVSESSION_DISC_POSITIONS)
        dir = -1;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    s_side[side].disc[src] = (int16_t)dir;
    if (dir >= 0)
        s_side[side].stamp[src] = ++s_stamp;
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

static void clear_source_locked(int src)
{
    int side;
    for (side = 0; side < INTV_SIDES; side++) {
        s_side[side].keys[src] = 0;
        s_side[side].disc[src] = -1;
    }
}

void input_clear_source(int src)
{
    if (src < 0 || src >= INTV_SRC_COUNT)
        return;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    clear_source_locked(src);
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

void input_pad_clear(int owner)
{
    if (owner < 0 || owner >= INTV_MAX_PADS)
        return;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    clear_source_locked(INTV_SRC_PAD(owner));
    clear_source_locked(INTV_SRC_STICK(owner));
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

void input_clear_all(void)
{
    int src, side;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    for (src = 0; src < INTV_SRC_COUNT; src++)
        clear_source_locked(src);
    /* The machine's pads were zeroed under us (or are about to be), so what we
     * think we forwarded is stale: zero them and re-forward from scratch. */
    intv_host_pads_clear();
    /* A reset or a rebuilt machine also has an empty ECS matrix, so the held
     * keys we remember (latched SHIFT/CTL) are stale too. */
    memset(s_ecs_down, 0, sizeof s_ecs_down);
    for (side = 0; side < INTV_SIDES; side++) {
        s_fwd_keys[side] = 0;
        s_fwd_disc[side] = -1;
    }
    pthread_mutex_unlock(&s_mtx);
}

unsigned input_keys_merged(int side)
{
    unsigned bits;
    if (side < 0 || side >= INTV_SIDES)
        return 0;
    pthread_mutex_lock(&s_mtx);
    bits = keys_merged_locked(side);
    pthread_mutex_unlock(&s_mtx);
    return bits;
}

int input_disc_merged(int side)
{
    int dir;
    if (side < 0 || side >= INTV_SIDES)
        return -1;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    dir = disc_merged_locked(side);
    pthread_mutex_unlock(&s_mtx);
    return dir;
}

/* ---- the ECS keyboard's held keys ---------------------------------------- */

void input_ecs_set(int key, int down)
{
    if (key < 0 || key >= INTVSESSION_ECS_KEY_NONE)
        return;
    pthread_mutex_lock(&s_mtx);
    s_ecs_down[key] = down ? 1 : 0;
    intv_host_ecs_key((intv_ecs_key)key, down);
    pthread_mutex_unlock(&s_mtx);
}

int input_ecs_held(int key)
{
    int held;
    if (key < 0 || key >= INTVSESSION_ECS_KEY_NONE)
        return 0;
    pthread_mutex_lock(&s_mtx);
    held = s_ecs_down[key];
    pthread_mutex_unlock(&s_mtx);
    return held;
}

void input_ecs_clear(void)
{
    pthread_mutex_lock(&s_mtx);
    memset(s_ecs_down, 0, sizeof s_ecs_down);
    intv_host_ecs_keys_clear();
    pthread_mutex_unlock(&s_mtx);
}

/* ---- public API: the keyboard source, the pointer, release, held ------------ */

void intvsession_pad_key(intvsession *s, intvsession_pad_side side,
                         intvsession_key key, int pressed)
{
    (void)s;
    input_key(INTV_SRC_KEYBOARD, (int)side, (int)key, pressed);
}

void intvsession_pad_disc(intvsession *s, intvsession_pad_side side,
                          int direction)
{
    (void)s;
    input_disc(INTV_SRC_KEYBOARD, (int)side, direction);
}

void intvsession_pointer_set(intvsession *s, intvsession_pad_side side,
                             int key, int disc)
{
    int other;
    (void)s;
    if (side < 0 || side >= INTV_SIDES)
        return;
    pthread_mutex_lock(&s_mtx);
    init_locked();
    /* the pointer is one gesture: what it held on any other side is let go */
    for (other = 0; other < INTV_SIDES; other++) {
        s_side[other].keys[INTV_SRC_POINTER] = 0;
        s_side[other].disc[INTV_SRC_POINTER] = -1;
    }
    if (key >= 0 && key < NUM_KEYS)
        s_side[side].keys[INTV_SRC_POINTER] = (uint16_t)(1u << key);
    if (disc >= 0 && disc < INTVSESSION_DISC_POSITIONS) {
        s_side[side].disc[INTV_SRC_POINTER] = (int16_t)disc;
        s_side[side].stamp[INTV_SRC_POINTER] = ++s_stamp;
    }
    forward_locked();
    pthread_mutex_unlock(&s_mtx);
}

void intvsession_release_pointer(intvsession *s)
{
    (void)s;
    input_clear_source(INTV_SRC_POINTER);
}

void intvsession_release_keyboard(intvsession *s)
{
    (void)s;
    input_clear_source(INTV_SRC_KEYBOARD);
}

/* Focus loss: the keyboard's and the pointer's holds. The pads' stay: each
 * knows when it lets go. */
void intvsession_release_all(intvsession *s)
{
    intvsession_release_keyboard(s);
    intvsession_release_pointer(s);
}

void intvsession_clear_held(intvsession *s)
{
    intvsession_release_all(s);
}

void intvsession_pads_clear(intvsession *s)
{
    intvsession_release_all(s);
}

unsigned intvsession_buttons_held(intvsession *s, intvsession_pad_side side)
{
    (void)s;
    return input_keys_merged((int)side);
}

int intvsession_disc_held(intvsession *s, intvsession_pad_side side)
{
    (void)s;
    return input_disc_merged((int)side);
}

int intvsession_target_held(intvsession *s, intvsession_key_mapping target)
{
    if (target.kind == INTVSESSION_MAP_KEY) {
        if (target.key < 0 || target.key >= NUM_KEYS)
            return 0;
        return (intvsession_buttons_held(s, target.side) >> target.key) & 1u;
    }
    if (target.kind == INTVSESSION_MAP_DISC)
        return target.direction >= 0 &&
               intvsession_disc_held(s, target.side) == target.direction;
    return 0;
}
