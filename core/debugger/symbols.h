/*
 * intvsymtab -- name<->address table for the debugger's disassembly view.
 *
 * Not installed; shared only between debugger.c (if it ever wants to offer
 * symbol lookups) and symbols.c. Modelled directly on fujinet-go-coco-
 * desktop's own cocosymtab.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_SYMTAB_H
#define INTV_SYMTAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct intvsymtab intvsymtab;

/* Seeds the built-in table (see symbols.c for exactly what -- deliberately
 * minimal, see that file's header) and returns a table ready to use. Never
 * NULL except on out-of-memory. */
intvsymtab *intvsymtab_create(void);
void intvsymtab_destroy(intvsymtab *t);

/* Loads a symbol file on top of whatever is already there. Accepts as1600's
 * own -s output format exactly (verified against jzIntv's own loader for
 * the same files, src/debug/debug.c: one "hexaddr name" pair per line,
 * whitespace-separated, undefined symbols marked "????????" and skipped,
 * anything above $FFFF skipped). Returns the number of symbols added, or -1
 * if the file could not be opened. */
int intvsymtab_load(intvsymtab *t, const char *path);

/* Adds one symbol directly. Counts as a user symbol, so
 * intvsymtab_clear_user() drops it too. Returns 1 on success, 0 on
 * out-of-memory or a NULL table/name. */
int intvsymtab_add(intvsymtab *t, uint16_t addr, const char *name);

/* Drops everything intvsymtab_load/_add added; the built-in table is
 * untouched. */
void intvsymtab_clear_user(intvsymtab *t);

/* Exact-address lookup. Returns 1 and copies the name (NUL-terminated,
 * truncated to dstsz) when addr has one, 0 otherwise. Symbols added later
 * win ties over earlier ones (a loaded file can shadow a built-in name at
 * the same address), so this scans newest-first. */
int intvsymtab_lookup_addr(const intvsymtab *t, uint16_t addr, char *dst,
                           int dstsz);

/* Name -> address, case-sensitive exact match (as1600 symbols are
 * case-sensitive). Returns 1 and fills *addr_out on a hit. */
int intvsymtab_lookup_name(const intvsymtab *t, const char *name,
                           uint16_t *addr_out);

#ifdef __cplusplus
}
#endif

#endif /* INTV_SYMTAB_H */
