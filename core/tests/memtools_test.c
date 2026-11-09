/*
 * memtools_test -- the pure parts of the watch list, range parsing, search
 * queries and breakpoint descriptions (no machine needed).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memtools.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "memtools_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    intvsymtab *t = intvsymtab_create();
    uint16_t lo, hi, pat[64], mask;
    int width, flags, n;
    char line[160];

    intvsymtab_add(t, 0x010C, "var_X");
    intvsymtab_add(t, 0x02F0, "var_&SCORE");
    intvsymtab_add(t, 0x0120, "array_ARR");
    intvsymtab_add(t, 0x0128, "var_Z");

    /* ---- ranges ---- */
    check("a variable", intvsym_parse_range(t, "x", &lo, &hi, &width) && lo == 0x010C && hi == lo && width == 8);
    check("a 16-bit variable", intvsym_parse_range(t, "#score", &lo, &hi, &width) && lo == 0x02F0 && width == 16);
    check("a whole array", intvsym_parse_range(t, "arr", &lo, &hi, &width) && lo == 0x0120 && hi == 0x0127);
    check("an element", intvsym_parse_range(t, "arr(2)", &lo, &hi, NULL) && lo == 0x0122 && hi == 0x0122);
    check("lo-hi", intvsym_parse_range(t, "$100-$10F", &lo, &hi, NULL) && lo == 0x0100 && hi == 0x010F);
    check("lo-hi backwards", intvsym_parse_range(t, "10F-100", &lo, &hi, NULL) && lo == 0x0100 && hi == 0x010F);
    check("symbols in a range", intvsym_parse_range(t, "x-z", &lo, &hi, NULL) && lo == 0x010C && hi == 0x0128);
    check("a bare hex address", intvsym_parse_range(t, "  $5000 ", &lo, &hi, &width) && lo == 0x5000 && hi == lo && width == 16);
    check("scratchpad addresses are 8 bits", intvsym_parse_range(t, "$0150", &lo, &hi, &width) && width == 8);
    check("nonsense", !intvsym_parse_range(t, "", &lo, &hi, NULL) && !intvsym_parse_range(t, "zzz!", &lo, &hi, NULL));

    /* ---- the watch list ---- */
    intvwatch_clear();
    check("empty", intvwatch_count() == 0);
    check("add", intvwatch_add("x") == 0 && intvwatch_add("  #score ") == 1 && intvwatch_add("nosuch") == 2);
    check("blank refused", intvwatch_add("   ") == -1 && intvwatch_add("") == -1);
    check("text is trimmed", strcmp(intvwatch_text(1), "#score") == 0);
    check("generation moves", intvwatch_generation() > 0);
    check("range of a watch", intvwatch_range(t, 0, &lo, &hi) && lo == 0x010C);
    check("an unresolved watch has no range", !intvwatch_range(t, 2, &lo, &hi));
    intvwatch_format(NULL, t, 0, line, sizeof(line));
    check("shown while running as a dash", strstr(line, "$010C") && strstr(line, "   -"));
    intvwatch_remove(0);
    check("remove shifts", intvwatch_count() == 2 && strcmp(intvwatch_text(0), "#score") == 0);
    intvwatch_clear();
    check("clear", intvwatch_count() == 0);

    /* ---- search queries ---- */
    n = intvsearch_parse(INTVSEARCH_HEX, "02A0 $0268, 0x1000", pat, &mask, &flags);
    check("hex words", n == 3 && pat[0] == 0x02A0 && pat[1] == 0x0268 && pat[2] == 0x1000 && mask == 0xFFFF && flags == 0);
    check("hex too long refused", intvsearch_parse(INTVSEARCH_HEX, "12345", pat, &mask, &flags) == 0);
    check("hex garbage refused", intvsearch_parse(INTVSEARCH_HEX, "12 zz", pat, &mask, &flags) == 0);
    check("hex empty", intvsearch_parse(INTVSEARCH_HEX, "  ", pat, &mask, &flags) == 0);
    n = intvsearch_parse(INTVSEARCH_TEXT, "Hi!", pat, &mask, &flags);
    check("text", n == 3 && pat[0] == 'H' && pat[2] == '!' && mask == 0x00FF && (flags & INTVDEBUG_SEARCH_NOCASE));

    /* ---- breakpoint descriptions ---- */
    {
        intvdebug_bp bp;
        memset(&bp, 0, sizeof(bp));
        bp.type = INTVDEBUG_BP_WRITE;
        bp.lo = bp.hi = 0x010C;
        bp.cond = INTVDEBUG_COND_CHANGED;
        intvbp_describe(&bp, t, line, sizeof(line));
        check("write, symbol, condition", strstr(line, "Write") && strstr(line, "$010C") &&
                                              strstr(line, "var_X") && strstr(line, "if changed"));
        bp.type = INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE;
        bp.lo = 0x0100;
        bp.hi = 0x010F;
        bp.cond = INTVDEBUG_COND_ANY;
        intvbp_describe(&bp, t, line, sizeof(line));
        check("access range", strstr(line, "Access") && strstr(line, "$0100-$010F"));
        bp.type = INTVDEBUG_BP_EXEC;
        bp.lo = bp.hi = 0x50C8;
        intvbp_describe(&bp, NULL, line, sizeof(line));
        check("execute", strstr(line, "Execute") && strstr(line, "$50C8"));
    }

    intvsymtab_destroy(t);
    if (failed) {
        fprintf(stderr, "memtools_test: FAILED\n");
        return 1;
    }
    printf("memtools_test: OK\n");
    return 0;
}
