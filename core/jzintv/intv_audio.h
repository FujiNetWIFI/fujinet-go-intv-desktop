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
 * empty -- not an error, just means nothing new has played yet). */
int intv_audio_copy(int16_t *dst, int max_samples);

#ifdef __cplusplus
}
#endif

#endif /* INTV_AUDIO_H */
