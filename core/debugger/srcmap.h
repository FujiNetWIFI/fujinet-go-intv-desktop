/*
 * intvsrcmap -- address <-> IntyBASIC source line, for the debugger's BASIC
 * tab and its line stepping.
 *
 * The map comes from the toolchain, not from us. IntyBASIC writes a
 * `SRCFILE "file.bas",N` line into its .asm before every BASIC line (and
 * follows INCLUDEd files), and as1600 turns those into either
 *   -j game.smap   "FILE name" directives plus "lo hi flags line listline"
 *                  rows (hex addresses), or
 *   -l game.lst    the listing, which repeats the SRCFILE lines in front of
 *                  "ADDR  word word ..." rows.
 * Both are read here. jzIntv's own src/debug/source.c reads the .smap too,
 * but it keeps one static 64K table that cannot be reset, bakes in the build
 * machine's absolute paths, and prints through jzp_printf; this is the small
 * version a GUI debugger can reload and query from two threads.
 *
 * Only BASIC files count: anything as1600 names with a .asm extension (the
 * IntyBASIC prologue/epilogue, the generated .asm itself) is ignored, so
 * stepping by line runs straight through the runtime.
 *
 * Thread-safe: one mutex inside. The emulator thread queries
 * (intvsrcmap_is_line_start) while a UI thread imports.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_SRCMAP_H
#define INTV_SRCMAP_H

#include <stdint.h>

#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct intvsrcmap intvsrcmap;

intvsrcmap *intvsrcmap_create(void);
void intvsrcmap_destroy(intvsrcmap *m);

/* The process-wide map, like intvsymtab_shared(): the debugger window comes
 * and goes, the imported source should not. Never NULL except on
 * out-of-memory. */
intvsrcmap *intvsrcmap_shared(void);

/* Drops every file and mapping. */
void intvsrcmap_clear(intvsrcmap *m);

typedef struct {
    int  files;        /* source files now loaded (INCLUDEs included) */
    int  mapped;       /* address ranges the map placed on a loaded file; 0 =
                        * no map found, the caller should ask for one */
    int  symbols;      /* symbols loaded from the .sym next to the map */
    char map_path[512];/* the .smap/.lst used, "" if none */
} intvsrcmap_import_result;

/* The one call a frontend needs. `path` is a .bas file or a folder.
 *   file   : loads it and everything it INCLUDEs (recursively; a cycle is
 *            loaded once).
 *   folder : loads every .bas under it, to a depth of 8.
 * Replaces whatever was loaded before. Then looks for the map: for a file,
 * <name>.smap then <name>.lst beside it; in every case any .smap/.lst in the
 * same folder, the one naming the most loaded files winning. If one is found
 * it is loaded, and a <name>.sym beside it goes into `sym` (may be NULL).
 * Returns the file count, or -1 if `path` can't be read or holds no .bas
 * files (nothing is changed then). `r` may be NULL. */
int intvsrcmap_import(intvsrcmap *m, const char *path, intvsymtab *sym,
                      intvsrcmap_import_result *r);

/* Loads an .smap or .lst on top of the loaded sources ("Load Map..."). The
 * kind is detected from the content, not the extension. Names in the map are
 * matched to loaded files by path suffix, then by base name; a name that
 * matches nothing is tried relative to the map's folder and loaded from
 * there. Returns the number of ranges mapped, or -1 if unreadable. */
int intvsrcmap_load_map(intvsrcmap *m, const char *path);

/* Loads <map without its extension>.sym into `sym`, if there is one. Returns
 * the number of symbols added; 0 for none or no such file. The import does
 * this by itself; call it after intvsrcmap_load_map. */
int intvsrcmap_load_sibling_sym(const char *map_path, intvsymtab *sym);

/* Bumped by every import/load/clear. A UI can skip re-rendering while it is
 * unchanged. */
unsigned intvsrcmap_generation(const intvsrcmap *m);

/* ---- files ------------------------------------------------------------- */
int         intvsrcmap_file_count(const intvsrcmap *m);
/* Display name (path relative to the import root when there is one). The
 * pointer stays valid until the next import/load/clear. */
const char *intvsrcmap_file_name(const intvsrcmap *m, int file);
int         intvsrcmap_line_count(const intvsrcmap *m, int file);
/* 1-based line, no newline. NULL if out of range (or the file's text was
 * never found: the map named it but it isn't on disk). Same lifetime as
 * file_name. */
const char *intvsrcmap_line_text(const intvsrcmap *m, int file, int line);

/* ---- address <-> line -------------------------------------------------- */
/* Finds the BASIC line holding addr. Returns 1 and fills file and line (the
 * line is 1-based), 0 when addr isn't BASIC code. Either out pointer may be
 * NULL. */
int intvsrcmap_lookup(const intvsrcmap *m, uint16_t addr, int *file, int *line);

/* 1 when addr is the first word of a BASIC line's code: where Step Line
 * stops. Cheap; called on the emulator thread once per instruction. */
int intvsrcmap_is_line_start(const intvsrcmap *m, uint16_t addr);

/* The lowest line-start address for the line: where a breakpoint on it goes.
 * Returns -1 when the line has no code (a comment, a label, a blank). */
int intvsrcmap_first_addr(const intvsrcmap *m, int file, int line);

#ifdef __cplusplus
}
#endif

#endif /* INTV_SRCMAP_H */
