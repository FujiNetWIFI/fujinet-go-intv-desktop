/*
 * intvsymtab -- see symbols.h.
 *
 * The built-in table used to hold only two entries, because at the time
 * this file was first written there was no verified primary source in
 * hand for Intellivision EXEC subroutine entry points. That source now
 * exists: `dis1600` (the disassembler shipped in jzIntv's own tree) run
 * against the console EXEC ROM produces build/exec.dis, and the table
 * below is exactly what five independent FujiNet Intellivision ports --
 * intv-baseball-experiment, then fujinet-intv-auto-racing,
 * fujinet-intv-football, fujinet-intv-armor-battle and
 * fujinet-intv-utopia, each building on the last -- confirmed against that
 * disassembly while reverse-engineering the console EXEC's timer table,
 * RNG, controller scan and ISR. See each port's tools/recon.py
 * (EXEC_NAMES/CELLS dicts), src/exec_equ.asm, and PORTING.md Sections 3,
 * 5.4 and 7 for the underlying analysis and the cross-cart confirmation
 * table.
 *
 * The list still stops well short of "every EXEC routine": the same
 * principle that motivated the original two-entry table applies to every
 * entry added since -- "a wrong address in a debugger is worse than a
 * missing one" -- so anything the ports flagged as a recon *candidate*
 * (not confirmed by hand in dis1600 output) or as game-specific is
 * deliberately left out of this built-in table. Per-game symbols (BB_*,
 * AR_*, FB_*, AB_*, UT_*, and the netcode's own LS_/RS_/NET_ symbols)
 * belong in a loaded .sym file, not here -- see intvsymtab_load.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_NAME_MAX 32
#define SYM_NOTE_MAX 48

typedef struct {
    uint16_t addr;
    char name[SYM_NAME_MAX];
    char note[SYM_NOTE_MAX]; /* may be empty; only ever set for built-ins */
} sym_entry;

struct intvsymtab {
    sym_entry *entries;
    int count;
    int cap;
    int builtin_count; /* entries[0..builtin_count) survive clear_user */
};

/* EXEC ROM entry points ($1000-$1FFF) and system RAM cells the EXEC owns,
 * confirmed against build/exec.dis -- see this file's header. Addresses
 * and notes taken verbatim from fujinet-intv-utopia/tools/recon.py's
 * EXEC_NAMES/CELLS dicts and cross-checked against the other four ports'
 * src/exec_equ.asm. Cart header fields ($5000-$5013) follow -- structural
 * to the EXEC cart format itself, not any one game, so they belong here
 * too rather than in a per-game .sym. */
