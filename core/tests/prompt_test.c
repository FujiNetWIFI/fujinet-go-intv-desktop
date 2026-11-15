/*
 * prompt_test -- the debugger's command line, expressions and breakpoint
 * conditions with no machine behind them (so it runs in every build
 * configuration, with or without the EXEC/GROM): what the prompt says when
 * the machine is not running, what it refuses, completions, and the
 * breakpoint table's condition checks.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "intvdebug.h"
#include "intvmemtools.h"
#include "symbols.h"

static int failed = 0;

static void check(const char *what, int passed)
{
    if (!passed) {
        fprintf(stderr, "prompt_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static const char *run(intvdebug *dbg, const char *command)
{
    static char text[4096];
    intvdebug_command(dbg, command, text, sizeof(text));
    return text;
}

int main(void)
{
    intvdebug *dbg = intvdebug_get();
    intvdebug_bp spec, list[8];
    long value;
    char err[96], text[1024];
    int id;

    check("help", strstr(run(dbg, "help"), "runto") != NULL);
    check("an empty line says nothing", run(dbg, "   ")[0] == '\0');
    check("unknown", strstr(run(dbg, "frobnicate"), "unknown command") != NULL);
    check("arithmetic", strstr(run(dbg, "print 0x10 + 2 * 3"), "$0016") != NULL);
    check("binary and decimal are shown", strstr(run(dbg, "print 5"), "%0000000000000101") != NULL);
    check("an unknown name", strstr(run(dbg, "print nosuch"), "unknown name") != NULL);
    check("registers need a stopped machine", strstr(run(dbg, "print r3"), "running") != NULL);
    check("so do memory reads", strstr(run(dbg, "print [$0200]"), "running") != NULL);
    check("regs", strstr(run(dbg, "regs"), "running") != NULL);
    check("mem", strstr(run(dbg, "mem $0100"), "running") != NULL);
    check("poke", strstr(run(dbg, "poke $0200 1"), "running") != NULL);
    check("set", strstr(run(dbg, "set r0 1"), "running") != NULL);
    check("a bad register", strstr(run(dbg, "set r9 1"), "usage") != NULL);
    check("disasm", strstr(run(dbg, "disasm $5000"), "running") != NULL);
    check("map", strstr(run(dbg, "map"), "running") != NULL);
    check("sound", strstr(run(dbg, "sound"), "running") != NULL);
    check("io with no machine", strstr(run(dbg, "io"), "off") != NULL);
    check("find with no machine", strstr(run(dbg, "find $1234"), "stop the machine") != NULL);
    check("a find needs words", strstr(run(dbg, "find"), "find [hex]") != NULL);

    /* labels */
    run(dbg, "label $5000 start_here");
    check("a label is an address", strstr(run(dbg, "print start_here + 4"), "$5004") != NULL);
    check("symbols lists it", strstr(run(dbg, "symbols start_"), "start_here") != NULL);
    check("no match", strstr(run(dbg, "symbols zzzzzz"), "no symbols") != NULL);

    /* conditions: parsed and checked when the entry is added */
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_WRITE;
    spec.lo = spec.hi = 0x0200;
    snprintf(spec.condition, sizeof(spec.condition), "value == old + 1");
    id = intvdebug_bp_add(dbg, &spec);
    check("a good condition", id > 0);
    snprintf(spec.condition, sizeof(spec.condition), "value ==");
    check("a bad one", intvdebug_bp_add(dbg, &spec) < 0);
    snprintf(spec.condition, sizeof(spec.condition), "start_here < 4 || r0 == addr");
    check("labels, registers and addr", intvdebug_bp_add(dbg, &spec) > 0);
    snprintf(spec.condition, sizeof(spec.condition), "nope");
    check("an unknown name", intvdebug_bp_add(dbg, &spec) < 0);
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_ACCESS;
    spec.lo = 0x0210;
    spec.hi = 0x0200;   /* backwards */
    check("an access range is put in order", intvdebug_bp_add(dbg, &spec) > 0 &&
                                                 intvdebug_bp_list(dbg, list, 8) == 3 &&
                                                 list[2].lo == 0x0200 && list[2].hi == 0x0210 &&
                                                 list[2].type == INTVDEBUG_BP_ACCESS);
    check("the older cond is dropped when text is given", list[0].cond == INTVDEBUG_COND_ANY);
    intvbp_describe(&list[0], intvsymtab_shared(), text, sizeof(text));
    check("described with its condition", strstr(text, "Write") && strstr(text, "if value == old + 1"));
    check("breaks lists them", strstr(run(dbg, "breaks"), "Access") != NULL);
    run(dbg, "disable 1");
    check("disable", intvdebug_bp_list(dbg, list, 8) == 3 && !list[0].enabled);
    run(dbg, "delete #1");
    check("delete", intvdebug_bp_list(dbg, list, 8) == 2);
    check("delete of nothing", strstr(run(dbg, "delete 99"), "no breakpoint") != NULL);
    run(dbg, "hits");
    run(dbg, "clear");
    check("clear", intvdebug_bp_list(dbg, list, 8) == 0);

    /* a write breakpoint through the prompt, ranges and a condition */
    check("bpw", strstr(run(dbg, "bpw $0200-$0210 value > 4"), "breakpoint #") != NULL);
    check("listed", intvdebug_bp_list(dbg, list, 8) == 1 && list[0].lo == 0x0200 && list[0].hi == 0x0210 &&
                        strcmp(list[0].condition, "value > 4") == 0);
    intvdebug_breakpoint_clear_all(dbg);

    /* the watch list */
    check("watch", strstr(run(dbg, "watch $0150"), "$0150") != NULL);
    run(dbg, "unwatch all");
    check("unwatch", intvwatch_count() == 0);

    /* evaluation straight through the API */
    check("eval", intvdebug_eval(dbg, "(1 + 2) << 4", &value, err, sizeof(err)) == 0 && value == 48);
    check("eval of a register while running", intvdebug_eval(dbg, "pc", &value, err, sizeof(err)) != 0 &&
                                                  strstr(err, "running") != NULL);

    /* completions */
    check("commands", intvdebug_completions(dbg, "wat", text, sizeof(text)) == 1 && strstr(text, "watch") != NULL);
    check("registers", intvdebug_completions(dbg, "print p", text, sizeof(text)) >= 1 && strstr(text, "pc") != NULL);
    check("labels", intvdebug_completions(dbg, "runto start_", text, sizeof(text)) == 1);
    check("save kinds", intvdebug_completions(dbg, "save g", text, sizeof(text)) == 1 && strstr(text, "gram") != NULL);

    /* stop text and the snapshot when nothing has run */
    intvdebug_stop_text(dbg, text, sizeof(text), &id);
    check("running", strcmp(text, "running") == 0 && id == -1);
    check("snapshot", intvdebug_stop_snapshot(dbg, &(intvdebug_snapshot){ 0 }) == 0);

    if (failed) {
        fprintf(stderr, "prompt_test: FAILED\n");
        return 1;
    }
    printf("prompt_test: OK\n");
    return 0;
}
