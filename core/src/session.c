/*
 * intvsession -- see intvsession.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include <pthread.h>

#include "bindings.h"
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
    /* Reloads the remappable-bindings table from this session's own
     * settings store -- process-global (like gamepad_sdl.c's own pad
     * table), but every intvsession_new should still see whatever this
     * particular config dir has persisted, matching session_test.c's own
     * "re-open sees the persisted setting" expectation for everything else
     * in the store. */
    bindings_init(s);
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
    opts->cart_path = intvsession_get_str(s, "cart", NULL);
    if (opts->cart_path && !opts->cart_path[0])
        opts->cart_path = NULL;
}

const char *intvsession_cart_path(intvsession *s)
{
    const char *path = intvsession_get_str(s, "cart", NULL);
    return (path && path[0]) ? path : "";
}

int intvsession_load_cart(intvsession *s, const char *path)
{
    intvsession_stop(s);
    intvsession_set_str(s, "cart", path && path[0] ? path : "");
    intvsession_settings_flush(s);

    intvsession_start_opts opts;
    intvsession_default_opts(s, &opts);
    return intvsession_start(s, &opts);
}

int intvsession_reset_to_config(intvsession *s)
{
    return intvsession_load_cart(s, NULL);
}

int intvsession_reset_game(intvsession *s)
{
    (void)s;
    /* gamepad_sdl.c's own last_disc[] cache has to be told first: it lives
     * in the SDL-linked half of this port (jzintv_core itself stays
     * SDL-free, see no_sdl_link_test), so this is the one place that can
     * call both intv_gamepad_forget_disc and intv_host_reset -- see the
     * former's own comment on why a stick/D-pad held across the reset
     * would otherwise leave the disc reading stuck centered. */
    intv_gamepad_forget_disc();
    intv_host_reset();
    return 0;
}

int intvsession_sysaction_fire(intvsession *s, intvsession_sysaction a)
{
    switch (a) {
    case INTVSESSION_SYSACT_RESET_GAME:   return intvsession_reset_game(s);
    case INTVSESSION_SYSACT_RESET_CONFIG: return intvsession_reset_to_config(s);
    default:                             return -1;
    }
}

/* ---- system-action latch -------------------------------------------------
 * See intvsession_sysaction_post's own comment in intvsession.h: the one
 * caller that can't fire directly is gamepad_sdl.c's SDL thread, for
 * RESET_CONFIG. A bitmask, not a single pending intvsession_sysaction --
 * INTVSESSION_SYSACT_RESET_GAME is 0, indistinguishable from "nothing
 * pending" if this were a bare enum, and two distinct actions posted before
 * a drain must both survive, not collapse into one. Process-global like
 * bindings.c's own table and gamepad_sdl.c's pad table, for the same
 * reason: jzIntv's machine is a process singleton. */
static pthread_mutex_t s_sysact_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_sysact_pending;

void intvsession_sysaction_post(intvsession *s, intvsession_sysaction a)
{
    (void)s;
    if (a < 0 || a >= INTVSESSION_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&s_sysact_mtx);
    s_sysact_pending |= (1u << (unsigned)a);
    pthread_mutex_unlock(&s_sysact_mtx);
}

int intvsession_sysaction_take(intvsession *s, intvsession_sysaction *out)
{
    int a;
    (void)s;
    pthread_mutex_lock(&s_sysact_mtx);
    if (!s_sysact_pending) {
        pthread_mutex_unlock(&s_sysact_mtx);
        return 0;
    }
    /* Lowest set bit -- which of the two, if both are pending, is
     * arbitrary; both are delivered, one per _take call, not coalesced. */
    for (a = 0; a < INTVSESSION_SYSACT_COUNT; a++) {
        if (s_sysact_pending & (1u << (unsigned)a)) {
            s_sysact_pending &= ~(1u << (unsigned)a);
            break;
        }
    }
    pthread_mutex_unlock(&s_sysact_mtx);
    if (out)
        *out = (intvsession_sysaction)a;
    return 1;
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
        .state_dir = s->data_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = s->fujinet_port,
        .ecs = hw_to_intv(opts->ecs),
        .ivoice = hw_to_intv(opts->ivoice),
        .pal = opts->video == INTVSESSION_VIDEO_PAL,
        .cart_path = opts->cart_path,
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
    /* The held-key table has to go with the matrix, not just alongside it:
     * a stale entry would make the NEXT release of that host key clear an
     * ECS key nobody pressed. */
    if (s)
        memset(s->ecs_held, 0, sizeof(s->ecs_held));
    intv_host_ecs_keys_clear();
}

