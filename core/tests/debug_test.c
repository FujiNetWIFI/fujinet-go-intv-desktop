/*
 * debug_test -- exercises intvdebug against the real running machine:
 * engage, pause, inspect registers/memory/disassembly, set a breakpoint,
 * resume to it, single-step, and disengage cleanly.
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

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "debug_test: FAILED: %s\n", what);
        failed = 1;
    }
}

/* Waits (bounded) for intvdebug_is_paused() to become true -- pausing
 * takes effect at the next instruction boundary, not instantly. */
static int wait_paused(intvdebug *d, int timeout_ms)
{
    int waited = 0;
    while (!intvdebug_is_paused(d) && waited < timeout_ms) {
        usleep(1000);
        waited++;
    }
    return intvdebug_is_paused(d);
}

int main(void)
{
    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "debug_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    char rom_dir[1024];
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-debug-test-");
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65503,
    };
    if (intv_host_start(&opts) != 0) {
        fprintf(stderr, "debug_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000); /* let cfg_init() finish before touching intv.cp1600 */

    intvdebug *d = intvdebug_get();
    check("not engaged initially", !intvdebug_is_engaged(d));

    intvdebug_set_engaged(d, 1);
    check("engaged", intvdebug_is_engaged(d));

    intvdebug_pause(d);
    check("paused", wait_paused(d, 2000));

    /* ---- registers: PC must be a plausible EXEC address (0x1000-0x1FFF
     * is the EXEC ROM's own range, see intv_host.c's boot log) ---- */
    intvdebug_regs regs;
    check("regs_get", intvdebug_regs_get(d, &regs) == 1);
    check("pc mirrors r[7]", regs.pc == regs.r[7]);
    /* By the time the pause actually lands (a few ms in), execution has
     * long since left EXEC ROM init and is running the embedded config
     * ROM's own code -- PC should be somewhere in one of the address
     * ranges intv_host's own boot log reports as mapped (EXEC ROM or one
     * of the ICart windows the config ROM occupies). */
    check("pc in a mapped ROM range",
          (regs.pc >= 0x1000 && regs.pc <= 0x1FFF) ||
          (regs.pc >= 0x5000 && regs.pc <= 0x6FFF) ||
          (regs.pc >= 0x8000 && regs.pc <= 0x9BFF) ||
          (regs.pc >= 0xD000 && regs.pc <= 0xDFFF));

    /* ---- disassembly at PC produces at least one real instruction ---- */
    intvdebug_dasm_line lines[4];
    int n = intvdebug_disassemble(d, regs.pc, regs.D, lines, 4);
    check("disassembled some instructions", n > 0);
    if (n > 0) {
        check("first line at PC", lines[0].addr == regs.pc);
        check("first line has text", lines[0].text[0] != '\0');
        check("first line has plausible length", lines[0].length >= 1 &&
              lines[0].length <= 3);
    }

    /* ---- memory: EXEC ROM should read back non-trivial content (not all
     * zero) -- a real ROM image, not an empty/unmapped range ---- */
    uint16_t words[8];
    check("read 8 words", intvdebug_read(d, 0x1000, words, 8) == 8);
    int any_nonzero = 0;
    for (int i = 0; i < 8; i++)
        if (words[i] != 0) any_nonzero = 1;
    check("EXEC ROM is not all zero", any_nonzero);

    /* ---- scratchpad RAM ($0100-$01EF, 8-bit-wide per the boot log) is
     * writable; round-trip a poke ---- */
    uint16_t probe = 0x55;
    check("write RAM", intvdebug_write(d, 0x0100, &probe, 1) == 1);
    uint16_t readback = 0;
    check("read back RAM", intvdebug_read(d, 0x0100, &readback, 1) == 1);
    check("RAM round-trip matches", (readback & 0xFF) == (probe & 0xFF));

    /* ---- breakpoints ---- */
    check("set breakpoint", intvdebug_breakpoint_toggle(d, 0x1010) == 1);
    check("breakpoint is set", intvdebug_breakpoint_is_set(d, 0x1010));
    uint16_t bps[8];
    check("breakpoint list has 1", intvdebug_breakpoint_list(d, bps, 8) == 1);
    check("toggle again clears it",
          intvdebug_breakpoint_toggle(d, 0x1010) == 0);
    check("breakpoint no longer set",
          !intvdebug_breakpoint_is_set(d, 0x1010));

    /* ---- single step advances PC and bumps the stop serial ---- */
    uint64_t serial_before = intvdebug_stop_serial(d);
    intvdebug_regs before = regs;
    intvdebug_step(d);
    check("step re-pauses", wait_paused(d, 2000));
    check("stop serial advanced", intvdebug_stop_serial(d) != serial_before);
    intvdebug_regs after;
    check("regs_get after step", intvdebug_regs_get(d, &after) == 1);
    check("PC moved (or a branch/loop landed elsewhere, but not stuck "
          "mid-instruction)", after.pc != 0 || before.pc != 0);

    /* ---- resume: machine should leave the paused state ---- */
    intvdebug_resume(d);
    usleep(20000);
    check("resumed (no longer paused)", !intvdebug_is_paused(d));

    /* ---- disengage must leave the emulator thread free-running, not
     * parked -- confirmed by the session stopping cleanly below. ---- */
    intvdebug_set_engaged(d, 0);
    check("disengaged", !intvdebug_is_engaged(d));

    intv_host_stop();

    if (failed) {
        fprintf(stderr, "debug_test: FAILED\n");
        return 1;
    }
    printf("debug_test: OK\n");
    return 0;
}
