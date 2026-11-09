/*
 * See memtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memtools.h"

/* ---- ranges -------------------------------------------------------------------- */

int intvsym_parse_range(const intvsymtab *t, const char *text, uint16_t *lo,
                        uint16_t *hi, int *width)
{
    char buf[128];
    const char *dash = NULL, *p;
    intvsym_var var;
    uint16_t a, b;
    size_t len;

    if (!text)
        return 0;
    while (isspace((unsigned char)*text))
        text++;
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1]))
        len--;
    if (len == 0 || len >= sizeof(buf))
        return 0;
    memcpy(buf, text, len);
    buf[len] = 0;

    /* lo-hi: the dash must not be the first character */
    for (p = buf + 1; *p; p++)
        if (*p == '-') {
            dash = p;
            break;
        }
    if (dash) {
        char left[128];
        size_t n = (size_t)(dash - buf);
        memcpy(left, buf, n);
        left[n] = 0;
        if (!intvsym_parse_addr(t, left, &a) || !intvsym_parse_addr(t, dash + 1, &b))
            return 0;
        if (b < a) {
            const uint16_t x = a;
            a = b;
            b = x;
        }
        *lo = a;
        *hi = b;
        if (width)
            *width = 16;
        return 1;
    }
    if (t && intvsym_resolve_basic(t, buf, &var)) {
        *lo = var.addr;
        *hi = (uint16_t)(var.is_array && !strchr(buf, '(') ? var.addr + var.count - 1 : var.addr);
        if (width)
            *width = var.width;
        return 1;
    }
    if (!intvsym_parse_addr(t, buf, &a))
        return 0;
    *lo = *hi = a;
    if (width)
        *width = a >= 0x0100 && a <= 0x01EF ? 8 : 16;
    return 1;
}

/* ---- watches -------------------------------------------------------------------- */

static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;
static char g_watch[INTVWATCH_MAX][64];
static int g_nwatch;
static unsigned g_wgen;

int intvwatch_count(void)
{
    int n;
    pthread_mutex_lock(&g_wlock);
    n = g_nwatch;
    pthread_mutex_unlock(&g_wlock);
    return n;
}

int intvwatch_add(const char *text)
{
    int i = -1;
    size_t len;

    if (!text)
        return -1;
    while (isspace((unsigned char)*text))
        text++;
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1]))
        len--;
    if (len == 0 || len >= sizeof(g_watch[0]))
        return -1;
    pthread_mutex_lock(&g_wlock);
    if (g_nwatch < INTVWATCH_MAX) {
        i = g_nwatch++;
        memcpy(g_watch[i], text, len);
        g_watch[i][len] = 0;
        g_wgen++;
    }
    pthread_mutex_unlock(&g_wlock);
    return i;
}

void intvwatch_remove(int index)
{
    pthread_mutex_lock(&g_wlock);
    if (index >= 0 && index < g_nwatch) {
        memmove(g_watch[index], g_watch[index + 1],
                (size_t)(g_nwatch - index - 1) * sizeof(g_watch[0]));
        g_nwatch--;
        g_wgen++;
    }
    pthread_mutex_unlock(&g_wlock);
}

void intvwatch_clear(void)
{
    pthread_mutex_lock(&g_wlock);
    g_nwatch = 0;
    g_wgen++;
    pthread_mutex_unlock(&g_wlock);
}

const char *intvwatch_text(int index)
{
    const char *s = NULL;
    pthread_mutex_lock(&g_wlock);
    if (index >= 0 && index < g_nwatch)
        s = g_watch[index];   /* stable until the next remove */
    pthread_mutex_unlock(&g_wlock);
    return s;
}

unsigned intvwatch_generation(void)
{
    return g_wgen;
}

int intvwatch_range(const intvsymtab *t, int index, uint16_t *lo, uint16_t *hi)
{
    char text[64];
    const char *s = intvwatch_text(index);
    if (!s)
        return 0;
    snprintf(text, sizeof(text), "%s", s);
    return intvsym_parse_range(t, text, lo, hi, NULL);
}

int intvwatch_format(intvdebug *d, const intvsymtab *t, int index, char *dst, int dstsz)
{
    char text[64];
    uint16_t lo, hi, words[8];
    int width, n, i, len;
    const char *s = intvwatch_text(index);

    if (!s)
        return snprintf(dst, (size_t)dstsz, "");
    snprintf(text, sizeof(text), "%s", s);
    if (!intvsym_parse_range(t, text, &lo, &hi, &width))
        return snprintf(dst, (size_t)dstsz, "%-18s ?  (not a symbol, variable or address)", text);
    len = snprintf(dst, (size_t)dstsz, "%-18s $%04X", text, lo);
    if (hi != lo)
        len += snprintf(dst + len, (size_t)(dstsz > len ? dstsz - len : 0), "-$%04X", hi);
    n = (int)hi - (int)lo + 1;
    if (n > 8)
        n = 8;
    if (!d || !intvdebug_is_paused(d) || intvdebug_read(d, lo, words, n) != n)
        return len + snprintf(dst + len, (size_t)(dstsz > len ? dstsz - len : 0), "   -");
    len += snprintf(dst + len, (size_t)(dstsz > len ? dstsz - len : 0), "   ");
    for (i = 0; i < n && len < dstsz - 1; i++) {
        const unsigned v = width == 8 ? (words[i] & 0xFFu) : words[i];
        if (n == 1)
            len += snprintf(dst + len, (size_t)(dstsz - len), width == 8 ? "$%02X (%u)" : "$%04X (%u)", v, v);
        else
            len += snprintf(dst + len, (size_t)(dstsz - len), width == 8 ? "%02X " : "%04X ", v);
    }
    if ((int)hi - (int)lo + 1 > 8 && len < dstsz - 4)
        len += snprintf(dst + len, (size_t)(dstsz - len), "...");
    return len;
}

