/*
 * fujitrace -- FujiBus packets out of the raw bytes of a SLIP link, both
 * ways, kept in a ring for the debugger's FujiNet tab.
 *
 * Pure: no emulator and no locking. debugger.c feeds it from jzIntv's
 * fn_sock tap under its own lock; core/tests/fujitrace_test.c drives it with
 * synthetic bytes. Not installed, like symbols.h.
 *
 * Two quirks of the INTV link shape it:
 *  - A send throws away half a reply still waiting in the receive path,
 *    because fujinet.c drains (and drops) whatever is in the socket before
 *    each request.
 *  - Consecutive identical exchanges (CONFIG's polls) fold into one entry
 *    with a count, rather than flooding the ring.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_FUJITRACE_H
#define INTV_FUJITRACE_H

#include <stddef.h>
#include <stdint.h>

#include "intvdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FUJITRACE_MAX INTVDEBUG_FUJI_TRACE_MAX
#define FUJITRACE_FRAME_MAX 4096 /* longer frames keep their first bytes */

/* All zeroes is the empty trace. */
typedef struct {
    intvdebug_fuji_packet ring[FUJITRACE_MAX];
    uint32_t hash[FUJITRACE_MAX]; /* of each whole packet, for folding */
    int first, count;
    uint64_t serial;

    /* Indexed by to_host: the frame being decoded each way. */
    uint8_t frame[2][FUJITRACE_FRAME_MAX];
    int frame_len[2];
    int esc[2];

    /* The entry before last was just counted again as the start of a
     * repeated exchange; the next packet confirms it or takes it back. */
    int pending;
} fujitrace;

/* dir 1: bytes the Intellivision sent; dir 0: bytes it received. */
void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len);

/* Drops the packets, not a frame half decoded. */
void fujitrace_clear(fujitrace *t);

/* The newest max packets, oldest first; returns how many. */
int fujitrace_copy(const fujitrace *t, intvdebug_fuji_packet *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* INTV_FUJITRACE_H */
