/*
 * intvsymtab -- see symbols.h.
 *
 * The built-in table is deliberately small, unlike the CoCo/ADAM/MSX
 * ports' own (Color BASIC's POLCAT/CHROUT/etc., or EOS's documented
 * entries): this project has no verified primary source in hand for
 * documented Intellivision EXEC subroutine entry points (no ROM
 * disassembly listing, no vendor programming manual, nothing locally
 * checkable the way the CoCo port had two independent secondary sources to
 * cross-check against). Rather than transcribe addresses from memory with
 * no way to verify them -- which is exactly the failure mode CoCo's own
 * symtab.c warns against ("a wrong address in a debugger is worse than a
 * missing one") -- the built-in table here holds only what is directly
 * confirmed by reading the staged jzIntv tree's own source, not a
 * datasheet or a recollection:
 *
 *   $1000  EXEC_ENTRY     cp1600_reset's own r[7] = 0x1000 on reset
 *                         (src/cp1600/cp1600.c), and intv_host's own boot
 *                         log independently reports "EXEC ROM
 *                         [0x1000...0x1FFF]" as the mapped range.
 *   $9C00  FUJINET_MAILBOX FUJINET_WINDOW_SIZE's own placement
 *                         (src/fujinet/fujinet.h) and intv_host's boot log
 *                         ("FujiNet [0x9C00...0x9FFF]").
 *
 * A real EXEC symbol table is exactly what intvsymtab_load exists for: run
 * `as1600 -s` against a disassembly (or use a published community .sym
 * file) and load it -- the as1600 output format is what this loader
 * actually speaks, verified against jzIntv's own loader for the same files
 * (src/debug/debug.c), not assumed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_NAME_MAX 32

typedef struct {
    uint16_t addr;
    char name[SYM_NAME_MAX];
} sym_entry;

struct intvsymtab {
    sym_entry *entries;
    int count;
    int cap;
    int builtin_count; /* entries[0..builtin_count) survive clear_user */
};

static const struct { uint16_t addr; const char *name; } k_builtin[] = {
    { 0x1000, "EXEC_ENTRY" },
    { 0x9C00, "FUJINET_MAILBOX" },
};
#define K_BUILTIN_COUNT (int)(sizeof(k_builtin) / sizeof(k_builtin[0]))

static int ensure_cap(intvsymtab *t, int need)
{
    if (need <= t->cap)
        return 1;
    int new_cap = t->cap ? t->cap * 2 : 16;
    while (new_cap < need)
        new_cap *= 2;
    sym_entry *grown = realloc(t->entries, (size_t)new_cap * sizeof(sym_entry));
    if (!grown)
        return 0;
    t->entries = grown;
    t->cap = new_cap;
    return 1;
}

intvsymtab *intvsymtab_create(void)
{
    intvsymtab *t = calloc(1, sizeof(*t));
    int i;
    if (!t)
        return NULL;
    if (!ensure_cap(t, K_BUILTIN_COUNT)) {
        free(t);
        return NULL;
    }
    for (i = 0; i < K_BUILTIN_COUNT; i++) {
        t->entries[i].addr = k_builtin[i].addr;
        snprintf(t->entries[i].name, SYM_NAME_MAX, "%s", k_builtin[i].name);
    }
    t->count = K_BUILTIN_COUNT;
    t->builtin_count = K_BUILTIN_COUNT;
    return t;
}

void intvsymtab_destroy(intvsymtab *t)
{
    if (!t)
        return;
    free(t->entries);
    free(t);
}

int intvsymtab_add(intvsymtab *t, uint16_t addr, const char *name)
{
    if (!t || !name || !name[0])
        return 0;
    if (!ensure_cap(t, t->count + 1))
        return 0;
    t->entries[t->count].addr = addr;
    snprintf(t->entries[t->count].name, SYM_NAME_MAX, "%s", name);
    t->count++;
    return 1;
}

void intvsymtab_clear_user(intvsymtab *t)
{
    if (!t)
        return;
    t->count = t->builtin_count;
}

int intvsymtab_lookup_addr(const intvsymtab *t, uint16_t addr, char *dst,
                           int dstsz)
{
    int i;
    if (!t)
        return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (t->entries[i].addr == addr) {
            if (dst && dstsz > 0)
                snprintf(dst, (size_t)dstsz, "%s", t->entries[i].name);
            return 1;
        }
    }
    return 0;
}

int intvsymtab_lookup_name(const intvsymtab *t, const char *name,
                           uint16_t *addr_out)
{
    int i;
    if (!t || !name)
        return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (strcmp(t->entries[i].name, name) == 0) {
            if (addr_out)
                *addr_out = t->entries[i].addr;
            return 1;
        }
    }
    return 0;
}

/* as1600's -s output, one "hexaddr name" pair per line -- see this file's
 * header and symbols.h for the exact format, confirmed against jzIntv's own
 * loader for the same files (src/debug/debug.c): undefined symbols are
 * marked with a leading '?' instead of a hex address and are skipped,
 * anything decoding above $FFFF is skipped. */
int intvsymtab_load(intvsymtab *t, const char *path)
{
    FILE *f;
    char line[512];
    char name[512];
    unsigned addr;
    int added = 0;

    if (!t || !path)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '?')
            continue;
        if (sscanf(line, "%x %511s", &addr, name) != 2)
            continue;
        if (addr > 0xFFFF || name[0] == 0)
            continue;
        if (intvsymtab_add(t, (uint16_t)addr, name))
            added++;
    }

    fclose(f);
    return added;
}
