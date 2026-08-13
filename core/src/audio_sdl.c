/*
 * intvsession audio backend: SDL3 audio device pulling mixed emulator +
 * FujiNet samples. SDL routes through PipeWire/Pulse on modern Linux and
 * ports unchanged to the future Mac/Windows frontends. Only the audio
 * subsystem is initialized here -- never SDL video, which would fight the
 * GTK/Qt display stack. Modelled directly on the CoCo port's own
 * audio_sdl.c, mono instead of stereo (see intv_audio.h's own comment on
 * why: jzIntv's PSG mixer has no stereo signal to fake).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "intv_audio.h"
#include "session_internal.h"

typedef struct {
    SDL_AudioStream *stream;
} audio_state;

/* Pull model: the device asks for more and we hand back exactly that much.
 * intvsession_render_audio drains intv_audio's ring (filled by
 * snd_desktop.c as each buffer completes on the emulator thread) and
 * overlays FujiNet's own mixed audio, and is safe to call from the audio
 * thread. Mono throughout -- one sample per frame, unlike CoCo's
 * interleaved-stereo counterpart. */
static void audio_cb(void *ud, SDL_AudioStream *stream, int additional_amount,
                     int total_amount)
{
    struct intvsession *s = ud;
    int16_t buf[2048];
    (void)total_amount;

    while (additional_amount > 0) {
        int want_bytes = additional_amount > (int)sizeof(buf)
                             ? (int)sizeof(buf)
                             : additional_amount;
        int want_samples = want_bytes / 2;
        /* intvsession_render_audio only fills as many samples as the ring
         * actually has and leaves the rest of buf untouched -- pad the
         * shortfall with silence rather than handing the device whatever
         * was left over from a previous callback (or, on the very first
         * callback, uninitialised stack memory). Without this, any
         * underrun played back as a buzz/click instead of a gap. */
        int n = intvsession_render_audio(s, buf, want_samples);
        if (n < want_samples)
            SDL_memset(buf + n, 0,
                      (size_t)(want_samples - n) * sizeof(buf[0]));
        SDL_PutAudioStreamData(stream, buf, want_samples * 2);
        additional_amount -= want_samples * 2;
    }
}

int audio_start(struct intvsession *s)
{
    audio_state *a;
    SDL_AudioSpec spec;

    if (s->audio)
        return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        session_set_error(s, "SDL audio init failed: %s", SDL_GetError());
        return -1;
    }

    a = calloc(1, sizeof(*a));
    if (!a)
        return -1;

    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = INTVSESSION_AUDIO_RATE;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_cb, s);
    if (!a->stream) {
        session_set_error(s, "SDL audio device open failed: %s",
                          SDL_GetError());
        free(a);
        return -1;
    }
    /* Discard anything left over from a previous session (e.g. a
     * Settings-driven restart) so playback doesn't open with up to half a
     * second of stale audio replaying from the ring. */
    intv_audio_reset();
    SDL_ResumeAudioStreamDevice(a->stream);
    s->audio = a;
    return 0;
}

void audio_stop(struct intvsession *s)
{
    audio_state *a = s->audio;
    if (!a)
        return;
    s->audio = NULL;
    SDL_DestroyAudioStream(a->stream); /* also closes the bound device */
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
