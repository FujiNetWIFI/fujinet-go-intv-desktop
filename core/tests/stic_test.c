/*
 * stic_test -- pauses the real running machine, takes a STIC snapshot, and
 * cross-checks the decode (registers, mode, MOBs, card sheet, palette)
 * against values written directly into intv.stic.raw/gmem -- proving the
 * bit layouts in intvstic.h/sticview.c are actually correct, not just that
 * they compile.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "intvdebug.h"
#include "intvstic.h"
#include "roms_embedded.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "stic_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "stic_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    char rom_dir[] = "/tmp/intv-stic-test-XXXXXX";
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65503,
    };
    if (intv_host_start(&opts) != 0) {
        fprintf(stderr, "stic_test: intv_host_start failed\n");
        return 1;
    }
    usleep(50000);

    intvdebug *d = intvdebug_get();
    intvdebug_set_engaged(d, 1);
    intvdebug_pause(d);
    int waited = 0;
    while (!intvdebug_is_paused(d) && waited < 2000) { usleep(1000); waited++; }
    check("paused", intvdebug_is_paused(d));

    /* ---- write known values directly into the live STIC, then confirm
     * the snapshot/decode reports exactly what was written ---- */

    /* MOB 3: x=0x42, visible, x_size=2 (double width); y=0x10 (pixel 32),
     * y_res=16, y_stretch=4, flip_x, flip_y; color=0xB (bit3 from a_reg
     * bit11, bits2-0 from a_reg bits2-0: 0xB=0b1011 -> hi=1,lo=3 ->
     * a_reg bit11=1 (0x800... wait that's also the GRAM bit) -- keep GRAM
     * bit separate: use color 0x5 (0b0101, hi=0,lo=5, no bit11 needed) to
     * avoid conflating with the GRAM-select bit under test separately. */
    intv.stic.raw[3 + 0x00] = 0x0642; /* x=0x42, visible(0x200), x_size(0x400) */
    /* y_reg: y_pos bits[6:0]=0x10, y_res bit7=1(0x80), y_stretch
     * bits[9:8]=2(0x200 -> stretch 4), flip_x bit10(0x400), flip_y
     * bit11(0x800). */
    intv.stic.raw[3 + 0x08] = 0x10 | 0x80 | 0x200 | 0x400 | 0x800;
    intv.stic.raw[3 + 0x10] = 0x005 | 0x2000; /* color=5, priority=1, GRAM=0 */
    intv.stic.raw[3 + 0x18] = 0x0101; /* collided with MOB0 (bit0) + border
                                       * (bit8, 0x100) */

    intv.stic.raw[0x28] = 0x1; intv.stic.raw[0x29] = 0x2;
    intv.stic.raw[0x2A] = 0x3; intv.stic.raw[0x2B] = 0x4;
    intv.stic.raw[0x2C] = 0x7;             /* border color */
    intv.stic.raw[0x30] = 0x3;             /* h delay */
    intv.stic.raw[0x31] = 0x5;             /* v delay */
    intv.stic.mode = 0;                     /* Color Stack */
    intv.stic.gram_size = 1;                /* 128 cards */

    /* A recognisable 8x8 pattern in GRAM card 0 (right after GROM's 256
     * cards -- GROM is a fixed 2048 bytes, per stic_init -- i.e. gmem
     * index 256*8 = 2048): a diagonal line, byte i = 1<<(7-i). */
    for (int i = 0; i < 8; i++)
        intv.stic.gmem[256 * 8 + i] = (uint8_t)(1 << (7 - i));

    intvstic_snapshot snap;
    intvstic_snapshot_get(d, &snap);

    check("mode captured", snap.mode == 0);
    check("gram_size captured", snap.gram_size == 1);
    check("color stack captured",
          (snap.raw[0x28] & 0xF) == 1 && (snap.raw[0x29] & 0xF) == 2 &&
          (snap.raw[0x2A] & 0xF) == 3 && (snap.raw[0x2B] & 0xF) == 4);
    check("border captured", (snap.raw[0x2C] & 0xF) == 7);

    char text[512];
    int n = intvstic_describe_registers(&snap, text, sizeof(text));
    check("describe_registers produced text", n > 0);
    check("describe_registers mentions border 7", strstr(text, "7") != NULL);

    intvstic_mob mob;
    intvstic_mob_get(&snap, 3, &mob);
    check("mob x", mob.x == 0x42);
    check("mob visible", mob.visible == 1);
    check("mob x_size", mob.x_size == 2);
    check("mob y", mob.y == 0x10 * 2);
    check("mob y_res", mob.y_res == 16);
    check("mob y_stretch", mob.y_stretch == 4);
    check("mob flip_x", mob.flip_x == 1);
    check("mob flip_y", mob.flip_y == 1);
    check("mob color", mob.color == 5);
    check("mob priority", mob.priority == 1);
    check("mob gram", mob.gram == 0);
    check("mob collision bits", mob.collision == 0x0101);

    /* A MOB not touched should decode to all-zero/invisible. */
    intvstic_mob mob0;
    intvstic_mob_get(&snap, 0, &mob0);
    check("untouched mob is not visible", mob0.visible == 0);

    /* ---- card sheet: the diagonal pattern in GRAM card 0 (card index 256
     * in gmem terms) round-trips through intvstic_render_cards ---- */
    check("card_count includes GROM+GRAM",
          intvstic_card_count(&snap) == 256 + 128);
    {
        uint8_t *sheet = calloc(1, (size_t)(INTVSTIC_CARDS_PER_ROW * 8) * 8 * 4);
        /* first_card=256 puts our GRAM card 0 at sheet position (0,0). */
        intvstic_render_cards(&snap, 256, 1, sheet);
        int diag_ok = 1;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                const uint8_t *px = &sheet[(y * INTVSTIC_CARDS_PER_ROW * 8 + x) * 4];
                const int expect_set = (x == y); /* our diagonal pattern */
                const int got_set = px[0] != 0;
                if (expect_set != got_set) diag_ok = 0;
            }
        }
        check("rendered card matches the diagonal pattern written", diag_ok);
        free(sheet);
    }

    /* ---- palette: swatch colors match intv.gfx.palette exactly ---- */
    {
        uint8_t *pal = calloc(1, (size_t)(INTVSTIC_PAL_CELL * 16) *
                              INTVSTIC_PAL_CELL * 4);
        intvstic_render_palette(d, pal);
        const uint8_t *px3 = &pal[(3 * INTVSTIC_PAL_CELL) * 4]; /* color 3,
                                                                  * row 0 */
        check("palette swatch 3 matches intv.gfx.palette",
              px3[0] == intv.gfx.palette.color[3][0] &&
              px3[1] == intv.gfx.palette.color[3][1] &&
              px3[2] == intv.gfx.palette.color[3][2]);
        free(pal);
    }

    intvdebug_resume(d);
    intvdebug_set_engaged(d, 0);
    intv_host_stop();

    if (failed) {
        fprintf(stderr, "stic_test: FAILED\n");
        return 1;
    }
    printf("stic_test: OK\n");
    return 0;
}
