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

/* See intv_audio.h's LATENCY/PRIMING notes: these are fixed sample counts,
 * not scaled by any caller's request size, so the trim's behaviour doesn't
 * depend on how the consumer's chunk size happens to compare to the
 * producer's. ~30ms / ~100ms at INTV_AUDIO_RATE. */
#define RING_TARGET_SAMPLES    (INTV_AUDIO_RATE / 33)
#define RING_HIGHWATER_SAMPLES (INTV_AUDIO_RATE / 10)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t s_ring[RING_CAPACITY];
static int s_head = 0;    /* next write position */
static int s_count = 0;   /* samples currently buffered, <= RING_CAPACITY */
static int s_primed = 0;  /* has the ring reached RING_TARGET_SAMPLES since
                            * the last time it fully drained? */

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

    /* Catch up on lag before copying anything: if more than
     * RING_HIGHWATER_SAMPLES is backlogged, the consumer has been running
     * behind for a while (not stalled outright -- that overflow case is
     * handled in intv_audio_publish above). Trim down to
     * RING_TARGET_SAMPLES so what's left is a small, bounded cushion of
     * recent audio rather than a growing backlog -- see this file's
     * header for why this must be a fixed threshold, not scaled by
     * max_samples. */
    if (s_count > RING_HIGHWATER_SAMPLES)
        s_count = RING_TARGET_SAMPLES;

    /* Prime gate: don't start handing out samples until at least one
     * target's worth has accumulated, so a burst of production doesn't
     * dribble out as several under-filled requests (one micro-gap per
     * emulator publish) instead of a single clean gap. Once primed, stay
     * primed until the ring runs completely dry. */
    if (!s_primed)
    {
        if (s_count < RING_TARGET_SAMPLES)
        {
            pthread_mutex_unlock(&s_lock);
            return 0;
        }
        s_primed = 1;
    }

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
    if (s_count == 0)
        s_primed = 0;
    pthread_mutex_unlock(&s_lock);
    return n;
}

void intv_audio_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_head = 0;
    s_count = 0;
    s_primed = 0;
    pthread_mutex_unlock(&s_lock);
}
