/*
 * engine_test -- the debugger engine's newer parts on a real running machine:
 * expression conditions on access and execute stops, `old` through the watch
 * device's place on the bus, run_to, the stop text and its width, the stop
 * snapshot, registers, history after a breakpoint stop, the branches filter,
 * the memory map, symbols, the prompt, and the decoder's fetches not showing
 * up as reads.
 *
 * Boots tests/data/srcmap/main.bin (x = x + 1 each frame; x is var_X at $010C)
 * and, at the end, hijacks the CPU with a few words poked into system RAM.
 * Needs the embedded EXEC/GROM; skips (77) without.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "machine.h"

#include "intv_host.h"
#include "intvdebug.h"
#include "intvmemtools.h"
#include "roms_embedded.h"
#include "symbols.h"
#include "test_tmpdir.h"

#define VAR_X 0x010C
#define LOOP_TOP 0x50C8   /* label_MAIN: MVI var_X,R0 */
#define PROC_BEGIN 0x50D5 /* label_BUMP: BEGIN */

static int failed = 0;

static void check(const char *what, int passed)
{
    if (!passed) {
        fprintf(stderr, "engine_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static int wait_paused(intvdebug *dbg, int timeout_ms)
{
    int waited = 0;
    while (!intvdebug_is_paused(dbg) && waited < timeout_ms) {
        usleep(1000);
        waited++;
    }
    return intvdebug_is_paused(dbg);
}

static uint16_t peek(intvdebug *dbg, uint16_t addr)
{
    uint16_t value = 0xFFFF;
    intvdebug_read(dbg, addr, &value, 1);
    return value;
}

static void poke(intvdebug *dbg, uint16_t addr, uint16_t value)
{
    intvdebug_write(dbg, addr, &value, 1);
}

/* Resumes and waits for the next stop. */
static int run_stop(intvdebug *dbg, int timeout_ms)
{
    intvdebug_resume(dbg);
    return wait_paused(dbg, timeout_ms);
}

static int run_command(intvdebug *dbg, const char *command, char *dst, int dstsz)
{
    intvdebug_command(dbg, command, dst, dstsz);
    return dst[0] != '\0';
}

int main(void)
{
    char rom_dir[1024], text[4096];
    intvdebug_stop_info stop;
    intvdebug_snapshot snap;
    intvdebug_regs regs;
    intvdebug_bp spec;
    intvdebug_bp list[16];
    long value;
    char err[96];
    int id;

    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "engine_test: no embedded ROMs (build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-engine-test-");
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65506,
        .cart_path = SRCMAP_FIXTURE_DIR "/main.bin",
    };
    if (intv_host_start(&opts) != 0) {
        fprintf(stderr, "engine_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000);

    intvdebug *dbg = intvdebug_get();
    intvdebug_set_engaged(dbg, 1);
    intvdebug_pause(dbg);
    check("paused", wait_paused(dbg, 2000));

    /* ---- widths ---- */
    check("scratchpad is 8 bits", intvdebug_word_width(NULL, VAR_X) == 8);
    check("GRAM is 8 bits", intvdebug_word_width(NULL, 0x3800) == 8);
    check("system RAM is 16 bits", intvdebug_word_width(NULL, 0x0200) == 16);
    check("cartridge space is 16 bits", intvdebug_word_width(NULL, 0x5000) == 16);

    /* ---- registers ---- */
    check("regs", intvdebug_regs_get(dbg, &regs) && regs.cycles > 0);
    check("set R0", intvdebug_regs_set(dbg, INTVDEBUG_REG_R0, 0x1234) && intvdebug_regs_get(dbg, &regs) &&
                        regs.r[0] == 0x1234);
    check("set a flag", intvdebug_regs_set(dbg, INTVDEBUG_REG_C, 5) && intvdebug_regs_get(dbg, &regs) && regs.C == 1);
    check("clear it", intvdebug_regs_set(dbg, INTVDEBUG_REG_C, 0) && intvdebug_regs_get(dbg, &regs) && regs.C == 0);
    check("SDBD pending", intvdebug_regs_set(dbg, INTVDEBUG_REG_D, 9) && intvdebug_regs_get(dbg, &regs) &&
                              regs.D == 1 && !regs.interruptible);
    intvdebug_regs_set(dbg, INTVDEBUG_REG_D, 0);
    check("an unknown register", !intvdebug_regs_set(dbg, 99, 1) && !intvdebug_regs_set(dbg, -1, 1));
    check("the PC register moves the PC", intvdebug_regs_get(dbg, &regs) && intvdebug_regs_set(dbg, INTVDEBUG_REG_PC, regs.pc));

    /* ---- the watch device is first on the bus, and stays first ---- */
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_WRITE;
    spec.lo = spec.hi = VAR_X;
    spec.cond = INTVDEBUG_COND_CHANGED;
    id = intvdebug_bp_add(dbg, &spec);
    check("a changed watch", id > 0);
    check("it answers first in its bin", intv.intv->wr[0][VAR_X >> 4] != NULL &&
                                             !strcmp(intv.intv->wr[0][VAR_X >> 4]->name, "watch") &&
                                             !strcmp(intv.intv->rd[0][VAR_X >> 4]->name, "watch"));
    /* the cartridge re-registering over the map (a FujiNet hot swap) puts its
     * devices after ours: simulate it with the scratchpad's own */
    periph_unregister(intv.intv, &intv.scr_ram.periph);
    periph_register(intv.intv, &intv.scr_ram.periph, 0x0100, 0x01EF, "Scratch RAM");
    check("and still does after a re-registration", !strcmp(intv.intv->wr[0][VAR_X >> 4]->name, "watch"));
    check("the scratchpad is still behind it", intv.intv->wr[1][VAR_X >> 4] != NULL);
    check("stops", run_stop(dbg, 15000));
    check("last_stop", intvdebug_last_stop(dbg, &stop) && stop.reason == INTVDEBUG_STOP_WATCH_WRITE);
    check("old is the value before the store", ((stop.old_value + 1) & 0xFF) == (stop.value & 0xFF));
    check("pc and the writing instruction", stop.pc == 0x50CD && stop.from_pc == 0x50CB);
    {
        char want[96];
        int address = 0;
        intvdebug_stop_text(dbg, text, sizeof(text), &address);
        snprintf(want, sizeof(want), "wrote $010C = $%02X (was $%02X) at $50CB", stop.value & 0xFF,
                 stop.old_value & 0xFF);
        check("the 8-bit stop text", strcmp(text, want) == 0);
        check("and the address to show", address == 0x50CD);
    }
    intvdebug_bp_remove(dbg, id);

    /* ---- the stop snapshot agrees with itself ---- */
    check("snapshot", intvdebug_stop_snapshot(dbg, &snap) == 1 && snap.paused && snap.serial == snap.stop.serial &&
                          snap.serial == intvdebug_stop_serial(dbg) && snap.stop.reason == INTVDEBUG_STOP_WATCH_WRITE &&
                          strstr(snap.text, "wrote $010C") != NULL && snap.address == 0x50CD);
    check("generation moved", intvdebug_generation(dbg) > 0);

    /* ---- a stop is complete the instant paused flips ---- */
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_EXEC;
    spec.lo = spec.hi = 0x1004;   /* the interrupt vector: 60 times a second */
    id = intvdebug_bp_add(dbg, &spec);
    check("vector breakpoint", id > 0);
    {
        int round, torn = 0;
        for (round = 0; round < 12; round++) {
            intvdebug_resume(dbg);
            while (!intvdebug_is_paused(dbg))
                ;   /* spin: look at it the moment it flips */
            if (!(intvdebug_last_stop(dbg, &stop) && stop.serial == intvdebug_stop_serial(dbg) &&
                  stop.reason == INTVDEBUG_STOP_BREAKPOINT && stop.pc == 0x1004 && stop.bp_id == id))
                torn++;
        }
        check("the reason, PC and serial are there when paused is", torn == 0);
    }
    intvdebug_bp_remove(dbg, id);

    /* ---- expression conditions ---- */
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_WRITE;
    spec.lo = spec.hi = VAR_X;
    snprintf(spec.condition, sizeof(spec.condition), "value ==");
    check("a condition that does not parse is refused", intvdebug_bp_add(dbg, &spec) < 0);
    snprintf(spec.condition, sizeof(spec.condition), "nosuch == 1");
    check("so is one that names nothing", intvdebug_bp_add(dbg, &spec) < 0);
    snprintf(spec.condition, sizeof(spec.condition), "value == old + 1 && value >= 3 && r7 != 0");
    id = intvdebug_bp_add(dbg, &spec);
    check("a good one is taken", id > 0 && intvdebug_bp_list(dbg, list, 16) == 1 &&
                                     strcmp(list[0].condition, spec.condition) == 0);
    check("it can be replaced", intvdebug_bp_set_condition(dbg, id, "value >= 3") &&
                                    !intvdebug_bp_set_condition(dbg, id, "value >=") &&
                                    !intvdebug_bp_set_condition(dbg, 9999, "1"));
    intvdebug_bp_set_condition(dbg, id, "value == old + 1 && value >= 3");
    check("stops on it", run_stop(dbg, 15000));
    check("with the right value", intvdebug_last_stop(dbg, &stop) && stop.value >= 3 &&
                                      stop.old_value + 1 == stop.value);
    intvdebug_bp_remove(dbg, id);

    check("eval a register", intvdebug_regs_get(dbg, &regs) && intvdebug_eval(dbg, "r7 == pc", &value, err, sizeof(err)) == 0 &&
                                 value == 1);
    check("eval memory", intvdebug_eval(dbg, "[$010C] + 1", &value, err, sizeof(err)) == 0 &&
                             value == (long)(peek(dbg, VAR_X) + 1));
    intvsymtab_add(intvsymtab_shared(), LOOP_TOP, "loop_top");
    check("eval a label", intvdebug_eval(dbg, "loop_top", &value, err, sizeof(err)) == 0 && value == LOOP_TOP);
    check("value is only defined in a condition", intvdebug_eval(dbg, "value", &value, err, sizeof(err)) != 0);

    /* an execute breakpoint with a condition */
    {
        const uint16_t start = peek(dbg, VAR_X) & 0xFF;
        memset(&spec, 0, sizeof(spec));
        spec.type = INTVDEBUG_BP_EXEC;
        spec.lo = spec.hi = LOOP_TOP;
        snprintf(spec.condition, sizeof(spec.condition), "[$010C] >= %u", start + 2);
        id = intvdebug_bp_add(dbg, &spec);
        check("conditional execute breakpoint", id > 0);
        check("the first stop is the one whose condition holds", run_stop(dbg, 15000));
        check("at the loop top, not before", intvdebug_regs_get(dbg, &regs) && regs.pc == LOOP_TOP &&
                                                 (peek(dbg, VAR_X) & 0xFF) >= start + 2);
        check("hit once, though the loop passed it before", intvdebug_bp_list(dbg, list, 16) == 1 && list[0].hits == 1);
        check("reason is a breakpoint with its id", intvdebug_last_stop(dbg, &stop) &&
                                                       stop.reason == INTVDEBUG_STOP_BREAKPOINT && stop.bp_id == id);
        intvdebug_stop_text(dbg, text, sizeof(text), NULL);
        check("exec stop text", strcmp(text, "breakpoint: exec $50C8") == 0);
        intvdebug_bp_remove(dbg, id);
    }

    /* ---- run_to, and its one-shot not outliving the stop ---- */
    intvdebug_run_to(dbg, PROC_BEGIN);
    check("run_to stops", wait_paused(dbg, 15000));
    check("at the address", intvdebug_regs_get(dbg, &regs) && regs.pc == PROC_BEGIN);
    intvdebug_stop_text(dbg, text, sizeof(text), NULL);
    check("run_to stop text", strcmp(text, "stopped at $50D5") == 0);
    check("a one-shot is not a table entry", intvdebug_bp_list(dbg, list, 16) == 0);
    intvdebug_run_to(dbg, LOOP_TOP);
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_EXEC;
    spec.lo = spec.hi = PROC_BEGIN;   /* a real one comes first */
    intvdebug_resume(dbg);
    usleep(1000);
    intvdebug_pause(dbg);
    wait_paused(dbg, 2000);
    intvdebug_run_to(dbg, 0x50CB);
    check("run_to lands again", wait_paused(dbg, 15000) && intvdebug_regs_get(dbg, &regs) && regs.pc == 0x50CB);
    intvdebug_resume(dbg);
    usleep(300000);
    check("no stale one-shot stops the machine later", !intvdebug_is_paused(dbg));
    intvdebug_pause(dbg);
    check("pause", wait_paused(dbg, 2000));

    /* ---- history after a breakpoint stop, its clock, and the branches filter ---- */
    {
        static intvdebug_hist hist[4096];
        char line[256];
        int count, idx, saw_interrupt = 0, saw_return = 0, wrongly = 0;

        intvdebug_history_clear(dbg);
        intvdebug_history_enable(dbg, 1);
        memset(&spec, 0, sizeof(spec));
        spec.type = INTVDEBUG_BP_EXEC;
        spec.lo = spec.hi = LOOP_TOP;
        id = intvdebug_bp_add(dbg, &spec);
        usleep(1000);
        check("history stops at the breakpoint", run_stop(dbg, 15000));
        count = intvdebug_history_copy(dbg, hist, 8);
        check("the instruction the breakpoint is on has not run", count == 8 && hist[7].pc != LOOP_TOP);
        intvdebug_bp_remove(dbg, id);
        intvdebug_resume(dbg);
        usleep(150000);   /* a few frames */
        intvdebug_pause(dbg);
        check("paused again", wait_paused(dbg, 5000));
        count = intvdebug_history_copy(dbg, hist, 4096);
        check("a window of history", count == 4096);
        for (idx = 1; idx < count; idx++) {
            if (hist[idx].cycle <= hist[idx - 1].cycle)
                wrongly++;
        }
        check("the clock is the CPU's own, strictly increasing", wrongly == 0);
        check("entries carry the frame", hist[count - 1].frame >= hist[0].frame && hist[count - 1].frame > 0);
        wrongly = 0;
        for (idx = 0; idx < count - 1; idx++) {
            if (hist[idx].event == INTVDEBUG_EV_INTERRUPT) {
                saw_interrupt = 1;
                if (hist[idx + 1].pc != 0x1004 && hist[idx + 1].event == INTVDEBUG_EV_NONE)
                    wrongly++;
                if (hist[idx].pc == 0x1004)
                    wrongly++;   /* where it was taken, not the vector */
            }
        }
        check("an interrupt is recorded where it was taken", saw_interrupt && wrongly == 0);
        for (idx = 0; idx < count; idx++) {
            intvdebug_history_format(&hist[idx], line, sizeof(line));
            if (hist[idx].event == INTVDEBUG_EV_NONE && strstr(line, "PULR") && strstr(line, "R7")) {
                saw_return = 1;
                if (!intvdebug_history_match(&hist[idx], INTVDEBUG_HIST_BRANCHES, 0, 0))
                    wrongly++;
            }
            if (hist[idx].event == INTVDEBUG_EV_NONE && strstr(line, "MVO") &&
                intvdebug_history_match(&hist[idx], INTVDEBUG_HIST_BRANCHES, 0, 0))
                wrongly++;
        }
        check("a return (PULR R7) is a branch, a store is not", saw_return && wrongly == 0);
        check("trace count", intvdebug_trace_count(dbg) == INTVDEBUG_HISTORY_SIZE ||
                                 (uint64_t)intvdebug_trace_count(dbg) == intvdebug_history_count(dbg));
        {
            intvdebug_trace newest[3];
            check("trace_read is newest first", intvdebug_trace_read(dbg, newest, 3) == 3 &&
                                                    newest[0].cycle > newest[1].cycle && newest[1].cycle > newest[2].cycle);
            check("trace_read matches the history", newest[0].cycle == hist[count - 1].cycle);
        }
        intvdebug_history_enable(dbg, 0);
        intvdebug_history_clear(dbg);
    }

    /* ---- the memory map has no holes, and names things by function ---- */
    {
        intvdebug_region map[64];
        int count = intvdebug_memory_map(dbg, map, 64), idx, holes = 0, pads = 0, unmapped = 0;
        check("a map", count > 5);
        check("from $0000 to $FFFF", count > 0 && map[0].lo == 0x0000 && map[count - 1].hi == 0xFFFF);
        for (idx = 1; idx < count; idx++)
            if (map[idx].lo != (uint16_t)(map[idx - 1].hi + 1))
                holes++;
        check("gap-free", holes == 0);
        for (idx = 0; idx < count; idx++) {
            if (map[idx].lo <= 0x01F0 && map[idx].hi >= 0x01F0 && strstr(map[idx].name, "Pads") && strstr(map[idx].name, "PSG0"))
                pads = 1;
            if (!strcmp(map[idx].name, "unmapped") && !map[idx].readable && !map[idx].writable)
                unmapped = 1;
        }
        check("$01F0 is the PSG and the pads", pads);
        check("holes are named unmapped", unmapped);
    }

    /* ---- symbols ---- */
    {
        int picks[8], count;
        intvsymtab_add(intvsymtab_shared(), 0x5004, "Game_Loop");
        intvsymtab_add(intvsymtab_shared(), 0x5000, "game_start");
        count = intvdebug_symbols_filter(dbg, "GAME", picks, 8);
        check("filter, case-insensitive", count == 2);
        {
            uint16_t first = 0, second = 0;
            intvsymtab_enum(intvsymtab_shared(), picks[0], &first, NULL, 0, NULL, 0);
            intvsymtab_enum(intvsymtab_shared(), picks[1], &second, NULL, 0, NULL, 0);
            check("ordered by address", first == 0x5000 && second == 0x5004);
        }
        check("an empty filter matches all", intvdebug_symbols_filter(dbg, "", picks, 8) == 8);
    }

    /* ---- Cart & I/O and Sound ---- */
    {
        intvdebug_io ports;
        intvdebug_psg psg;
        intvdebug_cart cart;
        check("io", intvdebug_io_get(dbg, &ports) == 1 && ports.pad[0].present && !ports.pad[1].present);
        check("psg 0", intvdebug_sound_get(dbg, 0, &psg) == 1 && psg.present);
        check("psg 1 needs the ECS", intvdebug_sound_get(dbg, 1, &psg) == 0);
        intvdebug_cart_info(dbg, &cart);
        check("cart_info", strstr(cart.path, "main.bin") != NULL);
        intvdebug_cart_summary(dbg, text, sizeof(text));
        check("cart summary", strstr(text, "Cartridge:") != NULL);
    }

    /* ---- the prompt ---- */
    check("help", run_command(dbg, "help", text, sizeof(text)) && strstr(text, "runto") != NULL);
    run_command(dbg, "regs", text, sizeof(text));
    check("regs", strstr(text, "R7") && strstr(text, "cycles"));
    if (getenv("ENGINE_TEST_SHOW"))
        fprintf(stderr, "---- regs\n%s", text);
    run_command(dbg, "print 3 + 4 * 2", text, sizeof(text));
    check("print", strstr(text, "$000B") && strstr(text, "11"));
    run_command(dbg, "print nosuch", text, sizeof(text));
    check("print error", strstr(text, "error") != NULL);
    run_command(dbg, "set r2 $beef", text, sizeof(text));
    check("set", intvdebug_regs_get(dbg, &regs) && regs.r[2] == 0xBEEF);
    run_command(dbg, "print r2", text, sizeof(text));
    check("print a register", strstr(text, "$BEEF") != NULL);
    run_command(dbg, "poke $0200 $1234 $5678", text, sizeof(text));
    check("poke", peek(dbg, 0x0200) == 0x1234 && peek(dbg, 0x0201) == 0x5678);
    run_command(dbg, "mem $0200 2", text, sizeof(text));
    check("mem", strstr(text, "1234 5678") != NULL);
    run_command(dbg, "bpw $0200 value == 7", text, sizeof(text));
    check("bpw with a condition", strstr(text, "breakpoint #") != NULL);
    run_command(dbg, "bpw $0200 value ==", text, sizeof(text));
    check("a bad condition is refused", strstr(text, "does not parse") != NULL);
    run_command(dbg, "breaks", text, sizeof(text));
    check("breaks", strstr(text, "Write") && strstr(text, "if value == 7"));
    run_command(dbg, "bpa $0210", text, sizeof(text));
    check("bpa is read-or-write", intvdebug_bp_list(dbg, list, 16) == 2 && list[1].type == INTVDEBUG_BP_ACCESS);
    run_command(dbg, "clear", text, sizeof(text));
    check("clear", intvdebug_bp_list(dbg, list, 16) == 0);
    run_command(dbg, "disasm $50C8 3", text, sizeof(text));
    check("disasm", strstr(text, "50C8") && strstr(text, "MVI"));
    if (getenv("ENGINE_TEST_SHOW"))
        fprintf(stderr, "---- disasm\n%s", text);
    run_command(dbg, "find $1234 $5678", text, sizeof(text));
    check("find", strstr(text, "$0200") != NULL);
    run_command(dbg, "map", text, sizeof(text));
    check("map", strstr(text, "$0000-") && strstr(text, "Scratch"));
    if (getenv("ENGINE_TEST_SHOW"))   /* look at what the prompt says */
        fprintf(stderr, "---- map\n%s", text);
    run_command(dbg, "io", text, sizeof(text));
    check("io", strstr(text, "pads") != NULL);
    if (getenv("ENGINE_TEST_SHOW"))
        fprintf(stderr, "---- io\n%s", text);
    run_command(dbg, "sound", text, sizeof(text));
    check("sound", strstr(text, "Base PSG") != NULL);
    if (getenv("ENGINE_TEST_SHOW"))
        fprintf(stderr, "---- sound\n%s", text);
    run_command(dbg, "cart", text, sizeof(text));
    check("cart", strstr(text, "Cartridge") != NULL);
    run_command(dbg, "symbols game", text, sizeof(text));
    check("symbols", strstr(text, "game_start") && strstr(text, "Game_Loop"));
    intvsymtab_add(intvsymtab_shared(), VAR_X, "var_X");
    run_command(dbg, "locals", text, sizeof(text));
    check("locals", strstr(text, "x ") != NULL);
    if (getenv("ENGINE_TEST_SHOW"))
        fprintf(stderr, "---- locals\n%s", text);
    run_command(dbg, "watch x", text, sizeof(text));
    check("watch", strstr(text, "$010C") != NULL);
    run_command(dbg, "unwatch all", text, sizeof(text));
    check("unwatch", intvwatch_count() == 0);
    run_command(dbg, "trace on", text, sizeof(text));
    check("trace on", intvdebug_history_enabled(dbg));
    run_command(dbg, "trace off", text, sizeof(text));
    check("trace off", !intvdebug_history_enabled(dbg));
    run_command(dbg, "frobnicate", text, sizeof(text));
    check("unknown command", strstr(text, "unknown command") != NULL);
    run_command(dbg, "save regs /nonexistent-dir/x", text, sizeof(text));
    check("save fails politely", strstr(text, "cannot write") != NULL);
    check("completions: commands", intvdebug_completions(dbg, "br", text, sizeof(text)) == 2 &&
                                       strstr(text, "break\n") && strstr(text, "breaks\n"));
    check("completions: registers", intvdebug_completions(dbg, "print r", text, sizeof(text)) >= 8);
    check("completions: labels", intvdebug_completions(dbg, "runto game_s", text, sizeof(text)) == 1 &&
                                     strstr(text, "game_start") != NULL);
    check("completions: save kinds", intvdebug_completions(dbg, "save r", text, sizeof(text)) >= 1 &&
                                         strstr(text, "ram") && strstr(text, "regs"));

    /* ---- the decoder's fetches are not reads; the CPU hijacked with words
     * poked into system RAM (a poke invalidates the decode cache, so the
     * first pass over them decodes, and decoding reads the words) ---- */
    {
        /* MVII #$1234,R0 / MVO R0,$0310 / MVI $0310,R1 / B $ */
        static const uint16_t program[] = { 0x02B8, 0x1234, 0x0240, 0x0310, 0x0280, 0x0310, 0x0220, 0x0001 };
        char want[96];
        int address = 0;

        poke(dbg, 0x0310, 0x1233);
        {
            int idx;
            for (idx = 0; idx < 8; idx++)
                poke(dbg, (uint16_t)(0x0300 + idx), program[idx]);
        }
        check("poked words read back", peek(dbg, 0x0300) == 0x02B8 && peek(dbg, 0x0307) == 0x0001 &&
                                           peek(dbg, 0x0310) == 0x1233);
        run_command(dbg, "disasm $0300 4", text, sizeof(text));
        check("the program is what was meant", strstr(text, "MVII") && strstr(text, "MVO") && strstr(text, "MVI") &&
                                                   strstr(text, "$0310"));

        memset(&spec, 0, sizeof(spec));
        spec.type = INTVDEBUG_BP_WRITE;
        spec.lo = spec.hi = 0x0310;
        id = intvdebug_bp_add(dbg, &spec);
        intvdebug_regs_set(dbg, INTVDEBUG_REG_PC, 0x0300);
        check("the word stop", run_stop(dbg, 5000));
        check("last_stop is a write of a 16-bit word", intvdebug_last_stop(dbg, &stop) &&
                                                          stop.reason == INTVDEBUG_STOP_WATCH_WRITE && stop.addr == 0x0310 &&
                                                          stop.value == 0x1234 && stop.old_value == 0x1233);
        snprintf(want, sizeof(want), "wrote $0310 = $1234 (was $1233) at $0302");
        intvdebug_stop_text(dbg, text, sizeof(text), &address);
        check("the 16-bit stop text", strcmp(text, want) == 0 && address == 0x0304);
        check("the stop snapshot says the same", intvdebug_stop_snapshot(dbg, &snap) && strcmp(snap.text, want) == 0);
        intvdebug_bp_remove(dbg, id);

        /* a read watch over the code itself must see nothing: those are fetches */
        memset(&spec, 0, sizeof(spec));
        spec.type = INTVDEBUG_BP_READ;
        spec.lo = 0x0300;
        spec.hi = 0x0307;
        id = intvdebug_bp_add(dbg, &spec);
        intvdebug_regs_set(dbg, INTVDEBUG_REG_PC, 0x0300);
        intvdebug_resume(dbg);
        usleep(300000);
        check("instruction fetches are not reads", !intvdebug_is_paused(dbg));
        intvdebug_pause(dbg);
        check("paused", wait_paused(dbg, 2000));
        intvdebug_bp_remove(dbg, id);

        /* ...but the data read of the word it stored is */
        memset(&spec, 0, sizeof(spec));
        spec.type = INTVDEBUG_BP_READ;
        spec.lo = spec.hi = 0x0310;
        id = intvdebug_bp_add(dbg, &spec);
        intvdebug_regs_set(dbg, INTVDEBUG_REG_PC, 0x0300);
        check("the read stop", run_stop(dbg, 5000));
        intvdebug_stop_text(dbg, text, sizeof(text), NULL);
        check("a data read is", intvdebug_last_stop(dbg, &stop) && stop.reason == INTVDEBUG_STOP_WATCH_READ &&
                                    stop.addr == 0x0310 && stop.value == 0x1234 &&
                                    strcmp(text, "read $0310 = $1234 at $0304") == 0);
        intvdebug_bp_remove(dbg, id);
    }

    intvdebug_breakpoint_clear_all(dbg);
    intvdebug_resume(dbg);
    usleep(20000);
    intvdebug_set_engaged(dbg, 0);
    intv_host_stop();

    if (failed) {
        fprintf(stderr, "engine_test: FAILED\n");
        return 1;
    }
    printf("engine_test: OK\n");
    return 0;
}
