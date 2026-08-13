/*
 * ecs_key_test -- verifies intv_host_ecs_key writes the exact intv.pad1.k[]
 * bits mapping.c's "KEYB_*" entries define (see intv_host.h's ecs_key_codes
 * table for which mirrors which), that chording ORs rather than overwrites,
 * and that intv_host_ecs_keys_clear zeroes every row.
 *
 * Same white-box approach as pad_test.c: reads intv.pad1.k[] directly
 * rather than through any public accessor.
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

static int check(const char *what, uint32_t got, uint32_t want)
{
    if (got != want)
    {
        fprintf(stderr, "ecs_key_test: %s: got 0x%x, want 0x%x\n", what, got,
                want);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "ecs_key_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    char rom_dir[1024];
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-ecs-key-test-");
    if (!mkdtemp(rom_dir))
    {
        perror("mkdtemp");
        return 1;
    }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65503,
    };
    if (intv_host_start(&opts) != 0)
    {
        fprintf(stderr, "ecs_key_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000);

    int failed = 0;

    /* KEYB_A: row 5, mask 128 (mapping.c "A" -> KEYB_A -> pad1.k[5] |= 128). */
    intv_host_ecs_key(INTV_ECS_KEY_A, 1);
    failed |= check("A pressed", intv.pad1.k[5], 128);
    intv_host_ecs_key(INTV_ECS_KEY_A, 0);
    failed |= check("A released", intv.pad1.k[5], 0);

    /* KEYB_ESC: row 0, mask 16. */
    intv_host_ecs_key(INTV_ECS_KEY_ESC, 1);
    failed |= check("ESC pressed", intv.pad1.k[0], 16);
    intv_host_ecs_key(INTV_ECS_KEY_ESC, 0);
    failed |= check("ESC released", intv.pad1.k[0], 0);

    /* KEYB_SHIFT: row 6, mask 128 -- the one row with only a single bit
     * used (mapping.c has no other row-6 KEYB_* entry). */
    intv_host_ecs_key(INTV_ECS_KEY_SHIFT, 1);
    failed |= check("Shift pressed", intv.pad1.k[6], 128);
    intv_host_ecs_key(INTV_ECS_KEY_SHIFT, 0);
    failed |= check("Shift released", intv.pad1.k[6], 0);

    /* Chording: A (row5 mask128) held with 1 (row5 mask16) should OR into
     * the same row, and releasing one must not clear the other's bit --
     * unlike the disc, which always overwrites. */
    intv_host_ecs_key(INTV_ECS_KEY_A, 1);
    intv_host_ecs_key(INTV_ECS_KEY_1, 1);
    failed |= check("A+1 chorded", intv.pad1.k[5], 128 | 16);
    intv_host_ecs_key(INTV_ECS_KEY_1, 0);
    failed |= check("releasing 1 leaves A held", intv.pad1.k[5], 128);
    intv_host_ecs_key(INTV_ECS_KEY_A, 0);
    failed |= check("releasing A clears the row", intv.pad1.k[5], 0);

    /* Clear-all: hold several keys across different rows, then confirm
     * every row zeroes. */
    intv_host_ecs_key(INTV_ECS_KEY_Q, 1);   /* row 5 */
    intv_host_ecs_key(INTV_ECS_KEY_L, 1);   /* row 1 */
    intv_host_ecs_key(INTV_ECS_KEY_SHIFT, 1); /* row 6 */
    intv_host_ecs_keys_clear();
    for (int row = 0; row < 7; row++)
    {
        char what[32];
        snprintf(what, sizeof(what), "row %d cleared", row);
        failed |= check(what, intv.pad1.k[row], 0);
    }

    intv_host_stop();

    if (failed)
    {
        fprintf(stderr, "ecs_key_test: FAILED\n");
        return 1;
    }
    printf("ecs_key_test: OK\n");
    return 0;
}
