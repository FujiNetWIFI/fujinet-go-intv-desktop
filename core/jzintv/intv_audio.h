/*
 * intv_audio -- the audio ring buffer the desktop snd backend publishes
 * into and frontends drain from.
 *
 * jzIntv's own PSG mixer (src/snd/snd.h) produces a single mono int16
 * stream -- there is no stereo signal anywhere in the pipeline to fake, so
 * this stays mono rather than inventing a channel. Sample rate is whatever
 * intv_host started the machine with (INTV_AUDIO_RATE -- see that macro's
 * own comment for why it is fixed rather than queried at runtime).
 *
 * Thread safety: intv_audio_publish is called from the emulator thread
 * (inside snd_tick, once per mixed buffer -- see
 * core/jzintv/desktop/snd_desktop.c); intv_audio_copy from any consumer
 * thread. A mutex-protected circular buffer; if the consumer falls behind,
 * the oldest unread samples are silently dropped rather than blocking the
 * emulator thread or growing without bound -- the same tradeoff a real
 * sound card's DMA ring makes.
 *
 * LATENCY: intv_audio_copy also self-corrects lag on every call, not just
 * on overflow. If more than RING_HIGHWATER_SAMPLES (a fixed ~100ms) is
 * sitting unread, the consumer has been running behind for a while (not
 * stalled outright -- that overflow case is handled by intv_audio_publish
 * above), so the ring is fast-forwarded down to RING_TARGET_SAMPLES
 * (~30ms) before copying, rather than left to keep growing toward the
 * ring's full half-second capacity.
 *
 * This threshold is deliberately an ABSOLUTE sample count, not scaled by
 * the caller's own max_samples: the ring is fed by the emulator in fixed
 * bursts (snd_desktop.c publishes SND_BUF_SIZE_DEFAULT samples at a time,
 * independent of anything a consumer asks for), so a request-relative
 * threshold fires or doesn't purely based on how the consumer's chunk size
 * happens to compare to the producer's -- on a host whose audio device
 * quantum is smaller than one producer burst, a relative threshold trimmed
 * the ring after every single publish, discarding most of the emulator's
 * output and starving the consumer between bursts. Fixed sample counts
 * derived from INTV_AUDIO_RATE make the trim fire only on genuine
 * sustained lag, regardless of either side's chunk size.
 *
 * PRIMING: intv_audio_copy also withholds samples until the ring has
 * accumulated RING_TARGET_SAMPLES at least once (and again after it fully
 * drains), rather than trickling out whatever's there as soon as the first
 * few samples land. Without this, bursty production (the emulator
 * publishes in chunks, not continuously) would make the very first
 * requests after a publish under-fill, one micro-gap per burst instead of
 * a single clean gap at startup.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_AUDIO_H
#define INTV_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches config.h's DEFAULT_AUDIO_HZ on every desktop platform (Linux,
 * macOS, Windows all define it as 48000 -- see the staged tree's
 * src/config.h). intv_host.c does not override it with -a, so this is what
 * snd_init() actually uses; asserted against at runtime in snd_desktop.c
 * rather than trusted blindly, since a config.h change upstream would
 * otherwise desync this silently. */
#define INTV_AUDIO_RATE 48000

/* Emulator thread: append `count` mono samples. Drops the oldest buffered
 * samples first if the ring is full. */
void intv_audio_publish(const int16_t *samples, int count);

/* Consumer thread: copies up to max_samples into dst (mono int16), removing
 * them from the ring. Returns the number actually copied (0 if the ring was
 * empty, or not yet primed -- see this file's own PRIMING note -- not an
 * error, just means nothing new has played yet). */
int intv_audio_copy(int16_t *dst, int max_samples);

/* Empties the ring and clears the prime gate. Call when (re)starting
 * playback (e.g. audio_start) so a fresh session never plays samples left
 * over from a previous one. Safe to call from any thread. */
void intv_audio_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* INTV_AUDIO_H */
