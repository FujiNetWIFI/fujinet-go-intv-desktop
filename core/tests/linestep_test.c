/*
 * linestep_test -- stepping by IntyBASIC line on a real running machine.
 *
 * Boots tests/data/srcmap/main.bin (real intybasic + as1600 output; main.bas
 * INCLUDEs inc.bas), imports its source map, then:
 *   break on main.bas:4  ->  Step Line x2 lands on the GOSUB's callee
 *   Step Over skips the GOSUB  ->  Step Out leaves the procedure.
 * Needs the embedded EXEC/GROM (WITH_INTV_ROMS=ON); skips (77) without.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv_host.h"
#include "intvdebug.h"
#include "roms_embedded.h"
#include "srcmap.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "linestep_test: FAILED: %s\n", what);
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

/* The BASIC line the machine is stopped on, as "name:line" (or "?"). */
static const char *where(intvdebug *d, char *buf, size_t n)
{
    intvdebug_regs regs;
    int f, l;
    snprintf(buf, n, "?");
    if (intvdebug_regs_get(d, &regs) &&
        intvsrcmap_lookup(intvsrcmap_shared(), regs.pc, &f, &l))
        snprintf(buf, n, "%s:%d", intvsrcmap_file_name(intvsrcmap_shared(), f), l);
    return buf;
}

/* Runs one stepper and expects to stop on `want` ("main.bas:5"), at that
 * line's first word. */
static void expect(intvdebug *d, const char *what, void (*step)(intvdebug *),
                   const char *want)
{
    char got[64], msg[128];
    intvdebug_regs regs;
    uint64_t serial = intvdebug_stop_serial(d);

    step(d);
    snprintf(msg, sizeof(msg), "%s: pauses", what);
    check(msg, wait_paused(d, 10000));
    snprintf(msg, sizeof(msg), "%s: stop serial moves", what);
    check(msg, intvdebug_stop_serial(d) != serial);
    where(d, got, sizeof(got));
    snprintf(msg, sizeof(msg), "%s: stops on %s (got %s)", what, want, got);
    check(msg, strcmp(got, want) == 0);
    snprintf(msg, sizeof(msg), "%s: at the line's first word", what);
    check(msg, intvdebug_regs_get(d, &regs) &&
               intvsrcmap_is_line_start(intvsrcmap_shared(), regs.pc));
}

int main(void)
{
    char rom_dir[1024];
    intvsrcmap *map = intvsrcmap_shared();
    intvsrcmap_import_result r;
    int mainf = -1, i;

    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "linestep_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    check("import", intvsrcmap_import(map, SRCMAP_FIXTURE_DIR, NULL, &r) == 2 &&
                    r.mapped > 0);
    for (i = 0; i < intvsrcmap_file_count(map); i++)
        if (strcmp(intvsrcmap_file_name(map, i), "main.bas") == 0)
            mainf = i;
    check("main.bas loaded", mainf >= 0);
    if (mainf < 0)
        return 1;

    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-linestep-test-");
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65504,
        .cart_path = SRCMAP_FIXTURE_DIR "/main.bin",
    };
    if (intv_host_start(&opts) != 0) {
        fprintf(stderr, "linestep_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000);

    intvdebug *d = intvdebug_get();
    intvdebug_set_engaged(d, 1);

    /* Run to main.bas:4 (x = x + 1) with an ordinary breakpoint made from
     * the BASIC line. The EXEC's title screen runs first, hence the wait. */
    int bp = intvsrcmap_first_addr(map, mainf, 4);
    check("line 4 has an address", bp >= 0);
    check("set breakpoint", intvdebug_breakpoint_toggle(d, (uint16_t)bp) == 1);
    intvdebug_pause(d);
    check("paused", wait_paused(d, 2000));
    intvdebug_resume(d);
    check("hits the breakpoint on main.bas:4", wait_paused(d, 20000));
    {
        char got[64];
        check("stopped on main.bas:4", strcmp(where(d, got, sizeof(got)), "main.bas:4") == 0);
    }

    /* Into: 4 -> 5 (GOSUB) -> inc.bas:2 (the procedure) */
    expect(d, "step line", intvdebug_step_line, "main.bas:5");
    expect(d, "step line into the call", intvdebug_step_line, "inc.bas:2");
    /* Over at the procedure's first word stays inside it (its PSHR R5 has
     * not run yet, so the stack does not look deeper) */
    expect(d, "step over inside the procedure", intvdebug_step_line_over, "inc.bas:3");
    /* Out: back in main.bas, on the line after the GOSUB */
    expect(d, "step out", intvdebug_step_line_out, "main.bas:6");

    /* Round the loop to line 4 again, then Over the GOSUB on line 5 */
    expect(d, "step line (WAIT)", intvdebug_step_line, "main.bas:7");
    expect(d, "step line (GOTO)", intvdebug_step_line, "main.bas:4");
    expect(d, "step line to the GOSUB", intvdebug_step_line, "main.bas:5");
    expect(d, "step over the call", intvdebug_step_line_over, "main.bas:6");

    /* ... and once more, leaving by Out straight from the first word */
    expect(d, "step line (WAIT) again", intvdebug_step_line, "main.bas:7");
    expect(d, "step line (GOTO) again", intvdebug_step_line, "main.bas:4");
    expect(d, "step line to the GOSUB again", intvdebug_step_line, "main.bas:5");
    expect(d, "step line into the call again", intvdebug_step_line, "inc.bas:2");
    expect(d, "step out from the first word", intvdebug_step_line_out, "main.bas:6");

    /* A by-line step can be cancelled with Pause: with no map nothing is a
     * line, so this would run forever -- Pause must still stop it. */
    intvdebug_breakpoint_clear_all(d);
    intvsrcmap_clear(map);
    intvdebug_step_line(d);
    usleep(20000);
    check("running while nothing is a line", !intvdebug_is_paused(d));
    intvdebug_pause(d);
    check("Pause stops a by-line step", wait_paused(d, 5000));

    intvdebug_resume(d);
    usleep(20000);
    intvdebug_set_engaged(d, 0);
    intv_host_stop();

    if (failed) {
        fprintf(stderr, "linestep_test: FAILED\n");
        return 1;
    }
    printf("linestep_test: OK\n");
    return 0;
}
