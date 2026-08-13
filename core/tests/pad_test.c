/*
 * pad_test -- verifies intv_host_pad_key/intv_host_pad_disc actually reach
 * the running machine's pad0.l[]/r[] state, and that release zeroes it.
 *
 * Reads intv.pad0 directly (declared extern via cfg/cfg.h -- same headers
 * intv_host.c itself needs, in the same order; see that file's own comment
 * on why the order matters) rather than through any public accessor, since
 * this is a white-box test of the injection mechanism, not of a frontend
 * API.
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

static int check(const char *what, int got, int want)
{
    if (got != want)
    {
        fprintf(stderr, "pad_test: %s: got 0x%x, want 0x%x\n", what, got,
                want);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "pad_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    char rom_dir[1024];
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-pad-test-");
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
        fprintf(stderr, "pad_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000); /* let the machine boot before poking its pads */

    int failed = 0;

    /* KP5 on the left controller: press should write exactly the scan code
     * from mapping.c's PD0L_KP5 (0x42); release should zero it. */
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_5, 1);
    failed |= check("left KP5 pressed", intv.pad0.l[5], 0x42);
    failed |= check("left KP5 side effect (right unaffected)",
                    intv.pad0.r[5], 0x00);
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_5, 0);
    failed |= check("left KP5 released", intv.pad0.l[5], 0x00);

    /* Same digit, right controller -- must not touch the left controller's
     * state. */
    intv_host_pad_key(INTV_PAD_RIGHT, INTV_PAD_KEY_5, 1);
    failed |= check("right KP5 pressed", intv.pad0.r[5], 0x42);
    failed |= check("right KP5 side effect (left unaffected)",
                    intv.pad0.l[5], 0x00);
    intv_host_pad_key(INTV_PAD_RIGHT, INTV_PAD_KEY_5, 0);

    /* Action buttons and Clear/Enter, left controller. */
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_ACTION_TOP, 1);
    failed |= check("left action-top pressed", intv.pad0.l[12], 0xA0);
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_ACTION_TOP, 0);
    failed |= check("left action-top released", intv.pad0.l[12], 0x00);

    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_CLEAR, 1);
    failed |= check("left Clear pressed", intv.pad0.l[10], 0x88);
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_CLEAR, 0);

    /* Disc: North (clock position 4) on the left controller. */
    intv_host_pad_disc(INTV_PAD_LEFT, 4);
    failed |= check("left disc North", intv.pad0.l[15], 4);
    /* ESE (clock position 15) is the one non-obvious combo (E|SE = 129). */
    intv_host_pad_disc(INTV_PAD_LEFT, 15);
    failed |= check("left disc ESE", intv.pad0.l[15], 129);
    intv_host_pad_disc(INTV_PAD_LEFT, -1);
    failed |= check("left disc centered", intv.pad0.l[15], 0);

    /* ECS's second controller pair -- intv.pad1.l[]/r[], same scan codes as
     * pad0, selected by intv_pad_side's top bit (see intv_host.h's own
     * comment on INTV_PAD_ECS_LEFT/_RIGHT). Writable regardless of whether
     * ECS is actually enabled this run -- nothing on the peripheral bus
     * reads pad1 unless it is, but the injection itself doesn't gate on
     * that, matching every other intv_host_pad_key call. */
    intv_host_pad_key(INTV_PAD_ECS_LEFT, INTV_PAD_KEY_5, 1);
    failed |= check("ECS-left KP5 pressed", intv.pad1.l[5], 0x42);
    failed |= check("ECS-left KP5 does not disturb pad0",
                    intv.pad0.l[5], 0x00);
    intv_host_pad_key(INTV_PAD_ECS_LEFT, INTV_PAD_KEY_5, 0);
    failed |= check("ECS-left KP5 released", intv.pad1.l[5], 0x00);

    intv_host_pad_disc(INTV_PAD_ECS_RIGHT, 4);
    failed |= check("ECS-right disc North", intv.pad1.r[15], 4);
    failed |= check("ECS-right disc does not disturb ECS-left",
                    intv.pad1.l[15], 0);
    intv_host_pad_disc(INTV_PAD_ECS_RIGHT, -1);
    failed |= check("ECS-right disc centered", intv.pad1.r[15], 0);

    intv_host_stop();

    if (failed)
    {
        fprintf(stderr, "pad_test: FAILED\n");
        return 1;
    }
    printf("pad_test: OK\n");
    return 0;
}
