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
};

void settings_init(struct intvsession *s);
void settings_free_all(struct intvsession *s);

int paths_init(struct intvsession *s, const char *config_dir,
              const char *data_dir);

void session_set_error(struct intvsession *s, const char *fmt, ...);

#endif /* INTV_SESSION_INTERNAL_H */