/* See intvsession.h for the contract: why the release cannot simply
 * re-resolve the event it is releasing, and why the table is keyed on
 * host_key rather than on the keysym. */
void intvsession_ecs_key_event(intvsession *s, uint32_t host_key,
                               uint32_t keysym, uint32_t ch, int down)
{
    intvsession_ecs_key key;
    int i, slots, free_slot = -1;

    if (!s)
        return;

    slots = (int)(sizeof(s->ecs_held) / sizeof(s->ecs_held[0]));

    /* A frontend with no stable physical-key id falls back to the keysym --
     * exactly right for any key with a curated symbol (those never move
     * under Shift), and no worse than resolving afresh for the rest. */
    if (!host_key)
        host_key = keysym;
    if (!host_key)
        return;

    if (!down)
    {
        for (i = 0; i < slots; i++)
            if (s->ecs_held[i].host_key == host_key)
            {
                intvsession_ecs_key_set(
                    s, (intvsession_ecs_key)s->ecs_held[i].key, 0);
                s->ecs_held[i].host_key = 0;
                return;
            }
        /* No record: either the press resolved to nothing, or it happened
         * before an intvsession_ecs_keys_clear (a focus loss, a mode
         * toggle). Nothing to release either way -- and deliberately NOT a
         * re-resolve, which is the bug this table exists to prevent. */
        return;
    }

    key = intvsession_ecs_key_from_char(keysym, ch);
    if (key == INTVSESSION_ECS_KEY_NONE)
        return;

    for (i = 0; i < slots; i++)
    {
        if (s->ecs_held[i].host_key == host_key)
        {
            /* Auto-repeat (GTK and Win32 both deliver it; Qt and AppKit
             * drop it before this point), or a press whose release was
             * swallowed. Reusing the slot keeps a held key from filling the
             * table -- and releasing the old ECS key first matters when the
             * character CHANGED mid-hold, e.g. Shift pressed after the key
             * with the toolkit still repeating it: without this the old
             * key's bit would stay down for good. */
            if (s->ecs_held[i].key != (uint16_t)key)
                intvsession_ecs_key_set(
                    s, (intvsession_ecs_key)s->ecs_held[i].key, 0);
            s->ecs_held[i].key = (uint16_t)key;
            intvsession_ecs_key_set(s, key, 1);
            return;
        }
        if (free_slot < 0 && s->ecs_held[i].host_key == 0)
            free_slot = i;
    }

    /* Table full -- 16 keys physically held at once, which the emulated
     * matrix could not resolve anyway (pads.c models the ECS's own
     * ghosting). Evict slot 0 rather than either dropping the keystroke or
     * leaking its assertion: the evicted key's own release will find no
     * record and do nothing, and the focus-loss clear is the backstop. */
    if (free_slot < 0)
    {
        intvsession_ecs_key_set(s, (intvsession_ecs_key)s->ecs_held[0].key, 0);
        free_slot = 0;
    }

    s->ecs_held[free_slot].host_key = host_key;
    s->ecs_held[free_slot].key = (uint16_t)key;
    intvsession_ecs_key_set(s, key, 1);
}

void intvsession_pads_clear(intvsession *s)
{
    (void)s;
    intv_host_pads_clear();
    /* A gamepad holding a direction would otherwise stay released until it
     * next CHANGES, since poll_sticks only writes the disc on a change --
     * this is exactly what forget_disc exists for. Held gamepad *buttons*
     * are edge-driven and do stay released until the next press; that is the
     * same trade intvsession_ecs_keys_clear already makes, and a controller
     * held down through a focus change is the rarer case than a key. */
    intv_gamepad_forget_disc();
}
