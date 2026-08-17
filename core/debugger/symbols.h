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

#include "intvdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct intvsymtab intvsymtab;

/* Seeds the built-in table (see symbols.c for exactly what -- deliberately
 * minimal, see that file's header) and returns a table ready to use. Never
 * NULL except on out-of-memory. Used directly only by symtab_test.c; a
 * debugger window should call intvsymtab_shared() instead (see below). */
intvsymtab *intvsymtab_create(void);
void intvsymtab_destroy(intvsymtab *t);

/* The process-wide symbol table -- same singleton contract as
 * intvdebug_get() (created on first call, never destroyed). A cart arrives
 * through FujiNet CONFIG with no path a .sym file could ever be derived
 * from (see intvsymtab_load's own comment), so loading one is always a
 * manual step the user takes after the game is already running; a symbol
 * table scoped to one debugger window would throw that work away the
 * moment the window closes. All four frontends should use this instead of
 * owning a private intvsymtab_create()/_destroy() pair. Never NULL except
 * on out-of-memory (matches intvsymtab_create). */
intvsymtab *intvsymtab_shared(void);

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

/* Nearest symbol at or below addr, within `window` words (addr - sym <=
 * window). Fills *delta with addr - sym (0 on an exact hit). Returns 0 if
 * nothing qualifies -- callers fall back to intvsym_region(). Scans
 * newest-first like intvsymtab_lookup_addr, so a loaded symbol can shadow a
 * closer built-in at the same address. */
int intvsymtab_lookup_nearest(const intvsymtab *t, uint16_t addr, int window,
                              char *dst, int dstsz, int *delta);

/* Name of the hardware/EXEC region containing addr (e.g. "STIC", "PSG",
 * "BACKTAB", "EXEC ROM"), or NULL if addr isn't in any named range. See
 * symbols.c for the full map and its sources (PORTING.md Sections 5.4/7.1
 * across the five FujiNet Intellivision ports). */
const char *intvsym_region(uint16_t addr);

/* Builds the trailing "   ; COMMENT" annotation for one disassembly line:
 * the line's own address resolved to a label/region, then any operand
 * ($XXXX token in l->text) resolved the same way. Writes "" (dst[0] = 0)
 * when there is nothing to say. Returns the number of characters written
 * (not counting the NUL), like snprintf. Shared by all four frontends so
 * the annotation logic exists in exactly one place. */
int intvsym_annotate_line(const intvsymtab *t, const intvdebug_dasm_line *l,
                          char *dst, int dstsz);

/* Resolves user-typed text (a memory "go to" box, a "break at" box, ...) to
 * an address: an exact, case-sensitive symbol name if one matches (as1600
 * symbols are case-sensitive), otherwise hex with an optional leading '$'
 * or "0x"/"0X". Surrounding whitespace is ignored; trailing garbage after
 * the hex digits, or anything decoding above $FFFF, is rejected. Name is
 * tried before hex, so a symbol literally named e.g. "1000" resolves as
 * that symbol rather than as $1000. Returns 1 and fills *addr_out on
 * success, 0 (== *addr_out unchanged) otherwise. t may be NULL (hex-only). */
int intvsym_parse_addr(const intvsymtab *t, const char *text,
                       uint16_t *addr_out);

/* Directory part of path, accepting both '/' and '\\' as separators (a
 * loaded .sym's path may come from any of the four frontends' native file
 * pickers). Writes "" when path has no separator. Returns the number of
 * characters written (not counting the NUL), like snprintf; 0 on a bad
 * argument. Used to remember the last directory a .sym was loaded from
 * (settings key "sym_dir") so re-opening the picker doesn't start from
 * scratch every time. */
int intvsym_dirname(const char *path, char *dst, int dstsz);

/* Number of symbols currently in the table (built-ins + loaded). */
int intvsymtab_count(const intvsymtab *t);

/* idx-th symbol in address-ascending order (ties broken oldest-first, i.e.
 * the *opposite* of intvsymtab_lookup_addr's newest-wins: a symbol browser
 * wants to show every shadowed name, not just the winner). Copies into
 * addr/name/note (note may be "" ; either out pointer may be NULL if the
 * caller doesn't need it). Returns 1 on success, 0 if idx is out of range.
 * Not const: may lazily (re)build the sort index used by lookups too. */
int intvsymtab_enum(intvsymtab *t, int idx, uint16_t *addr, char *name,
                    int namesz, char *note, int notesz);

/* Bumped by every intvsymtab_add/_load/_clear_user call. A symbol browser
 * can cache its rendered list and skip rebuilding it while this is
 * unchanged, rather than re-walking the table on every UI tick. */
unsigned intvsymtab_generation(const intvsymtab *t);

#ifdef __cplusplus
}
#endif

#endif /* INTV_SYMTAB_H */
