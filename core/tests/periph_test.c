/*
 * periph_test -- pure bus test, no ROMs and no emulator boot: proves
 * periph_unregister() and icart_unregister() actually drop a device out of
 * the address decode bins.
 *
 * Regression test for pushed carts coming back with their high byte masked
 * off. periph_register() only ever *adds* a device to a bin, which is all a
 * machine built once at startup ever needs -- but fujinet_apply_rom()'s
 * hot-swap re-registers the very same icart_t over a brand new memory map
 * when a pushed cart replaces the running one. Without an unregister, the
 * outgoing config ROM's ranges stay bound alongside the incoming game's, and
 * since periph_read()/periph_peek() return the logical AND of every device in
 * the bin (open-collector bus emulation), the config ROM's 8-bit RAM window
 * over $8000-$9BFF quietly truncated any pushed game with a segment there --
 * the 12K-at-$9000 shape nearly every 32K/48K INTV cart uses. Deep Pockets,
 * Body Slam, Spiker, Pole Position and friends all land in that window.
 *
 * The bin-compaction check matters just as much as the removal itself: every
 * walk of a bin (periph_read/periph_write, and periph_register looking for a
 * free slot) terminates at the first NULL and has no MAX_PERIPH_BIN bound, so
 * punching a hole instead of squeezing the column together would hide every
 * device behind it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "periph/periph.h"
#include "cp1600/cp1600.h"
#include "lzoe/lzoe.h"   /* icart.h's icart_init() takes an LZFILE * */
#include "file/file.h"
#include "icart/icart.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "periph_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static void check_eq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "periph_test: FAILED: %s (got 0x%04X, want 0x%04X)\n",
                what, got, want);
        failed = 1;
    }
}

/* Each stub drives a fixed pattern onto the bus; the bus ANDs them together,
 * so which peripherals are bound is directly readable off the result. */
static uint32_t rd_00FF(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; return 0x00FF; }

static uint32_t rd_ABCD(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; return 0xABCD; }

static uint32_t rd_FFF0(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; return 0xFFF0; }

static uint32_t rd_FF0F(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; return 0xFF0F; }

static uint32_t rd_F0FF(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; return 0xF0FF; }

static int writes_seen = 0;
static void wr_count(periph_t *p, periph_t *r, uint32_t a, uint32_t d)
{ (void)p; (void)r; (void)a; (void)d; writes_seen++; }

/* periph_register() binds the range but leaves addr_base/addr_mask to the
 * caller (see cfg.c and icart_register()); ours pass the address through. */
static void stub(periph_t *p, periph_rd_t *rd, periph_wr_t *wr)
{
    memset(p, 0, sizeof(*p));
    p->read      = rd;
    p->peek      = rd;
    p->write     = wr;
    p->poke      = wr;
    p->addr_base = 0;
    p->addr_mask = 0xFFFF;
    p->min_tick  = 1;
    p->max_tick  = ~0U;
}

/* ------------------------------------------------------------------------ */
/*  The FujiNet hot-swap: config ROM's 8-bit window vs. a pushed game.       */
/* ------------------------------------------------------------------------ */
static void test_hot_swap(void)
{
    periph_bus_t *bus = periph_new(16, 16, 4);  /* same shape as cfg.c */
    periph_t cfg_rom, game;

    stub(&cfg_rom, rd_00FF, wr_count);
    stub(&game,    rd_ABCD, NULL);

    periph_register(bus, &cfg_rom, 0x8000, 0x9BFF, "CfgROM");
    periph_register(bus, &game,    0x9000, 0x9FFF, "Game");

    /* The bug, reproduced: both bound, so the CPU sees the AND and the
     * game's high byte is gone. */
    check_eq("wire-AND masks the pushed game while both are bound",
             periph_read(&bus->periph, NULL, 0x9000, 0xFFFF), 0x00CD);

    periph_unregister(bus, &cfg_rom);

    check_eq("game reads back intact once the config ROM is unregistered",
             periph_read(&bus->periph, NULL, 0x9000, 0xFFFF), 0xABCD);
    check_eq("periph_peek agrees with periph_read",
             periph_peek(&bus->periph, NULL, 0x9000, 0xFFFF), 0xABCD);

    /* $8000 was covered only by the config ROM: it must now be undriven,
     * i.e. the bus identity -- proving the removal swept every bin the
     * device claimed, not just the ones the new map overlapped. */
    check_eq("a range only the outgoing device claimed is fully released",
             periph_read(&bus->periph, NULL, 0x8000, 0xFFFF), 0xFFFF);

    /* Writes have their own decode bins and must be released too. */
    writes_seen = 0;
    periph_write(&bus->periph, NULL, 0x8000, 0x1234);
    periph_write(&bus->periph, NULL, 0x9000, 0x1234);
    check("write bins are released as well", writes_seen == 0);

    /* The device is still on the bus' linked list (it keeps ticking and
     * still gets destructed), so re-registering it works normally. */
    periph_register(bus, &cfg_rom, 0x8000, 0x9BFF, "CfgROM");
    check_eq("re-registering after unregister rebinds the device",
             periph_read(&bus->periph, NULL, 0x8000, 0xFFFF), 0x00FF);

    periph_delete(bus);
}

