/*
 * intv_frame -- see intv_frame.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "intv_frame.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_pixels[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
static uint64_t s_serial = 0;
static void (*s_publish_hook)(void *ctx) = NULL;
static void *s_publish_hook_ctx = NULL;

void intv_frame_set_publish_hook(void (*hook)(void *ctx), void *ctx)
{
    s_publish_hook = hook;
    s_publish_hook_ctx = ctx;
}

void intv_frame_publish(const uint8_t *vid, const uint8_t palette[16][3],
                        int vid_enabled)
{
    uint32_t local[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
    const int shift = vid_enabled ? 0 : 1; /* halve RGB when blanked, like
                                              * gfx_sdl2.c's pal_off. */

    for (int i = 0; i < INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT; i++)
    {
        const uint8_t idx = vid[i] & 0x0F;
        const uint8_t r = palette[idx][0] >> shift;
        const uint8_t g = palette[idx][1] >> shift;
        const uint8_t b = palette[idx][2] >> shift;
        local[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    pthread_mutex_lock(&s_lock);
    memcpy(s_pixels, local, sizeof(s_pixels));
    s_serial++;
    pthread_mutex_unlock(&s_lock);

    if (s_publish_hook)
        s_publish_hook(s_publish_hook_ctx);
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
