/*
 * Watches, breakpoint descriptions and memory-search queries, kept in the
 * core so the four debugger windows only lay them out.
 *
 * A watch is an expression the user typed (an address, a symbol, an IntyBASIC
 * variable or array element, or a range lo-hi) shown with its live value. It
 * is stored as text and resolved against the symbol table every time it is
 * shown, so loading a .sym later makes "x" start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_MEMTOOLS_H
#define INTV_MEMTOOLS_H

#include <stdint.h>

#include "intvdebug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo-hi", a single address/symbol/BASIC variable, or an array (the whole
 * array). Returns 1 and fills lo, hi (inclusive) and the word width (8 or 16),
 * 0 if the text means nothing. width may be NULL. */
int intvsym_parse_range(const intvsymtab *t, const char *text, uint16_t *lo,
                        uint16_t *hi, int *width);

/* ---- watches ---------------------------------------------------------------- */
#define INTVWATCH_MAX 64

/* The process-wide list, like the symbol table: it outlives the window. */
int  intvwatch_count(void);
/* Adds `text`; returns its index, or -1 if empty / full. A watch that does not
 * resolve yet is still kept (it shows "?"). */
int  intvwatch_add(const char *text);
void intvwatch_remove(int index);
void intvwatch_clear(void);
const char *intvwatch_text(int index);
/* Bumped by add/remove/clear. */
unsigned intvwatch_generation(void);

/* One display line for a watch: "x      $010C  $42 (66)". While running (the
 * machine's memory cannot be read) the value is "-". A range shows its first
 * 8 words. Returns the number of characters written. */
int  intvwatch_format(intvdebug *d, const intvsymtab *t, int index, char *dst, int dstsz);
/* The resolved range of a watch, for "go to in memory" / "break on write". */
int  intvwatch_range(const intvsymtab *t, int index, uint16_t *lo, uint16_t *hi);

/* ---- memory search queries ----------------------------------------------------- */
enum { INTVSEARCH_HEX = 0, INTVSEARCH_TEXT = 1 };

/* Turns what the user typed into a pattern. Hex: words separated by spaces or
 * commas, `$`/`0x` allowed, 1-4 digits each. Text: one character per word, low
 * byte only, case-insensitive. Returns the pattern length (1..64), 0 if the
 * text is empty or not valid. */
int  intvsearch_parse(int kind, const char *text, uint16_t *pattern, uint16_t *mask, int *flags);

/* The next match at or after `from` (wrapping once past $FFFF), or the
 * previous one before it when `backward`. Returns 1 and fills *found. Paused only. */
int  intvsearch_next(intvdebug *d, const uint16_t *pattern, int n, uint16_t mask, int flags,
                     uint16_t from, int backward, uint16_t *found);

/* ---- describing a breakpoint -------------------------------------------------------- */
/* "Write  $010C            if changed" -- type, range with its symbol, condition. */
int  intvbp_describe(const intvdebug_bp *bp, const intvsymtab *t, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* INTV_MEMTOOLS_H */
