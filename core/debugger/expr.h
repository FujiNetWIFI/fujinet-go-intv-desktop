/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the prompt takes.
 *
 *   numbers     $C000  0xC000  C000h  49152  %1010
 *   names       whatever the context resolves: registers, flags, labels,
 *               r0-r7, pc, sp, s z o c i d, frame, cycles, and
 *               value/addr/old in a breakpoint condition
 *   memory      [expr] the 16-bit word at that address, side-effect free
 *               (the scratchpad, GROM and GRAM hold 8 bits of it)
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_EXPR_H
#define INTV_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A word of memory, without side effects. */
    uint16_t (*read_word)(void *user, uint16_t addr);
    void *user;
} intvexpr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int intvexpr_eval(const char *text, const intvexpr_ctx *ctx, long *out, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* INTV_EXPR_H */