static const struct { uint16_t addr; const char *name; const char *note; }
    k_builtin[] = {
    /* EXEC ROM entry points */
    { 0x1000, "EXEC_ENTRY", "" },
    { 0x1082, "X_POST_TITLE", "post-title mainline" },
    { 0x108F, "X_MAIN_LOOP", "EXEC main loop" },
    { 0x10BE, "X_ISR_BOOT", "boot-phase branch of X_DEF_ISR" },
    { 0x1126, "X_DEF_ISR", "default ISR (EXEC ISR vector value)" },
    { 0x11FA, "X_BOOT_HANDOFF", "boot/title hand-off, object/collision walk" },
    { 0x14CD, "X_TITLE_WAIT", "title wait loop" },
    { 0x14F1, "X_SCAN", "controller scan entry" },
    { 0x1523, "X_SCAN_INNER", "controller scan" },
    { 0x1525, "X_SCAN_RAW_L", "left raw-port read" },
    { 0x152C, "X_SCAN_RAW_R", "right raw-port read" },
    { 0x1532, "X_SCAN_DECODE", "shared decode entry" },
    { 0x15AC, "X_SCAN_HOLD", "re-marks decoded value as held" },
    { 0x15F8, "X_DISPATCH", "handler-table dispatch into game code" },
    { 0x167D, "X_RAND1", "RNG" },
    { 0x169E, "X_RAND2", "RNG" },
    { 0x1784, "X_OBJ_DIST", "object distance math" },
    { 0x17D5, "X_TIMER_PASS", "timer-table dispatch pass" },
    { 0x181E, "X_TIMER_RELOAD", "countdown := interval" },
    { 0x1831, "X_TIMER_SET_R0", "countdown := R0 (music)" },
    { 0x1838, "X_TIMER_STOP", "" },
    { 0x1844, "X_TIMER_START", "" },
    { 0x1906, "X_HTBL_NULL", "resident all-zero handler table" },
    { 0x1910, "X_NUM_ENTRY", "dispatch-driven number entry" },
    { 0x1A71, "X_MUSIC_TICK", "music note timer" },
    { 0x1AAD, "X_SOUND_UPDATE", "sound update" },
    { 0x1CCE, "X_SOUND_RNG", "sound engine RNG caller, churns $035E" },
    { 0x1F2F, "X_DO_GRAM_INIT", "" },

    /* System RAM cells owned by the EXEC */
    { 0x0100, "EXEC_ISRVEC_LO", "ISR vector, EXEC default $1126" },
    { 0x0101, "EXEC_ISRVEC_HI", "ISR vector, EXEC default $1126" },
    { 0x0102, "EXEC_ISR_PHASE", "ISR phase counter" },
    { 0x0103, "EXEC_PASS_LEN", "main-loop pass length in frames (3)" },
    { 0x0107, "EXEC_MOB_MASKS", "ISR MOB-rotation masks, 8 words" },
    { 0x011F, "EXEC_IN_L", "decoded input, left controller" },
    { 0x0120, "EXEC_IN_R", "decoded input, right controller" },
    { 0x0121, "EXEC_KEY_L", "keypad/action state, left" },
    { 0x0122, "EXEC_KEY_R", "keypad/action state, right" },
    { 0x0123, "EXEC_RAW_L", "raw port, left" },
    { 0x0124, "EXEC_RAW_R", "raw port, right" },
    { 0x0125, "EXEC_TIMERS", "timer countdown slots, 2 words/entry" },
    { 0x0149, "EXEC_SFX_BUSY", "SFX-busy gate" },
    { 0x035D, "EXEC_HTBL", "input handler-table pointer" },
    { 0x035E, "EXEC_RNG", "LFSR state -- never sim state" },
    { 0x035F, "EXEC_SND_FRAME", "sound frame counter" },

    /* Cart header ($5000 base) */
    { 0x5002, "CART_TIMER_TBL_LO", "" },
    { 0x5003, "CART_TIMER_TBL_HI", "" },
    { 0x5004, "CART_START_LO", "" },
    { 0x5005, "CART_START_HI", "" },
    { 0x5008, "CART_GRAM_INIT_LO", "" },
    { 0x5009, "CART_GRAM_INIT_HI", "" },
    { 0x500D, "CART_BORDER_EXT", "" },
    { 0x500E, "CART_DISP_MODE", "0 = colour stack, 1 = fg/bg" },
    { 0x500F, "CART_CSTACK0", "" },
    { 0x5010, "CART_CSTACK1", "" },
    { 0x5011, "CART_CSTACK2", "" },
    { 0x5012, "CART_CSTACK3", "" },
    { 0x5013, "CART_BORDER_COLOR", "" },

    /* Kept for compatibility with anything that still looks it up by name */
    { 0x9C00, "FUJINET_MAILBOX", "" },
};
#define K_BUILTIN_COUNT (int)(sizeof(k_builtin) / sizeof(k_builtin[0]))

/* Named hardware/EXEC address ranges. Sorted ascending by lo; see
 * PORTING.md Sections 5.4 (volatile-cell map) and 7.1 (STIC partial-decode
 * aliasing -- why $4000/$8000/$C000 are named "STIC alias" rather than
 * left blank: a netcode variable placed there silently aliases a STIC
 * register). Kept as a small linear-scan table -- it only runs once per
 * rendered row/line. */
static const struct { uint16_t lo, hi; const char *name; } k_regions[] = {
    { 0x0000, 0x003F, "STIC" },
    { 0x0100, 0x015C, "EXEC scratch" },
    { 0x015D, 0x01EF, "cart scratch" },
    { 0x01F0, 0x01FF, "PSG" },
    { 0x0200, 0x02EF, "BACKTAB" },
    { 0x02F0, 0x031C, "CPU stack" },
    { 0x031D, 0x035C, "EXEC object table" },
    { 0x035D, 0x035F, "EXEC globals" },
    { 0x1000, 0x1FFF, "EXEC ROM" },
    { 0x3000, 0x37FF, "GROM" },
    { 0x3800, 0x39FF, "GRAM" },
    { 0x4000, 0x4000, "STIC alias" },
    { 0x5000, 0x6FFF, "cart" },
    { 0x8000, 0x8000, "STIC alias" },
    { 0x9C00, 0x9FFF, "FujiNet mailbox" },
    { 0xC000, 0xC000, "STIC alias" },
};
#define K_REGION_COUNT (int)(sizeof(k_regions) / sizeof(k_regions[0]))

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
        snprintf(t->entries[i].note, SYM_NOTE_MAX, "%s", k_builtin[i].note);
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
    t->entries[t->count].note[0] = 0; /* notes are a built-in-only detail */
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

