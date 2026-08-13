/*
 * hwopts_test -- verifies intv_host_start's ecs/ivoice/pal options actually
 * reach jzIntv's cfg_t intv global (the argv it builds gets parsed into
 * these fields by cfg_init -- see intv_host.c's own comment on why this is
 * the argv route, not a struct-field one).
 *
 * Same white-box approach as pad_test.c: reads intv.ecs_enable/ivc_enable/
 * pal_mode/stic.pal/speed.periph.min_tick directly, one intv_host_start per
 * case (each case gets its own fresh ROM dir + emulator thread, since
 * intv_host_stop/start is the only way to change these -- there is no live
 * reconfiguration API, matching the plan's choice of the boring stop/start
 * over jzIntv's own risky do_reload path).
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

static void check(const char *what, long got, long want)
{
    if (got != want)
    {
        fprintf(stderr, "hwopts_test: %s: got %ld, want %ld\n", what, got,
                want);
        failed = 1;
    }
}

/* One case's own ROM dir (ECS needs ecs.bin materialised there by
 * intv_host_provision_roms, which intv_host_start calls itself) and a fresh
 * boot/settle/stop cycle. */
static int run_case(int ecs, int ivoice, int pal)
{
    char rom_dir[1024];
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-hwopts-test-");
    if (!mkdtemp(rom_dir))
    {
        perror("mkdtemp");
        return -1;
    }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65503,
        .ecs = ecs,
        .ivoice = ivoice,
        .pal = pal,
    };
    if (intv_host_start(&opts) != 0)
    {
        fprintf(stderr, "hwopts_test: intv_host_start failed\n");
        return -1;
    }
    usleep(50000); /* let cfg_init finish before reading intv.* */
    return 0;
}

int main(void)
{
    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "hwopts_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    /* ECS off, Intellivoice off, NTSC: today's unchanged default shape. */
    if (run_case(INTV_HW_OFF, INTV_HW_OFF, 0) != 0)
        return 1;
    check("default ecs_enable", intv.ecs_enable, 0);
    check("default ivc_enable", intv.ivc_enable, 0);
    check("default pal_mode", intv.pal_mode, 0);
    check("default stic.pal", intv.stic.pal, 0);
    intv_host_stop();

    /* ECS on. */
    if (run_case(INTV_HW_ON, INTV_HW_OFF, 0) != 0)
        return 1;
    check("ecs_enable", intv.ecs_enable, 1);
    intv_host_stop();

    /* Intellivoice on. */
    if (run_case(INTV_HW_OFF, INTV_HW_ON, 0) != 0)
        return 1;
    check("ivc_enable", intv.ivc_enable, 1);
    intv_host_stop();

    /* PAL: pal_mode, the STIC's own copy of it, and speed.c's tick bounds
     * (14934 NTSC vs 19968 PAL, per cfg.c's speed_init call -- see
     * intv_host.h's own comment on where these numbers come from). */
    if (run_case(INTV_HW_OFF, INTV_HW_OFF, 1) != 0)
        return 1;
    check("pal_mode", intv.pal_mode, 1);
    check("stic.pal", intv.stic.pal, 1);
    check("speed min_tick", intv.speed.periph.min_tick, 19968);
    check("speed max_tick", intv.speed.periph.max_tick, 19968);
    intv_host_stop();

    if (failed)
    {
        fprintf(stderr, "hwopts_test: FAILED\n");
        return 1;
    }
    printf("hwopts_test: OK\n");
    return 0;
}
