/*
 * intvsession -- see intvsession.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "gamepad_sdl.h"
#include "intv_audio.h"
#include "intv_frame.h"
#include "intv_host.h"
#include "session_internal.h"

/* Both fixed at 48000 (config.h's DEFAULT_AUDIO_HZ on every desktop
 * platform); a mismatch would mean the two headers have drifted apart. */
#if INTVSESSION_AUDIO_RATE != INTV_AUDIO_RATE
#error "INTVSESSION_AUDIO_RATE and INTV_AUDIO_RATE must match"
#endif

intvsession *intvsession_new(const intvsession_paths *paths)
{
    intvsession *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        free(s);
        return NULL;
    }
    settings_init(s);
    s->fujinet_port = INTVSESSION_BOIP_PORT;
    return s;
}

void intvsession_free(intvsession *s)
{
    if (!s)
        return;
    intvsession_stop(s);
    settings_free_all(s);
    free(s);
}

/* HW_AUTO/OFF/ON -> INTV_HW_AUTO/OFF/ON (-1/0/1); jzIntv's own tri-state
 * encoding, see intv_host.h. */
static int hw_to_intv(int hw)
{
    switch (hw) {
    case INTVSESSION_HW_OFF: return INTV_HW_OFF;
    case INTVSESSION_HW_ON:  return INTV_HW_ON;
    default:                return INTV_HW_AUTO;
    }
}

void intvsession_default_opts(intvsession *s, intvsession_start_opts *opts)
{
    opts->ecs = intvsession_get_int(s, "ecs", INTVSESSION_HW_AUTO);
    opts->ivoice = intvsession_get_int(s, "ivoice", INTVSESSION_HW_AUTO);
    opts->video = intvsession_get_int(s, "video_standard",
                                      INTVSESSION_VIDEO_NTSC);
}

static const char *const hw_mode_names[] = { "Auto", "Off", "On", NULL };

const char *intvsession_hw_mode_name(int idx)
{
    if (idx < 0 || (size_t)idx >= sizeof(hw_mode_names) / sizeof(hw_mode_names[0]) - 1)
        return NULL;
    return hw_mode_names[idx];
}

static const char *const video_names[] = { "NTSC (60 Hz)", "PAL (50 Hz)", NULL };

const char *intvsession_video_name(int idx)
{
    if (idx < 0 || (size_t)idx >= sizeof(video_names) / sizeof(video_names[0]) - 1)
        return NULL;
    return video_names[idx];
}

int intvsession_has_ecs_rom(const intvsession *s)
{
    return intv_host_has_ecs_rom(s->roms_dir);
}

int intvsession_start(intvsession *s, const intvsession_start_opts *opts)
{
    intvsession_start_opts defaults;
    if (!opts) {
        intvsession_default_opts(s, &defaults);
        opts = &defaults;
    }

    if (!intvsession_has_system_roms(s)) {
        session_set_error(s, "%s is missing exec.bin/grom.bin -- import "
                          "system ROMs, or build -DWITH_INTV_ROMS=ON for "
                          "local testing", s->roms_dir);
        return -1;
    }

    if (opts->ecs == INTVSESSION_HW_ON && !intvsession_has_ecs_rom(s)) {
        session_set_error(s, "ECS is enabled but %s/ecs.bin is missing or "
                          "the wrong size -- import an ECS ROM, or turn ECS "
                          "off in Settings", s->roms_dir);
        return -1;
    }

    /* FujiNet listens, jzIntv's --fujinet connects out (see
     * intvsession.h's own comment on this direction) -- so FujiNet has to
     * be up, and its listener actually accepting, before the emulator
     * thread starts. Best-effort: WITH_FUJINET=OFF or a load failure is not
     * a session-start failure, the machine still boots the embedded config
     * ROM either way (see intv_host.h). */
    if (fujinet_start(s) == 0)
        fujinet_wait_for_boip(s, 3000);

    intv_host_opts host_opts = {
        .rom_dir = s->roms_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = s->fujinet_port,
        .ecs = hw_to_intv(opts->ecs),
        .ivoice = hw_to_intv(opts->ivoice),
        .pal = opts->video == INTVSESSION_VIDEO_PAL,
    };
    if (intv_host_start(&host_opts) != 0) {
        session_set_error(s, "failed to start the emulator thread");
        fujinet_stop(s);
        return -1;
    }
    /* Best-effort: no gamepads attached is not a session-start failure, and
     * neither is no audio device (session_set_error still records why, for
     * a frontend that wants to surface it, but silence over sound is far
     * less disruptive than refusing to boot). */
    intv_gamepad_start();
    audio_start(s);
    return 0;
}

void intvsession_stop(intvsession *s)
{
    intv_gamepad_stop();
    audio_stop(s);
    intv_host_stop();
    fujinet_stop(s);
}

int intvsession_is_running(const intvsession *s)
{
    (void)s;
    return intv_host_is_running();
}

const char *intvsession_last_error(const intvsession *s)
{
    return s->last_error;
}

int intvsession_copy_frame(intvsession *s, uint32_t *dst,
                           uint64_t *serial_inout)
{
    (void)s;
    return intv_frame_copy(dst, serial_inout);
}

int intvsession_render_audio(intvsession *s, int16_t *dst, int max_samples)
{
    int n = intv_audio_copy(dst, max_samples);
    if (n > 0)
        fujinet_mix_audio(s, dst, n, INTVSESSION_AUDIO_RATE);
    return n;
}

void intvsession_pad_key(intvsession *s, intvsession_pad_side side,
                         intvsession_key key, int pressed)
{
    (void)s;
    intv_host_pad_key((intv_pad_side)side, (intv_pad_key)key, pressed);
}

void intvsession_pad_disc(intvsession *s, intvsession_pad_side side,
                          int direction)
{
    (void)s;
    intv_host_pad_disc((intv_pad_side)side, direction);
}

void intvsession_ecs_key_set(intvsession *s, intvsession_ecs_key key,
                             int pressed)
{
    (void)s;
    if (key < 0 || key >= INTVSESSION_ECS_KEY_NONE)
        return;
    intv_host_ecs_key((intv_ecs_key)key, pressed);
}

void intvsession_ecs_keys_clear(intvsession *s)
{
    (void)s;
    intv_host_ecs_keys_clear();
}
