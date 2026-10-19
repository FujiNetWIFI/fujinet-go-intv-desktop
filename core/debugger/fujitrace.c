/*
 * fujitrace -- see fujitrace.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujitrace.h"

#include <stdio.h>
#include <string.h>

#include "fuji_names.h"

static const char *fuji_command_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof(fuji_command_names) / sizeof(fuji_command_names[0]); i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* FNV-1a: two packets with the same first bytes, length and hash are the
 * same packet. */
static uint32_t hash_bytes(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static intvdebug_fuji_packet *entry(fujitrace *t, int k)
{
    return &t->ring[(t->first + k) % FUJITRACE_MAX];
}

static void format(intvdebug_fuji_packet *p)
{
    const char *who = p->to_host ? "FujiNet \xe2\x86\x92 Intellivision"
                                 : "Intellivision \xe2\x86\x92 FujiNet";
    char times[24] = "";

    if (p->repeat > 1)
        snprintf(times, sizeof(times), " \xc3\x97%u", (unsigned)p->repeat);
    if (p->device >= 0) {
        const char *name = fuji_command_name(p->command);
        snprintf(p->text, sizeof(p->text), "%s %02X %02X%s%s (%d bytes)%s",
                 who, p->device, p->command, name[0] ? " " : "", name,
                 p->nbytes, times);
    } else {
        snprintf(p->text, sizeof(p->text), "%s %d bytes%s", who, p->nbytes,
                 times);
    }
}

/* Entries from k on get new serials, oldest first, so a reader that only
 * watches the count and the last serial sees any change. */
static void touch(fujitrace *t, int k)
{
    for (; k < t->count; k++)
        entry(t, k)->serial = ++t->serial;
}

static void bump(fujitrace *t, int k, int by)
{
    intvdebug_fuji_packet *p = entry(t, k);
    p->repeat = (uint32_t)((int)p->repeat + by);
    format(p);
    touch(t, k);
}

static int same(fujitrace *t, int k, const intvdebug_fuji_packet *p,
                uint32_t hash)
{
    const intvdebug_fuji_packet *e = entry(t, k);
    int n = p->nbytes < (int)sizeof(p->data) ? p->nbytes : (int)sizeof(p->data);
    return e->to_host == p->to_host && e->nbytes == p->nbytes &&
           t->hash[(t->first + k) % FUJITRACE_MAX] == hash &&
           memcmp(e->data, p->data, (size_t)n) == 0;
}

static void push(fujitrace *t, const intvdebug_fuji_packet *p, uint32_t hash)
{
    int slot;
    if (t->count == FUJITRACE_MAX) {
        t->first = (t->first + 1) % FUJITRACE_MAX;
        t->count--;
    }
    slot = (t->first + t->count) % FUJITRACE_MAX;
    t->ring[slot] = *p;
    t->hash[slot] = hash;
    t->count++;
    touch(t, t->count - 1);
}

static void emit(fujitrace *t, int to_host)
{
    const uint8_t *b = t->frame[to_host];
    const int n = t->frame_len[to_host];
    intvdebug_fuji_packet p;
    uint32_t hash = hash_bytes(b, n);

    memset(&p, 0, sizeof(p));
    p.to_host = to_host;
    p.nbytes = n;
    p.repeat = 1;
    memcpy(p.data, b, (size_t)(n < (int)sizeof(p.data) ? n : (int)sizeof(p.data)));
    p.device = n >= 6 ? b[0] : -1;
    p.command = n >= 6 ? b[1] : -1;
    p.length = n >= 6 ? (b[2] | (b[3] << 8)) : -1;
    format(&p);

    if (t->pending) {
        t->pending = 0;
        /* The reply matches too: the whole exchange repeated. */
        if (same(t, t->count - 1, &p, hash)) {
            bump(t, t->count - 1, 1);
            return;
        }
        /* It did not: the request was a new one after all. */
        {
            intvdebug_fuji_packet req = *entry(t, t->count - 2);
            uint32_t req_hash = t->hash[(t->first + t->count - 2) % FUJITRACE_MAX];
            bump(t, t->count - 2, -1);
            req.repeat = 1;
            format(&req);
            push(t, &req, req_hash);
        }
    }

    if (t->count >= 1 && same(t, t->count - 1, &p, hash)) {
        bump(t, t->count - 1, 1);
        return;
    }
    /* A request that starts the exchange before last over again: count it
     * now, and let its reply decide. */
    if (t->count >= 2 && same(t, t->count - 2, &p, hash) &&
        entry(t, t->count - 2)->to_host != entry(t, t->count - 1)->to_host &&
        entry(t, t->count - 2)->repeat == entry(t, t->count - 1)->repeat) {
        bump(t, t->count - 2, 1);
        t->pending = 1;
        return;
    }
    push(t, &p, hash);
}

void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len)
{
    const int to_host = dir == 0;
    size_t i;

    /* fujinet.c drops whatever sits in the socket before it sends, so half a
     * reply here was thrown away too. */
    if (!to_host) {
        t->frame_len[1] = 0;
        t->esc[1] = 0;
    }

    for (i = 0; i < len; i++) {
        const uint8_t b = buf[i];
        if (b == 0xC0) {
            if (t->frame_len[to_host] > 0) {
                emit(t, to_host);
                t->frame_len[to_host] = 0;
            }
            t->esc[to_host] = 0;
            continue;
        }
        if (t->esc[to_host]) {
            if (t->frame_len[to_host] < FUJITRACE_FRAME_MAX)
                t->frame[to_host][t->frame_len[to_host]++] =
                    b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b;
            t->esc[to_host] = 0;
        } else if (b == 0xDB) {
            t->esc[to_host] = 1;
        } else if (t->frame_len[to_host] < FUJITRACE_FRAME_MAX) {
            t->frame[to_host][t->frame_len[to_host]++] = b;
        }
    }
}

void fujitrace_clear(fujitrace *t)
{
    t->first = 0;
    t->count = 0;
    t->pending = 0;
}

int fujitrace_copy(const fujitrace *t, intvdebug_fuji_packet *out, int max)
{
    int start, k, n = 0;
    if (max <= 0)
        return 0;
    start = t->count > max ? t->count - max : 0;
    for (k = start; k < t->count; k++)
        out[n++] = t->ring[(t->first + k) % FUJITRACE_MAX];
    return n;
}