int intvsymtab_lookup_nearest(const intvsymtab *t, uint16_t addr, int window,
                              char *dst, int dstsz, int *delta)
{
    int i;
    int best_idx = -1;
    int best_delta = 0;

    if (!t)
        return 0;
    for (i = t->count - 1; i >= 0; i--) {
        int d = (int)addr - (int)t->entries[i].addr;
        if (d < 0 || d > window)
            continue;
        /* Newest-first scan: first candidate found at the smallest delta
         * wins ties the same way intvsymtab_lookup_addr does. Prefer a
         * strictly closer symbol over one merely found first. */
        if (best_idx < 0 || d < best_delta) {
            best_idx = i;
            best_delta = d;
            if (d == 0)
                break; /* exact match, can't do better */
        }
    }
    if (best_idx < 0)
        return 0;
    if (dst && dstsz > 0)
        snprintf(dst, (size_t)dstsz, "%s", t->entries[best_idx].name);
    if (delta)
        *delta = best_delta;
    return 1;
}

const char *intvsym_region(uint16_t addr)
{
    int i;
    for (i = 0; i < K_REGION_COUNT; i++) {
        if (addr >= k_regions[i].lo && addr <= k_regions[i].hi)
            return k_regions[i].name;
    }
    return NULL;
}

/* Finds the first "$XXXX" (1-4 hex digits) token in text starting at or
 * after *pos, advances *pos past it, and returns the parsed value. Returns
 * -1 and leaves *pos alone when there is no more to find. */
static int next_hex_operand(const char *text, int *pos)
{
    int i = *pos;
    int len = (int)strlen(text);

    while (i < len) {
        if (text[i] == '$' && i + 1 < len && isxdigit((unsigned char)text[i + 1])) {
            int j = i + 1;
            int val = 0;
            int ndigits = 0;
            while (j < len && isxdigit((unsigned char)text[j]) && ndigits < 4) {
                int c = text[j];
                int v = (c >= '0' && c <= '9') ? c - '0'
                       : (tolower(c) - 'a' + 10);
                val = (val << 4) | v;
                j++;
                ndigits++;
            }
            *pos = j;
            return val;
        }
        i++;
    }
    return -1;
}

/* Resolves one address to the best available label: an exact symbol,
 * else a nearby symbol as NAME+n, else the containing hardware/EXEC
 * region. Returns 1 and writes dst on a hit, 0 (dst untouched) when
 * nothing is known about addr at all. */
static int describe_addr(const intvsymtab *t, uint16_t addr, char *dst,
                         int dstsz)
{
    char name[SYM_NAME_MAX];
    int delta;

    if (intvsymtab_lookup_addr(t, addr, name, sizeof(name))) {
        snprintf(dst, (size_t)dstsz, "%s", name);
        return 1;
    }
    if (intvsymtab_lookup_nearest(t, addr, 64, name, sizeof(name), &delta)) {
        if (delta > 0)
            snprintf(dst, (size_t)dstsz, "%s+%d", name, delta);
        else
            snprintf(dst, (size_t)dstsz, "%s", name);
        return 1;
    }
    {
        const char *region = intvsym_region(addr);
        if (region) {
            snprintf(dst, (size_t)dstsz, "%s", region);
            return 1;
        }
    }
    return 0;
}

int intvsym_annotate_line(const intvsymtab *t, const intvdebug_dasm_line *l,
                          char *dst, int dstsz)
{
    char own[SYM_NAME_MAX];
    char operand[64];
    int pos = 0;
    int have_own, have_operand = 0;
    int operand_addr;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = 0;
    if (!t || !l)
        return 0;

    /* The line's own address: exact match only, same as the pre-existing
     * per-frontend behaviour. Falling back to a region name here would
     * label every single line inside e.g. EXEC ROM with "; EXEC ROM" --
     * region context belongs on the memory pane (intvsym_region, used by
     * refresh_mem), not repeated on every disassembly line. */
    have_own = intvsymtab_lookup_addr(t, l->addr, own, sizeof(own));

    /* Operand tokens start after the "$" that would just re-match the
     * line's own address if the mnemonic column happened to repeat it, so
     * scan the whole line text -- dasm text never includes l->addr's own
     * value as a token (jzIntv's dasm1600 only emits addresses/immediates
     * for operands, never for the instruction's own location). */
    operand_addr = next_hex_operand(l->text, &pos);
    if (operand_addr >= 0)
        have_operand =
            describe_addr(t, (uint16_t)operand_addr, operand, sizeof(operand));

    if (!have_own && !have_operand) {
        dst[0] = 0;
        return 0;
    }
    if (have_own && have_operand)
        return snprintf(dst, (size_t)dstsz, "   ; %s -> %s", own, operand);
    if (have_own)
        return snprintf(dst, (size_t)dstsz, "   ; %s", own);
    return snprintf(dst, (size_t)dstsz, "   ; %s", operand);
}
