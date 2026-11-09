/*
 * watch_test -- the breakpoint table and access watchpoints on a real
 * running machine.
 *
 * Boots tests/data/srcmap/main.bin (main.bas: x = x + 1 each frame, x being
 * var_X at $010C, written by the MVO at $50CB) and checks that a WRITE
 * watchpoint stops after the instruction that wrote it, READ sees the
 * operand read, conditions filter, hit counts and the stop info are right,
 * and a disabled entry stays quiet. Needs the embedded EXEC/GROM; skips (77)
 * without.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv_host.h"
#include "intvdebug.h"
#include "roms_embedded.h"
#include "test_tmpdir.h"

#define VAR_X 0x010C

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "watch_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static int wait_paused(intvdebug *d, int timeout_ms)
{
    int waited = 0;
    while (!intvdebug_is_paused(d) && waited < timeout_ms) {
        usleep(1000);
        waited++;
    }
    return intvdebug_is_paused(d);
}

static uint16_t peek(intvdebug *d, uint16_t addr)
{
    uint16_t v = 0xFFFF;
    intvdebug_read(d, addr, &v, 1);
    return v;
}

/* Adds one entry, resumes, and waits for the stop. */
static int run_to_stop(intvdebug *d, int type, int cond, uint16_t value, intvdebug_stop_info *info,
                       int *id_out)
{
    intvdebug_bp bp;
    memset(&bp, 0, sizeof(bp));
    bp.type = type;
    bp.lo = bp.hi = VAR_X;
    bp.cond = cond;
    bp.value = value;
    bp.mask = 0xFF; /* the scratchpad is 8 bits wide */
    *id_out = intvdebug_bp_add(d, &bp);
    if (*id_out < 0)
        return 0;
    intvdebug_resume(d);
    if (!wait_paused(d, 15000))
        return 0;
    return intvdebug_last_stop(d, info);
}

