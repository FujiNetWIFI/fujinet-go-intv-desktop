/*
 * intvsession's private state. Not installed; only session.c, settings.c and
 * paths.c include it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_SESSION_INTERNAL_H
#define INTV_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "intvsession.h"

#define INTV_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct intvsession {
    char config_dir[INTV_PATH_MAX];
    char data_dir[INTV_PATH_MAX];
    char roms_dir[INTV_PATH_MAX];
    char settings_file[INTV_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    int fujinet_port;
    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) --------------------------
     * Pure derivations of data_dir, settled in paths_init regardless of
     * whether FujiNet ever actually starts -- see paths.c's own comment on
     * why (mirrors cocosession's paths_init). */
    char fujinet_root[INTV_PATH_MAX];
    char fujinet_config[INTV_PATH_MAX];
    char fujinet_sd[INTV_PATH_MAX];
    char fujinet_data[INTV_PATH_MAX];
    char fujinet_lib[INTV_PATH_MAX];   /* resolved lazily; "" until then */
    char webui_url[64];
    int  fujinet_running;

    void *audio;   /* audio_sdl.c state */
};

void settings_init(struct intvsession *s);
void settings_free_all(struct intvsession *s);

int paths_init(struct intvsession *s, const char *config_dir,
              const char *data_dir);
/* Locates libfujinet.{so,dylib}/fujinet.dll and provisions the runtime tree
 * (fnconfig.ini + data/ + SD/) on first run. Returns 0 on success, -1 if no
 * runtime is available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct intvsession *s);

void session_set_error(struct intvsession *s, const char *fmt, ...);

/* ---- fujinet_runtime.c ---------------------------------------------- */
int  fujinet_start(struct intvsession *s);
void fujinet_stop(struct intvsession *s);
/* Blocks (up to timeout_ms) until something is accepting connections on
 * the BoIP port -- see fujinet_runtime.c's own comment on why start()
 * returning is not enough to know the listener is actually up yet. */
int  fujinet_wait_for_boip(struct intvsession *s, int timeout_ms);
int  fujinet_copy_log(struct intvsession *s, char *dst, int max);
/* Overlays FujiNet's (mono) SAM speech audio onto buf in place, saturating.
 * nsamples/rate describe buf, which -- unlike the CoCo port's stereo XRoar
 * mix -- is already mono, matching jzIntv's own single PSG channel, so no
 * channel widening is needed here. */
void fujinet_mix_audio(struct intvsession *s, int16_t *buf, int nsamples,
                       int rate);

/* ---- audio_sdl.c ------------------------------------------------------ */
int  audio_start(struct intvsession *s);
void audio_stop(struct intvsession *s);

#endif /* INTV_SESSION_INTERNAL_H */
