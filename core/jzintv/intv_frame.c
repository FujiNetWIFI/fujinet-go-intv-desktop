/*
 * intv_frame -- see intv_frame.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "intv_frame.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_pixels[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
static uint64_t s_serial = 0;
static void (*s_publish_hook)(void *ctx) = NULL;
static void *s_publish_hook_ctx = NULL;

/* What the last published frame was made from, so an identical one can be
 * recognised without expanding it: the machine runs at 60 Hz whatever it
 * shows, and most of what it shows (CONFIG, a menu) does not move. */
static uint8_t s_last_vid[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
static uint8_t s_last_pal[16][3];
static int s_last_shift = 0;
static int s_valid = 0;
static atomic_ullong s_count = 0;

/* New-frame callback, held across the call so set(NULL) waits for one in
 * flight. Separate from s_lock: frame readers never wait on it. */
static pthread_mutex_t s_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*s_frame_cb)(void *ctx) = NULL;
static void *s_frame_cb_ctx = NULL;

void intv_frame_set_frame_callback(void (*cb)(void *ctx), void *ctx)
{
    pthread_mutex_lock(&s_cb_lock);
    s_frame_cb = cb;
    s_frame_cb_ctx = ctx;
    pthread_mutex_unlock(&s_cb_lock);
}

uint64_t intv_frame_count(void)
{
    return atomic_load(&s_count);
}

void intv_frame_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_valid = 0;
    pthread_mutex_unlock(&s_lock);
    atomic_store(&s_count, 0);
}

void intv_frame_set_publish_hook(void (*hook)(void *ctx), void *ctx)
{
    s_publish_hook = hook;
    s_publish_hook_ctx = ctx;
}

void intv_frame_publish(const uint8_t *vid, const uint8_t palette[16][3],
                        int vid_enabled)
{
    const int shift = vid_enabled ? 0 : 1; /* halve RGB when blanked, like
                                              * gfx_sdl2.c's pal_off. */

    /* Expanded straight into s_pixels under the lock. It used to go into a
     * 128,000-byte stack `local[]` first and then be memcpy'd in, which cost
     * the emulator thread a second full-frame write and a 128 KB stack frame
     * (plus MinGW's __chkstk_ms probing it) on every single frame, all to
     * shorten a lock nobody contends: the only other holder is
     * intv_frame_copy, once per displayed frame, and it does exactly one
     * memcpy of its own. A reader still cannot observe a partial frame,
     * which is the property that actually matters here. */
    int fresh = 1;

    atomic_fetch_add(&s_count, 1);
    pthread_mutex_lock(&s_lock);
    if (s_valid && s_last_shift == shift &&
        memcmp(s_last_vid, vid, sizeof(s_last_vid)) == 0 &&
        memcmp(s_last_pal, palette, sizeof(s_last_pal)) == 0)
    {
        fresh = 0;   /* same picture: the serial stays, nobody is called */
    }
    else
    {
        memcpy(s_last_vid, vid, sizeof(s_last_vid));
        memcpy(s_last_pal, palette, sizeof(s_last_pal));
        s_last_shift = shift;
        s_valid = 1;
    }
    for (int i = 0; fresh && i < INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT; i++)
    {
        const uint8_t idx = vid[i] & 0x0F;
        const uint8_t r = palette[idx][0] >> shift;
        const uint8_t g = palette[idx][1] >> shift;
        const uint8_t b = palette[idx][2] >> shift;
        s_pixels[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    if (fresh)
        s_serial++;
    pthread_mutex_unlock(&s_lock);

    /* The Android presenter blocks on this once per frame, changed or not. */
    if (s_publish_hook)
        s_publish_hook(s_publish_hook_ctx);

    if (fresh)
    {
        pthread_mutex_lock(&s_cb_lock);
        if (s_frame_cb)
            s_frame_cb(s_frame_cb_ctx);
        pthread_mutex_unlock(&s_cb_lock);
    }
}

int intv_frame_copy(uint32_t *dst, uint64_t *serial_inout)
{
    int changed;

    pthread_mutex_lock(&s_lock);
    changed = (*serial_inout != s_serial);
    if (changed)
    {
        memcpy(dst, s_pixels, sizeof(s_pixels));
        *serial_inout = s_serial;
    }
    pthread_mutex_unlock(&s_lock);

    return changed;
}