/* ---- search queries ------------------------------------------------------------------- */

int intvsearch_parse(int kind, const char *text, uint16_t *pattern, uint16_t *mask, int *flags)
{
    int n = 0;

    if (!text)
        return 0;
    if (kind == INTVSEARCH_TEXT) {
        for (; *text && n < 64; text++)
            pattern[n++] = (unsigned char)*text;
        *mask = 0x00FF;
        *flags = INTVDEBUG_SEARCH_NOCASE;
        return n;
    }
    while (*text && n < 64) {
        char *end;
        unsigned long v;
        while (isspace((unsigned char)*text) || *text == ',')
            text++;
        if (!*text)
            break;
        if (*text == '$')
            text++;
        else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            text += 2;
        if (!isxdigit((unsigned char)*text))
            return 0;
        v = strtoul(text, &end, 16);
        if (end - text > 4 || v > 0xFFFF)
            return 0;
        pattern[n++] = (uint16_t)v;
        text = end;
        if (*text && !isspace((unsigned char)*text) && *text != ',')
            return 0;
    }
    *mask = 0xFFFF;
    *flags = 0;
    return n;
}

int intvsearch_next(intvdebug *d, const uint16_t *pattern, int n, uint16_t mask, int flags,
                    uint16_t from, int backward, uint16_t *found)
{
    uint16_t hits[1024];
    int count;

    if (!intvdebug_is_paused(d) || n < 1)
        return 0;
    if (!backward) {
        /* from..$FFFF, then $0000..from-1 */
        count = intvdebug_search(d, pattern, n, mask, flags, from, 0xFFFF, hits, 1);
        if (count < 1 && from > 0)
            count = intvdebug_search(d, pattern, n, mask, flags, 0, (uint16_t)(from - 1), hits, 1);
        if (count < 1)
            return 0;
        *found = hits[0];
        return 1;
    }
    /* the last match before `from`: scan the range and keep the final hit */
    {
        uint16_t last = 0;
        int have = 0, k;
        if (from > 0) {
            count = intvdebug_search(d, pattern, n, mask, flags, 0, (uint16_t)(from - 1), hits, 1024);
            for (k = 0; k < count; k++) {
                last = hits[k];
                have = 1;
            }
        }
        if (!have) {   /* wrap: the last match in the rest of memory */
            count = intvdebug_search(d, pattern, n, mask, flags, from, 0xFFFF, hits, 1024);
            for (k = 0; k < count; k++) {
                last = hits[k];
                have = 1;
            }
        }
        if (!have)
            return 0;
        *found = last;
        return 1;
    }
}

/* ---- describing a breakpoint ------------------------------------------------------------- */

int intvbp_describe(const intvdebug_bp *bp, const intvsymtab *t, char *dst, int dstsz)
{
    const char *type;
    char sym[64] = "", cond[48] = "";
    int len;

    switch (bp->type & (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE)) {
    case INTVDEBUG_BP_READ: type = "Read"; break;
    case INTVDEBUG_BP_WRITE: type = "Write"; break;
    case INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE: type = "Access"; break;
    default: type = "Execute"; break;
    }
    if (t && bp->lo == bp->hi) {
        char name[48];
        if (intvsymtab_lookup_addr(t, bp->lo, name, sizeof(name)))
            snprintf(sym, sizeof(sym), "  %s", name);
    }
    if (bp->type & INTVDEBUG_BP_WRITE) {
        const unsigned mask = bp->mask ? bp->mask : 0xFFFF;
        if (bp->cond == INTVDEBUG_COND_CHANGED)
            snprintf(cond, sizeof(cond), "  if changed");
        else if (bp->cond == INTVDEBUG_COND_EQ)
            snprintf(cond, sizeof(cond), "  if = $%02X", bp->value & mask);
        else if (bp->cond == INTVDEBUG_COND_NE)
            snprintf(cond, sizeof(cond), "  if != $%02X", bp->value & mask);
    }
    len = snprintf(dst, (size_t)dstsz, "%-7s $%04X", type, bp->lo);
    if (bp->hi != bp->lo)
        len += snprintf(dst + len, (size_t)(dstsz > len ? dstsz - len : 0), "-$%04X", bp->hi);
    len += snprintf(dst + len, (size_t)(dstsz > len ? dstsz - len : 0), "%s%s", sym, cond);
    return len;
}
