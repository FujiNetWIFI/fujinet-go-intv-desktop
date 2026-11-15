/*
 * expr.c -- a recursive-descent evaluator for the grammar in expr.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"

typedef struct {
    const char *cur;
    const intvexpr_ctx *ctx;
    char *err;
    int errsz;
    int failed;
} parser;

static void fail(parser *prs, const char *msg)
{
    if (!prs->failed && prs->err && prs->errsz > 0)
        snprintf(prs->err, (size_t)prs->errsz, "%s", msg);
    prs->failed = 1;
}

static void skip(parser *prs)
{
    while (*prs->cur && isspace((unsigned char)*prs->cur))
        prs->cur++;
}

static int accept(parser *prs, const char *tok)
{
    size_t len = strlen(tok);
    skip(prs);
    if (strncmp(prs->cur, tok, len) == 0)
    {
        prs->cur += len;
        return 1;
    }
    return 0;
}

static long expr_or(parser *prs);

static int is_name_start(char chr)
{
    return isalpha((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@';
}

static int is_name_char(char chr)
{
    return isalnum((unsigned char)chr) || chr == '_' || chr == '.' || chr == '@' || chr == '\'';
}

static long number(parser *prs)
{
    const char *str = prs->cur;
    char *end;
    long val;

    if (*str == '$')
    {
        val = strtol(str + 1, &end, 16);
        if (end == str + 1)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    if (*str == '%')
    {
        val = strtol(str + 1, &end, 2);
        if (end == str + 1)
            fail(prs, "bad binary number");
        prs->cur = end;
        return val;
    }
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
    {
        val = strtol(str + 2, &end, 16);
        if (end == str + 2)
            fail(prs, "bad hex number");
        prs->cur = end;
        return val;
    }
    /* digits: hex with an h suffix (C000h must start with a digit, 0C000h),
     * else decimal */
    {
        const char *look = str;
        while (isxdigit((unsigned char)*look))
            look++;
        if ((*look == 'h' || *look == 'H') && !is_name_char(look[1]))
        {
            val = strtol(str, NULL, 16);
            prs->cur = look + 1;
            return val;
        }
    }
    val = strtol(str, &end, 10);
    prs->cur = end;
    return val;
}

static long primary(parser *prs)
{
    long val;

    skip(prs);
    if (accept(prs, "("))
    {
        val = expr_or(prs);
        if (!accept(prs, ")"))
            fail(prs, "missing )");
        return val;
    }
    if (accept(prs, "["))
    {
        long addr = expr_or(prs);
        if (!accept(prs, "]"))
            fail(prs, "missing ]");
        return prs->ctx && prs->ctx->read_word ? prs->ctx->read_word(prs->ctx->user, (uint16_t)addr) : 0;
    }
    if (isdigit((unsigned char)*prs->cur) || *prs->cur == '$' || *prs->cur == '%')
        return number(prs);
    if (is_name_start(*prs->cur))
    {
        char name[64];
        int len = 0;
        while (is_name_char(*prs->cur) && len + 1 < (int)sizeof name)
            name[len++] = *prs->cur++;
        name[len] = '\0';
        if (!prs->ctx)
            return 0;
        if (prs->ctx->name && prs->ctx->name(prs->ctx->user, name, &val))
            return val;
        {
            char msg[96];
            snprintf(msg, sizeof msg, "unknown name '%s'", name);
            fail(prs, msg);
        }
        return 0;
    }
    if (*prs->cur)
        fail(prs, "unexpected character");
    else
        fail(prs, "expression expected");
    return 0;
}

static long unary(parser *prs)
{
    if (accept(prs, "-"))
        return -unary(prs);
    if (accept(prs, "!"))
        return !unary(prs);
    if (accept(prs, "~"))
        return ~unary(prs);
    if (accept(prs, "+"))
        return unary(prs);
    return primary(prs);
}

static long mul(parser *prs)
{
    long val = unary(prs);
    for (;;)
    {
        if (accept(prs, "*"))
            val *= unary(prs);
        else if (accept(prs, "/"))
        {
            long den = unary(prs);
            if (den == 0)
            {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? -val : val / den;
        }
        else if (accept(prs, "%"))
        {
            long den = unary(prs);
            if (den == 0)
            {
                fail(prs, "division by zero");
                return 0;
            }
            val = den == -1 ? 0 : val % den;
        }
        else
            return val;
    }
}

static long add(parser *prs)
{
    long val = mul(prs);
    for (;;)
    {
        if (accept(prs, "+"))
            val += mul(prs);
        else if (accept(prs, "-"))
            val -= mul(prs);
        else
            return val;
    }
}

static long shift(parser *prs)
{
    long val = add(prs);
    for (;;)
    {
        if (accept(prs, "<<"))
        {
            long count = add(prs);
            val = count < 0 || count > 62 ? 0 : (long)((unsigned long)val << count);
        }
        else if (accept(prs, ">>"))
        {
            long count = add(prs);
            val = count < 0 || count > 62 ? 0 : val >> count;
        }
        else
            return val;
    }
}

static long rel(parser *prs)
{
    long val = shift(prs);
    for (;;)
    {
        if (accept(prs, "<="))
            val = val <= shift(prs);
        else if (accept(prs, ">="))
            val = val >= shift(prs);
        else if (strncmp((skip(prs), prs->cur), "<<", 2) != 0 && accept(prs, "<"))
            val = val < shift(prs);
        else if (strncmp((skip(prs), prs->cur), ">>", 2) != 0 && accept(prs, ">"))
            val = val > shift(prs);
        else
            return val;
    }
}

static long equality(parser *prs)
{
    long val = rel(prs);
    for (;;)
    {
        if (accept(prs, "=="))
            val = val == rel(prs);
        else if (accept(prs, "!="))
            val = val != rel(prs);
        else
            return val;
    }
}

static long band(parser *prs)
{
    long val = equality(prs);
    for (;;)
    {
        skip(prs);
        if (prs->cur[0] == '&' && prs->cur[1] != '&')
        {
            prs->cur++;
            val &= equality(prs);
        }
        else
            return val;
    }
}

static long bxor(parser *prs)
{
    long val = band(prs);
    while (accept(prs, "^"))
        val ^= band(prs);
    return val;
}

static long bor(parser *prs)
{
    long val = bxor(prs);
    for (;;)
    {
        skip(prs);
        if (prs->cur[0] == '|' && prs->cur[1] != '|')
        {
            prs->cur++;
            val |= bxor(prs);
        }
        else
            return val;
    }
}

static long land(parser *prs)
{
    long val = bor(prs);
    while (accept(prs, "&&"))
    {
        long rhs = bor(prs);
        val = val && rhs;
    }
    return val;
}

static long expr_or(parser *prs)
{
    long val = land(prs);
    while (accept(prs, "||"))
    {
        long rhs = land(prs);
        val = val || rhs;
    }
    return val;
}

int intvexpr_eval(const char *text, const intvexpr_ctx *ctx, long *out, char *err, int errsz)
{
    parser prs;
    long val;

    memset(&prs, 0, sizeof prs);
    prs.cur = text ? text : "";
    prs.ctx = ctx;
    prs.err = err;
    prs.errsz = errsz;
    if (err && errsz > 0)
        err[0] = '\0';

    val = expr_or(&prs);
    skip(&prs);
    if (!prs.failed && *prs.cur)
        fail(&prs, "unexpected text after the expression");
    if (prs.failed)
        return -1;
    if (out)
        *out = val;
    return 0;
}
