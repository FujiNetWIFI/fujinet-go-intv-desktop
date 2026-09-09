/*
 * ecs_key_event_test -- intvsession_ecs_key_event's held-key bookkeeping,
 * the half of natural ECS typing that no pure-function test can reach.
 *
 * The hazard it exists for: in ECS keyboard mode a press is resolved by the
 * CHARACTER the host layout produced, because the ECS's shifted layer bears
 * no relation to a PC's ('%' is SHIFT+LEFT-ARROW over there, '/' is
 * SHIFT+7). But the character a key reports is not stable across its own
 * press and release -- let go of Shift first and the key-up for "5" arrives
 * as '5' where the key-down was '%'. A release resolved afresh therefore
 * clears the wrong matrix bit, and the one it strands is a fake-shift bit,
 * which pads.c re-reads on EVERY scan (its need_fake_shift loop sweeps all
 * eight rows). One stranded bit leaves the machine reading SHIFT on every
 * later keystroke, so this is a whole-session corruption, not a lost key.
 *
 * White-box like ecs_key_test.c -- reads intv.pad1.k[] directly -- but
 * needs neither ROMs nor intv_host_start: intv_host_ecs_key writes the intv
 * global whether or not the machine is running (see its own contract), so
 * this runs in every build configuration, WITH_INTV_ROMS=OFF included.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "intvsession.h"
#include "intv_host.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "ecs_key_event_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static void check_row(const char *what, int row, uint32_t want)
{
    if (intv.pad1.k[row] != want) {
        fprintf(stderr, "ecs_key_event_test: FAILED: %s: k[%d] = 0x%x, "
                        "want 0x%x\n", what, row, intv.pad1.k[row], want);
        failed = 1;
    }
}

static int rows_clear(void)
{
    for (int row = 0; row < 7; row++)
        if (intv.pad1.k[row] != 0)
            return 0;
    return 1;
}

/* Stand-ins for a frontend's own physical-key ids (GDK keycode, Qt
 * nativeScanCode, AppKit keyCode, Win32 VK). Their values are arbitrary --
 * the contract is only that a key reports the SAME one on press and
 * release, which is exactly what the keysym cannot promise. */
enum { HOST_5 = 6, HOST_LSHIFT = 42, HOST_SLASH = 53, HOST_A = 30,
       HOST_KP7 = 71 };