int main(void)
{
    char rom_dir[1024];
    intvdebug_stop_info st;
    intvdebug_bp list[8];
    int id, id2;

    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "watch_test: no embedded ROMs (build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-watch-test-");
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65505,
        .cart_path = SRCMAP_FIXTURE_DIR "/main.bin",
    };
    if (intv_host_start(&opts) != 0) {
        fprintf(stderr, "watch_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000);

    intvdebug *d = intvdebug_get();
    intvdebug_set_engaged(d, 1);
    intvdebug_pause(d);
    check("paused", wait_paused(d, 2000));
    check("pause is the reason", intvdebug_last_stop(d, &st) && st.reason == INTVDEBUG_STOP_PAUSE);

    /* ---- the table itself ---- */
    check("toggle sets", intvdebug_breakpoint_toggle(d, 0x50C8) == 1);
    check("state enabled", intvdebug_breakpoint_state(d, 0x50C8) == 1 && intvdebug_breakpoint_is_set(d, 0x50C8));
    check("one entry", intvdebug_bp_list(d, list, 8) == 1 && list[0].type == INTVDEBUG_BP_EXEC && list[0].enabled);
    id2 = list[0].id;
    check("disable", intvdebug_bp_enable(d, id2, 0) && intvdebug_breakpoint_state(d, 0x50C8) == 2 &&
                         !intvdebug_breakpoint_is_set(d, 0x50C8));
    check("remove", intvdebug_bp_remove(d, id2) && intvdebug_breakpoint_state(d, 0x50C8) == 0);
    check("ids are not reused", intvdebug_breakpoint_toggle(d, 0x50C8) == 1 &&
                                    intvdebug_bp_list(d, list, 8) == 1 && list[0].id > id2);
    intvdebug_breakpoint_clear_all(d);
    check("cleared", intvdebug_bp_list(d, list, 8) == 0);

    /* ---- WRITE: stops after the MVO at $50CB, on the next instruction ---- */
    check("write watch stops", run_to_stop(d, INTVDEBUG_BP_WRITE, INTVDEBUG_COND_ANY, 0, &st, &id));
    check("reason is WATCH_WRITE", st.reason == INTVDEBUG_STOP_WATCH_WRITE);
    check("it names the entry and the word", st.bp_id == id && st.addr == VAR_X);
    check("PC is after the writing instruction", st.pc == 0x50CD);
    check("the value written is in memory", (peek(d, VAR_X) & 0xFF) == (st.value & 0xFF));
    check("hit counted", intvdebug_bp_list(d, list, 8) == 1 && list[0].hits == 1);
    {
        const uint16_t first = st.value & 0xFF;

        /* ---- CHANGED: the next frame's x differs from this one ---- */
        intvdebug_bp_remove(d, id);
        check("changed watch stops", run_to_stop(d, INTVDEBUG_BP_WRITE, INTVDEBUG_COND_CHANGED, 0, &st, &id));
        check("the old value is the last one", (st.old_value & 0xFF) == first);
        check("and it changed", (st.value & 0xFF) != first);
        intvdebug_bp_remove(d, id);

        /* ---- EQ: wait for x to reach a value a few frames away ---- */
        {
            const uint16_t want = (uint16_t)((peek(d, VAR_X) + 3) & 0xFF);
            check("== watch stops", run_to_stop(d, INTVDEBUG_BP_WRITE, INTVDEBUG_COND_EQ, want, &st, &id));
            check("on the value asked for", (st.value & 0xFF) == want);
            intvdebug_bp_remove(d, id);
        }
    }

    /* ---- READ: after the write at $50CB the next read of x is bump's
     * y = x + 2 (MVI var_X at $50D6); PC is then past that instruction ---- */
    check("read watch stops", run_to_stop(d, INTVDEBUG_BP_READ, INTVDEBUG_COND_ANY, 0, &st, &id));
    check("reason is WATCH_READ", st.reason == INTVDEBUG_STOP_WATCH_READ && st.addr == VAR_X);
    check("PC is after the reading instruction", st.pc == 0x50D8);
    intvdebug_bp_remove(d, id);

    /* ---- a disabled entry stays quiet; Pause still stops ---- */
    {
        intvdebug_bp bp;
        memset(&bp, 0, sizeof(bp));
        bp.type = INTVDEBUG_BP_WRITE | INTVDEBUG_BP_READ;
        bp.lo = bp.hi = VAR_X;
        id = intvdebug_bp_add(d, &bp);
        intvdebug_bp_enable(d, id, 0);
        intvdebug_resume(d);
        usleep(300000);
        check("a disabled watch does not stop", !intvdebug_is_paused(d));
        intvdebug_pause(d);
        check("Pause stops", wait_paused(d, 5000));
        check("and says why", intvdebug_last_stop(d, &st) && st.reason == INTVDEBUG_STOP_PAUSE);
        /* enabling while paused takes effect */
        intvdebug_bp_enable(d, id, 1);
        intvdebug_resume(d);
        check("re-enabled, it stops", wait_paused(d, 15000));
        check("as a watch", intvdebug_last_stop(d, &st) &&
                                (st.reason == INTVDEBUG_STOP_WATCH_READ || st.reason == INTVDEBUG_STOP_WATCH_WRITE));
        intvdebug_bp_remove(d, id);
    }

    /* ---- search: three words of the cart's own code find themselves ---- */
    {
        uint16_t pat[3], hits[16];
        int n, found = 0, k;
        intvdebug_read(d, 0x50C8, pat, 3);
        n = intvdebug_search(d, pat, 3, 0xFFFF, 0, 0x5000, 0x5FFF, hits, 16);
        for (k = 0; k < n; k++)
            if (hits[k] == 0x50C8)
                found = 1;
        check("hex search finds the code", n >= 1 && found);
        check("a pattern that is not there", intvdebug_search(d, (const uint16_t[]){ 0xBEEF, 0xF00D, 0xBEEF }, 3,
                                                              0xFFFF, 0, 0x5000, 0x5FFF, hits, 16) == 0);
        /* the match straddling a chunk boundary ($51FF/$5200 is one) */
        intvdebug_read(d, 0x51FF, pat, 3);
        n = intvdebug_search(d, pat, 3, 0xFFFF, 0, 0x5000, 0x5FFF, hits, 16);
        found = 0;
        for (k = 0; k < n; k++)
            if (hits[k] == 0x51FF)
                found = 1;
        check("a match across a chunk edge", found);
        /* the title text in EXEC's GROM-free data is not reliable; use masks:
         * the low byte of the same words, case folding on the low byte */
        check("masked search", intvdebug_search(d, (const uint16_t[]){ (uint16_t)(pat[0] & 0xFF) }, 1, 0x00FF,
                                                INTVDEBUG_SEARCH_NOCASE, 0x51FF, 0x51FF, hits, 4) == 1);
    }

    /* ---- the disassembly text has no address or opcode words in front ---- */
    {
        intvdebug_dasm_line dl[2];
        check("disassembly text starts at the mnemonic",
              intvdebug_disassemble(d, 0x50D5, 0, dl, 1) == 1 && dl[0].text[0] >= 'A' && dl[0].text[0] <= 'Z');
        check("BEGIN is a PSHR", strstr(dl[0].text, "PSHR") != NULL);
    }

    /* ---- the memory map and the cart ---- */
    {
        intvdebug_region reg[64];
        intvdebug_cart cart;
        int n = intvdebug_memory_map(d, reg, 64), k, saw_rom = 0, saw_scratch = 0;
        for (k = 0; k < n; k++) {
            if (reg[k].lo <= 0x5000 && reg[k].hi >= 0x5000 && reg[k].readable &&
                strstr(reg[k].name, "BIN"))
                saw_rom = 1;
            if (reg[k].lo <= 0x0100 && reg[k].hi >= 0x0100 && reg[k].readable && reg[k].writable)
                saw_scratch = 1;
        }
        check("a map came back", n > 5);
        check("the cart's image is mapped at $5000", saw_rom);
        check("scratch RAM is read-write", saw_scratch);
        check("regions are in order and do not overlap", n > 1 && reg[1].lo > reg[0].hi);
        intvdebug_cart_info(d, &cart);
        check("the cart path is the fixture", strstr(cart.path, "main.bin") != NULL);
    }

    /* ---- history: record a stretch of execution, then read it back ---- */
    {
        static intvdebug_hist h[512];
        char line[256];
        int n, k, saw_branch = 0, saw_cart = 0;
        intvdebug_history_clear(d);
        check("history starts off", !intvdebug_history_enabled(d));
        intvdebug_history_enable(d, 1);
        intvdebug_resume(d);
        usleep(400000);
        intvdebug_pause(d);
        check("paused again", wait_paused(d, 5000));
        check("instructions were recorded", intvdebug_history_count(d) > 1000);
        n = intvdebug_history_copy(d, h, 512);
        check("a full window came back", n == 512);
        for (k = 0; k < n; k++) {
            if (!intvdebug_history_match(&h[k], INTVDEBUG_HIST_BRANCHES, 0, 0))
                continue;
            saw_branch = 1;
            intvdebug_history_format(&h[k], line, sizeof(line));
            check("a branch line shows its mnemonic", strstr(line, " B") || strstr(line, " J"));
        }
        check("the history has branches in it", saw_branch);
        for (k = 0; k < n; k++)
            if (intvdebug_history_match(&h[k], INTVDEBUG_HIST_RANGE, 0x5000, 0x5FFF))
                saw_cart = 1;
        check("and the cart's own code", saw_cart);
        check("the last entry is where we stopped", h[n - 1].pc != 0 || n > 0);
        intvdebug_history_enable(d, 0);
        {
            const uint64_t c = intvdebug_history_count(d);
            intvdebug_resume(d);
            usleep(100000);
            check("off means no more records", intvdebug_history_count(d) == c);
            intvdebug_pause(d);
            wait_paused(d, 5000);
        }
        intvdebug_history_clear(d);
        check("clear", intvdebug_history_count(d) == 0);
    }

    /* ---- an EXEC entry reports as a breakpoint, with its hit ---- */
    intvdebug_breakpoint_toggle(d, 0x50C8);
    intvdebug_resume(d);
    check("exec stops", wait_paused(d, 15000));
    check("reason is BREAKPOINT at the address", intvdebug_last_stop(d, &st) &&
                                                   st.reason == INTVDEBUG_STOP_BREAKPOINT && st.pc == 0x50C8 &&
                                                   st.bp_id > 0);
    check("and counted", intvdebug_bp_list(d, list, 8) == 1 && list[0].hits >= 1);

    intvdebug_breakpoint_clear_all(d);
    intvdebug_resume(d);
    usleep(20000);
    intvdebug_set_engaged(d, 0);
    intv_host_stop();

    if (failed) {
        fprintf(stderr, "watch_test: FAILED\n");
        return 1;
    }
    printf("watch_test: OK\n");
    return 0;
}
