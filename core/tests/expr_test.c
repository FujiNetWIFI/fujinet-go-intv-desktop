/*
 * expr_test -- the debugger's expression evaluator on its own: numbers in
 * every notation, names, memory reads, C precedence and its errors. No
 * machine.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "expr.h"

static int failed = 0;

static void check(const char *what, int parsed)
{
    if (!parsed) {
        fprintf(stderr, "expr_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static int name_of(void *user, const char *name, long *out)
{
    (void)user;
    if (!strcmp(name, "r0")) { *out = 0x1234; return 1; }
    if (!strcmp(name, "pc")) { *out = 0x5000; return 1; }
    if (!strcmp(name, "var_X")) { *out = 0x010C; return 1; }
    return 0;
}

static uint16_t word_at(void *user, uint16_t addr)
{
    (void)user;
    return (uint16_t)(addr ^ 0xFFFF);   /* any recognisable function of the address */
}

static long value_of(const char *text, int *parsed)
{
    const intvexpr_ctx ctx = { name_of, word_at, NULL };
    long result = 0;
    char err[64];
    *parsed = intvexpr_eval(text, &ctx, &result, err, sizeof(err)) == 0;
    return result;
}

static int is_error(const char *text, const char *needle)
{
    const intvexpr_ctx ctx = { name_of, word_at, NULL };
    long result;
    char err[64];
    return intvexpr_eval(text, &ctx, &result, err, sizeof(err)) != 0 && strstr(err, needle) != NULL;
}

int main(void)
{
    int parsed;

    check("hex with $", value_of("$5000", &parsed) == 0x5000 && parsed);
    check("hex with 0x", value_of("0x5000", &parsed) == 0x5000 && parsed);
    check("hex with h", value_of("5000h", &parsed) == 0x5000 && parsed);
    check("decimal", value_of("20480", &parsed) == 20480 && parsed);
    check("binary", value_of("%1010", &parsed) == 10 && parsed);
    check("names", value_of("r0 + 1", &parsed) == 0x1235 && parsed);
    check("labels", value_of("var_X", &parsed) == 0x010C && parsed);
    check("a word of memory", value_of("[$0200]", &parsed) == 0xFDFF && parsed);
    check("memory of an expression", value_of("[r0 - 1]", &parsed) == (0x1233 ^ 0xFFFF) && parsed);
    check("precedence", value_of("1 + 2 * 3", &parsed) == 7 && parsed);
    check("parentheses", value_of("(1 + 2) * 3", &parsed) == 9 && parsed);
    check("shifts", value_of("1 << 4 | 3", &parsed) == 19 && parsed);
    check("shift counts out of range are zero", value_of("1 << 99", &parsed) == 0 && parsed);
    check("relations", value_of("3 < 4 && 4 >= 4 && 5 != 6", &parsed) == 1 && parsed);
    check("modulo is not a binary literal", value_of("7 % 4", &parsed) == 3 && parsed);
    check("unary", value_of("-5 + ~0 + !0", &parsed) == -5 && parsed);
    check("division", value_of("20 / 3", &parsed) == 6 && parsed);
    check("division by zero", is_error("1 / 0", "division"));
    check("an unknown name", is_error("nosuch", "nosuch"));
    check("a missing )", is_error("(1 + 2", ")"));
    check("a missing ]", is_error("[1", "]"));
    check("trailing text", is_error("1 2", "unexpected"));
    check("empty", is_error("", "expected"));
    check("no context: names are 0", intvexpr_eval("nosuch + 1", NULL, &(long){0}, NULL, 0) == 0);

    if (failed) {
        fprintf(stderr, "expr_test: FAILED\n");
        return 1;
    }
    printf("expr_test: OK\n");
    return 0;
}
