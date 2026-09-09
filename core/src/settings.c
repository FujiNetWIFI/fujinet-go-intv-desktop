/*
 * intvsession settings store: one flat key=value file shared by every
 * frontend, so a preference chosen in one app is what the next one launches
 * with. Deliberately not GSettings/KConfig.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

static setting_kv *find(struct intvsession *s, const char *key)
{
    setting_kv *kv;
    for (kv = s->settings; kv; kv = kv->next)
        if (strcmp(kv->key, key) == 0)
            return kv;
    return NULL;
}

static void set_locked(struct intvsession *s, const char *key,
                       const char *val)
{
    setting_kv *kv = find(s, key);
    if (kv) {
        if (strcmp(kv->val, val) != 0) {
            free(kv->val);
            kv->val = strdup(val);
            s->settings_dirty = 1;
        }
        return;
    }
    kv = calloc(1, sizeof(*kv));
    if (!kv) return;
    kv->key = strdup(key);
    kv->val = strdup(val);
    kv->next = s->settings;
    s->settings = kv;
    s->settings_dirty = 1;
}

/* Reads one whole line, however long, growing *buf as needed. Returns 1 on a
 * line (NUL-terminated, newline retained if there was one), 0 at EOF with
 * nothing read.
 *
 * A fixed buffer will not do here. The "bindings" value packs every
 * non-default key and gamepad mapping into a single line (see bindings.c's
 * pack_locked) -- a fully remapped two-controller adapter runs to several
 * kilobytes. fgets into a 1024-byte buffer used to split such a line, and
 * because the continuation has no '=' the parser below dropped it: every
 * binding past the first ~1015 bytes silently vanished on the next launch.
 * That hit exactly the users who had the most mapping work to lose. */
static int read_line(FILE *fp, char **buf, size_t *cap)
{
    size_t len = 0;

    for (;;) {
        if (len + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 256;
            char *nbuf = realloc(*buf, ncap);
            if (!nbuf)
                return len > 0; /* Out of memory: keep what we have. */
            *buf = nbuf;
            *cap = ncap;
        }
        if (!fgets(*buf + len, (int)(*cap - len), fp))
            return len > 0;
        len += strlen(*buf + len);
        if (len == 0 || (*buf)[len - 1] == '\n')
            return 1;
        /* No newline yet: either the line is longer than the buffer (grow and
         * read the rest) or this is a last line with no trailing newline,
         * which the next fgets reports as EOF and the `len > 0` above keeps. */
    }
}

void settings_init(struct intvsession *s)
{
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;

    pthread_mutex_init(&s->settings_mtx, NULL);
    fp = fopen(s->settings_file, "r");
    if (!fp) return;
    while (read_line(fp, &line, &cap)) {
        char *eq, *end;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        end = eq + 1 + strcspn(eq + 1, "\r\n");
        *end = '\0';
        set_locked(s, p, eq + 1);
    }
    free(line);
    fclose(fp);
    s->settings_dirty = 0;
}

void settings_free_all(struct intvsession *s)
{
    setting_kv *kv = s->settings;
    while (kv) {
        setting_kv *next = kv->next;
        free(kv->key);
        free(kv->val);
        free(kv);
        kv = next;
    }
    s->settings = NULL;
    pthread_mutex_destroy(&s->settings_mtx);
}

const char *intvsession_get_str(intvsession *s, const char *key,
                                const char *def)
{
    setting_kv *kv;
    const char *v;
    pthread_mutex_lock(&s->settings_mtx);
    kv = find(s, key);
    v = kv ? kv->val : def;
    pthread_mutex_unlock(&s->settings_mtx);
    return v;
}

void intvsession_set_str(intvsession *s, const char *key, const char *value)
{
    pthread_mutex_lock(&s->settings_mtx);
    set_locked(s, key, value ? value : "");
    pthread_mutex_unlock(&s->settings_mtx);
    intvsession_settings_flush(s);
}

int intvsession_get_int(intvsession *s, const char *key, int def)
{
    const char *v = intvsession_get_str(s, key, NULL);
    if (!v || !*v) return def;
    return atoi(v);
}

void intvsession_set_int(intvsession *s, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    intvsession_set_str(s, key, buf);
}

void intvsession_settings_flush(intvsession *s)
{
    char tmp[INTV_PATH_MAX + 8];
    FILE *fp;
    setting_kv *kv;

    pthread_mutex_lock(&s->settings_mtx);
    if (!s->settings_dirty) {
        pthread_mutex_unlock(&s->settings_mtx);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->settings_file);
    fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "# FujiNet Go Intv shared settings\n");
        for (kv = s->settings; kv; kv = kv->next)
            fprintf(fp, "%s=%s\n", kv->key, kv->val);
        fclose(fp);
#if defined(_WIN32)
        remove(s->settings_file);
#endif
        rename(tmp, s->settings_file);
        s->settings_dirty = 0;
    }
    pthread_mutex_unlock(&s->settings_mtx);
}
