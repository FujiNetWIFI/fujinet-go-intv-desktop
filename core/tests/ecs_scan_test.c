/*
 * ecs_scan_test -- the shifted ECS keys as the MACHINE sees them, read back
 * over the emulated peripheral bus rather than out of intv.pad1.k[].
 *
 * ecs_key_test already pins the bits each key writes. This answers the
 * question those assertions cannot: does jzIntv's own scan actually turn a
 * high-byte "fake shift" bit into SHIFT-plus-the-key on the bus? The whole
 * natural-typing design rests on it -- '/' is not a key on an ECS, it is
 * SHIFT+7, and if the synthesised SHIFT never reached the scan then every
 * one of those characters would silently type its unshifted twin ('7'),
 * which no bit-level test would catch because the bits would be right.
 *
 * The two-pass sweep is not padding. pads.c's fake shift is a state
 * machine: a pending shift only ENGAGES on the pass that selects row 6 (the
 * real SHIFT key's row), and only once engaged do the fake-shift bits fold
 * down into the key rows. A single targeted read would miss it -- and so
 * would real hardware, which is why ECS BASIC sweeps.
 *
 * Requires ECS: pad1 is only registered on the bus when it is enabled
 * (cfg.c's pad_init), so unlike ecs_key_test this needs a real ecs.bin, not
 * just the EXEC/GROM pair.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "lzoe/lzoe.h"
#include "file/file.h"
#include "periph/periph.h"
#include "cp1600/cp1600.h"
#include "mem/mem.h"
#include "ecs/ecs.h"
#include "icart/icart.h"
#include "bincfg/bincfg.h"
#include "bincfg/legacy.h"
#include "pads/pads.h"
#include "pads/pads_cgc.h"
#include "pads/pads_intv2pc.h"
#include "avi/avi.h"
#include "gfx/gfx.h"
#include "gfx/palette.h"
#include "snd/snd.h"
#include "ay8910/ay8910.h"
#include "demo/demo.h"
#include "stic/stic.h"
#include "speed/speed.h"
#include "debug/debug_.h"
#include "debug/debug_if.h"
#include "event/event.h"
#include "ivoice/ivoice.h"
#include "jlp/jlp.h"
#include "fujinet/fujinet.h"
#include "locutus/locutus_adapt.h"
#include "cheat/cheat.h"
#include "cfg/mapping.h"
#include "cfg/cfg.h"

#include "intv_host.h"
#include "roms_embedded.h"
#include "test_tmpdir.h"


static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok)
    {
        fprintf(stderr, "ecs_scan_test: FAILED: %s\n", what);
        failed = 1;
    }
}

/* One read of one row, driving intv.pad1 exactly as the ECS's keyboard scan
 * does: side 0 to OUTPUT and side 1 to INPUT (reg 8 bit 6 -- pads.c calls
 * this "normal scanning mode, 0 drives, 1 reads"), the active-LOW row select
 * to reg 14, then reg 15 for the columns, which come back active-low too. */
static uint32_t scan_row(int row)
{
    periph_t *const p = AS_PERIPH(&intv.pad1);
    p->write(p, NULL, 8, 1u << 6);
    p->write(p, NULL, 14, 0xFFu & ~(1u << row));
    return 0xFFu & ~p->read(p, NULL, 15, 0);
}

/* A full rows-0..7 sweep, twice, returning what `row` read on the second
 * pass -- see the file header on why one pass is not enough. */
static uint32_t sweep_for(int row)
{
    uint32_t got = 0;
    int pass, r;

    for (pass = 0; pass < 2; pass++)
        for (r = 0; r < 8; r++)
        {
            const uint32_t cols = scan_row(r);
            if (r == row && pass == 1)
                got = cols;
        }
    return got;
}

#define ROW_SHIFT 6
#define COL_SHIFT 128

int main(void)
{
    char rom_dir[1024];
    intv_host_opts opts;

    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "ecs_scan_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-ecs-scan-test-");
    if (!mkdtemp(rom_dir))
    {
        perror("mkdtemp");
        return 1;
    }

    memset(&opts, 0, sizeof(opts));
    opts.rom_dir = rom_dir;
    opts.fujinet_host = "127.0.0.1";
    opts.fujinet_port = 65503;
    opts.ecs = INTV_HW_ON;   /* pad1 reaches the bus only when ECS is on */

    if (intv_host_start(&opts) != 0)
    {
        fprintf(stderr, "ecs_scan_test: intv_host_start failed (no ecs.bin?) "
                        "-- SKIP\n");
        return 77;
    }
    usleep(200000);

    /* A plain key asserts its own bit and nothing else. This is the control:
     * without it, a test that only ever saw SHIFT asserted could not tell a
     * working fake shift from a shift that is stuck on. */
    intv_host_ecs_key(INTV_ECS_KEY_7, 1);
    check("'7' reads on row 2 col 4", (sweep_for(2) & 16) != 0);
    check("'7' asserts no shift", (sweep_for(ROW_SHIFT) & COL_SHIFT) == 0);
    intv_host_ecs_key(INTV_ECS_KEY_7, 0);

    /* '/' is the SAME row 2 col 4 bit -- on an ECS it is SHIFT+7, not a key
     * of its own -- so the ONLY thing distinguishing it from '7' above is
     * the synthesised shift. Both halves have to be true at once. */
    intv_host_ecs_key(INTV_ECS_KEY_SLASH, 1);
    check("'/' reads as the '7' key...", (sweep_for(2) & 16) != 0);
    check("...with SHIFT synthesised on row 6 col 7",
          (sweep_for(ROW_SHIFT) & COL_SHIFT) != 0);
    intv_host_ecs_key(INTV_ECS_KEY_SLASH, 0);

    /* '%' rides the LEFT-ARROW key (row 0 col 0) -- the pairing a PC layout
     * would never suggest, and the one most likely to be "corrected" into
     * something wrong later. */
    intv_host_ecs_key(INTV_ECS_KEY_PCT, 1);
    check("'%' reads as the LEFT-ARROW key...", (sweep_for(0) & 1) != 0);
    check("...with SHIFT synthesised",
          (sweep_for(ROW_SHIFT) & COL_SHIFT) != 0);
    intv_host_ecs_key(INTV_ECS_KEY_PCT, 0);

    /* Releasing has to retract the synthesised shift too. A fake-shift bit
     * left behind would not merely stick one key down -- pads.c re-derives
     * the pending shift from every row on every scan, so the machine would
     * read SHIFT on every later keystroke. */
    check("release clears the key", sweep_for(0) == 0 && sweep_for(2) == 0);
    check("release clears the synthesised shift", sweep_for(ROW_SHIFT) == 0);

    /* And the real SHIFT key still works on its own, unchanged. */
    intv_host_ecs_key(INTV_ECS_KEY_SHIFT, 1);
    check("the real SHIFT key still reads on row 6 col 7",
          (sweep_for(ROW_SHIFT) & COL_SHIFT) != 0);
    intv_host_ecs_keys_clear();
    check("clear-all retracts it", sweep_for(ROW_SHIFT) == 0);

    intv_host_stop();

    if (failed)
    {
        fprintf(stderr, "ecs_scan_test: FAILED\n");
        return 1;
    }
    printf("ecs_scan_test: OK\n");
    return 0;
}
