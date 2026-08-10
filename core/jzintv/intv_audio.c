/*
 * intv_audio -- see intv_audio.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "intv_audio.h"

#include <pthread.h>

/* ~0.5s at INTV_AUDIO_RATE: generous enough that a UI thread polling once
 * per video frame (~16ms) never starves, small enough that a consumer which
 * stops draining altogether does not accumulate unbounded latency -- it
 * just loses old audio, same as intv_frame's serial-based "latest wins". */
#define RING_CAPACITY (INTV_AUDIO_RATE / 2)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t s_ring[RING_CAPACITY];
static int s_head = 0;   /* next write position */
static int s_count = 0;  /* samples currently buffered, <= RING_CAPACITY */

void intv_audio_publish(const int16_t *samples, int count)
{
    if (count <= 0)
        return;
    if (count > RING_CAPACITY)
    {
        /* Larger than the whole ring: only the tail is reachable anyway. */
        samples += (count - RING_CAPACITY);
        count = RING_CAPACITY;
    }

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < count; i++)
    {
        s_ring[s_head] = samples[i];
        s_head = (s_head + 1) % RING_CAPACITY;
    }
    s_count += count;
    if (s_count > RING_CAPACITY)
        s_count = RING_CAPACITY; /* oldest samples were overwritten above */
    pthread_mutex_unlock(&s_lock);
}

int intv_audio_copy(int16_t *dst, int max_samples)
{
    pthread_mutex_lock(&s_lock);
    const int n = (max_samples < s_count) ? max_samples : s_count;
    /* Oldest buffered sample is (s_head - s_count), wrapped. */
    int read_pos = ((s_head - s_count) % RING_CAPACITY + RING_CAPACITY)
                   % RING_CAPACITY;
    for (int i = 0; i < n; i++)
    {
        dst[i] = s_ring[read_pos];
        read_pos = (read_pos + 1) % RING_CAPACITY;
    }
    s_count -= n;
    pthread_mutex_unlock(&s_lock);
    return n;
}