/* ------------------------------------------------------------------------ */
/*  Bins are columns terminated by the first NULL, so removal has to         */
/*  compact rather than punch a hole.                                        */
/* ------------------------------------------------------------------------ */
static void test_bin_compaction(void)
{
    periph_bus_t *bus = periph_new(16, 16, 4);
    periph_t a, b, c;

    stub(&a, rd_FFF0, NULL);
    stub(&b, rd_FF0F, NULL);
    stub(&c, rd_F0FF, NULL);

    periph_register(bus, &a, 0x5000, 0x5FFF, "A");
    periph_register(bus, &b, 0x5000, 0x5FFF, "B");
    periph_register(bus, &c, 0x5000, 0x5FFF, "C");

    check_eq("all three stubs drive the bus",
             periph_read(&bus->periph, NULL, 0x5000, 0xFFFF), 0xF000);

    /* Drop the middle one. If its slot were merely NULLed, the bin walk
     * would stop there and C -- sitting behind the hole -- would vanish
     * too, leaving 0xFFF0 instead of 0xF0F0. */
    periph_unregister(bus, &b);

    check_eq("removing the middle device leaves the ones behind it bound",
             periph_read(&bus->periph, NULL, 0x5000, 0xFFFF), 0xF0F0);

    periph_delete(bus);
}

/* ------------------------------------------------------------------------ */
/*  icart_unregister() has to cover every attribute flavor icart_register()  */
/*  can bind, not just the plain one.                                        */
/* ------------------------------------------------------------------------ */
static void test_icart_unregister(void)
{
    static icart_t ic;              /* icartrom_t inside is far too big for
                                       the stack */
    periph_bus_t *bus = periph_new(16, 16, 4);
    periph_t *const all[13] = {
        &ic.base,
        &ic.r,   &ic.w,   &ic.rw,
        &ic.rn,  &ic.wn,  &ic.rwn,
        &ic.rb,  &ic.wb,  &ic.rwb,
        &ic.rnb, &ic.wnb, &ic.rwnb
    };
    unsigned i;

    memset(&ic, 0, sizeof(ic));
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        stub(all[i], rd_00FF, wr_count);
        periph_register(bus, all[i], 0x9000, 0x9FFF, "IcartFlavor");
    }

    check_eq("the cart's flavors are bound",
             periph_read(&bus->periph, NULL, 0x9000, 0xFFFF), 0x00FF);

    icart_unregister(&ic, bus);

    check_eq("icart_unregister releases every attribute flavor",
             periph_read(&bus->periph, NULL, 0x9000, 0xFFFF), 0xFFFF);

    writes_seen = 0;
    periph_write(&bus->periph, NULL, 0x9000, 0x1234);
    check("icart_unregister releases the write bins too", writes_seen == 0);

    /* Defensive: the FujiNet path calls this before the cart is fully set
     * up on a failed push. */
    icart_unregister(NULL, bus);
    icart_unregister(&ic, NULL);
    periph_unregister(NULL, &ic.base);
    periph_unregister(bus, NULL);

    periph_delete(bus);
}

int main(void)
{
    test_hot_swap();
    test_bin_compaction();
    test_icart_unregister();

    if (failed) {
        fprintf(stderr, "periph_test: FAILED\n");
        return 1;
    }
    printf("periph_test: OK\n");
    return 0;
}