int main(void)
{
    char config_dir[1024], data_dir[1024];
    intvsession *s;
    intvsession_paths paths;

    test_tmp_template(config_dir, sizeof(config_dir), "intv-ecs-ev-cfg-");
    test_tmp_template(data_dir, sizeof(data_dir), "intv-ecs-ev-data-");
    if (!mkdtemp(config_dir) || !mkdtemp(data_dir)) {
        perror("mkdtemp");
        return 1;
    }
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = config_dir;
    paths.data_dir = data_dir;

    s = intvsession_new(&paths);
    if (!s) {
        fprintf(stderr, "ecs_key_event_test: intvsession_new failed\n");
        return 1;
    }
    intvsession_ecs_keys_clear(s);

    /* ---- THE regression --------------------------------------------------
     * Shift down; "5" down, which the layout reports as '%' (ECS row 0 bit
     * 1 << 8, the fake-shift alias of the LEFT-ARROW key); Shift UP FIRST,
     * so the "5" key-up now carries the character '5'; "5" up. */
    intvsession_ecs_key_event(s, HOST_LSHIFT, INTVSESSION_KEYSYM_LSHIFT, 0, 1);
    check_row("Shift held", 6, 128);
    intvsession_ecs_key_event(s, HOST_5, '5', '%', 1);
    check_row("Shift+5 asserts the '%' fake-shift bit", 0, 1u << 8);

    intvsession_ecs_key_event(s, HOST_LSHIFT, INTVSESSION_KEYSYM_LSHIFT, 0, 0);
    intvsession_ecs_key_event(s, HOST_5, '5', '5', 0);
    check_row("no fake-shift bit stranded by the release", 0, 0);
    check_row("and ECS '5' was not spuriously left down", 3, 0);
    check("every row clear after the sequence", rows_clear());

    /* The same asymmetry on a key whose KEYSYM also moves under Shift --
     * Qt and AppKit both resolve "/" through their own already-shifted
     * text, so the press says '?' and the release says '/'. This is why the
     * table is keyed on the frontend's physical-key id and not on the
     * keysym. */
    intvsession_ecs_key_event(s, HOST_SLASH, '?', '?', 1);
    check_row("Shift+/ asserts the '?' fake-shift bit", 5, 2u << 8);
    intvsession_ecs_key_event(s, HOST_SLASH, '/', '/', 0);
    check("a keysym that moved under Shift still releases", rows_clear());

    /* ---- the numpad stays positional ------------------------------------
     * With NumLock on every toolkit reports '7' here, but KP_7 is ECS "1"
     * (row 5, mask 16) in upstream's keypad-shaped layout -- NOT ECS "7"
     * (row 2, mask 16). Resolving by character alone would silently re-lay
     * the whole numpad. */
    intvsession_ecs_key_event(s, HOST_KP7, INTVSESSION_KEYSYM_KP_7, '7', 1);
    check_row("numpad 7 types ECS '1'", 5, 16);
    check_row("and not ECS '7'", 2, 0);
    intvsession_ecs_key_event(s, HOST_KP7, INTVSESSION_KEYSYM_KP_7, '7', 0);
    check("numpad released", rows_clear());

    /* ---- auto-repeat -----------------------------------------------------
     * GTK and Win32 both deliver repeats on the ECS path. A repeat must not
     * consume a second slot, and one release must still fully clear it. */
    intvsession_ecs_key_event(s, HOST_A, 'a', 'a', 1);
    intvsession_ecs_key_event(s, HOST_A, 'a', 'a', 1);
    intvsession_ecs_key_event(s, HOST_A, 'a', 'a', 1);
    check_row("A held through repeats", 5, 128);
    intvsession_ecs_key_event(s, HOST_A, 'a', 'a', 0);
    check("one release clears a repeated key", rows_clear());

    /* A key whose character CHANGES mid-hold (Shift pressed after the key,
     * with the toolkit still repeating it) must release the old ECS key
     * before asserting the new one, or the old bit stays down for good. */
    intvsession_ecs_key_event(s, HOST_5, '5', '5', 1);
    check_row("5 held", 3, 16);
    intvsession_ecs_key_event(s, HOST_5, '5', '%', 1);   /* repeat, shifted */
    check_row("the old ECS '5' bit was released", 3, 0);
    check_row("and '%' is now held", 0, 1u << 8);
    intvsession_ecs_key_event(s, HOST_5, '5', '%', 0);
    check("re-resolved key releases cleanly", rows_clear());

    /* ---- Ctrl combos -----------------------------------------------------
     * Qt reports no text for one and GDK unicodes it to a control code;
     * either way it has to fall back to the physical key so Ctrl+A reaches
     * ECS CTRL+A rather than nothing. */
    intvsession_ecs_key_event(s, HOST_A, 'a', 0, 1);
    check_row("Ctrl+A still reaches ECS 'A'", 5, 128);
    intvsession_ecs_key_event(s, HOST_A, 'a', 0, 0);
    check("Ctrl+A released", rows_clear());

    /* ---- the HID fallback band ------------------------------------------
     * A key with no curated symbol of its own -- which on Windows is how
     * "/" arrives. The band must still resolve by character, or the key
     * this whole change exists for stays dead there. */
    intvsession_ecs_key_event(s, HOST_SLASH,
                              INTVSESSION_KEYSYM_HID_BASE + 0x38, '/', 1);
    check_row("HID-band '/' types ECS '/'", 2, 16u << 8);
    intvsession_ecs_key_event(s, HOST_SLASH,
                              INTVSESSION_KEYSYM_HID_BASE + 0x38, '/', 0);
    check("HID-band key released", rows_clear());

    /* ---- a character the ECS has no key for ------------------------------
     * '!' must type nothing at all, and its release must not clear
     * something else. */
    intvsession_ecs_key_event(s, '1', '1', '!', 1);
    check("'!' types nothing", rows_clear());
    intvsession_ecs_key_event(s, '1', '1', '1', 0);
    check("its release is a no-op", rows_clear());

    /* ---- the focus-loss escape hatch -------------------------------------
     * More keys held than the table has slots, then a clear. Whatever the
     * overflow policy, nothing may be left down afterwards -- this is the
     * backstop every frontend already calls on focus loss. */
    for (int i = 0; i < 24; i++)
        intvsession_ecs_key_event(s, 1000 + i, 'a' + (i % 26),
                                  'a' + (i % 26), 1);
    intvsession_ecs_keys_clear(s);
    check("clear releases everything after an overflow", rows_clear());

    /* And a release arriving AFTER that clear (the key was still physically
     * down when focus moved away) must not resurrect anything. */
    intvsession_ecs_key_event(s, 1000, 'a', 'a', 0);
    check("a post-clear release is a no-op", rows_clear());

    intvsession_free(s);

    if (failed) {
        fprintf(stderr, "ecs_key_event_test: FAILED\n");
        return 1;
    }
    printf("ecs_key_event_test: OK\n");
    return 0;
}
