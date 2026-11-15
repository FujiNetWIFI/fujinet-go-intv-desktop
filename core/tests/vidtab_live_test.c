/*
 * vidtab_live_test -- the Video tab on a real running machine (the IntyBASIC
 * fixture in tests/data/srcmap, which draws a frame every vertical blank).
 * Running: the host captures frames and the Display view is one of them.
 * Stopped: the picture is the frame the machine published (compared pixel by
 * pixel with the frame store), the registers are the live ones, Break on
 * writes adds the right write breakpoints and a MOB register break fires.
 * Needs the embedded EXEC/GROM; skips (77) without. Builds under MinGW: no
 * setenv.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv_frame.h"
#include "intv_host.h"
#include "intvdebug.h"
#include "intvvidcap.h"
#include "intvvidtab.h"
#include "roms_embedded.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failed = 1;
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

static uint32_t published[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
static intvvid_capture first_cap, second_cap;

int main(void)
{
    char rom_dir[1024];
    intvvid_tab *tab;
    intvdebug *d;
    const uint32_t *pic;
    int wide, high;

    if (intv_embedded_rom_count == 0) {
        fprintf(stderr, "vidtab_live_test: no embedded ROMs (build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-vidtab-test-");
    if (!mkdtemp(rom_dir)) { perror("mkdtemp"); return 1; }

    {
        intv_host_opts opts = {
            .rom_dir = rom_dir,
            .fujinet_host = "127.0.0.1",
            .fujinet_port = 65507,
            .cart_path = SRCMAP_FIXTURE_DIR "/main.bin",
        };
        if (intv_host_start(&opts) != 0) {
            fprintf(stderr, "vidtab_live_test: intv_host_start failed\n");
            return 1;
        }
    }
    d = intvdebug_get();
    intvdebug_set_engaged(d, 1);
    tab = intvvid_tab_new(d);
    check("tab", tab != NULL);

    /* ---- running: the host captures a frame each vertical blank ---- */
    {
        uint64_t serial = 0, start = intv_frame_count();
        for (int waited = 0; waited < 8000 && intv_frame_count() < start + 90; waited += 10) usleep(10000);
        check("the machine ran 90 frames", intv_frame_count() >= start + 90);
        check("the host captured a frame", intv_host_capture_get(&first_cap, &serial) == 1 && serial != 0);
        check("a second get with the same serial copies nothing", intv_host_capture_get(&second_cap, &serial) == 0);
        usleep(200000);
        check("a later get has a newer frame", intv_host_capture_get(&second_cap, &serial) == 1 &&
                                                  second_cap.frame > first_cap.frame);
        check("a captured frame is not live", !first_cap.live && first_cap.vid_enable == 1);
        check("the capture carries the machine's palette",
              first_cap.palette[7][0] != 0 || first_cap.palette[7][1] != 0 || first_cap.palette[7][2] != 0);
    }
    intvvid_tab_set_zoom(tab, 1);
    intvvid_tab_set_overlays(tab, 0);
    check("refresh while running", (intvvid_tab_refresh(tab, 1) & INTVVID_TAB_PICTURE) != 0);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check("Display (running) is 160 x 200", pic && wide == 160 && high == 200);
    check("status says running", strstr(intvvid_tab_status(tab), "Running") != NULL);
    intvvid_tab_set_view(tab, INTVVID_V_BEAM);
    intvvid_tab_refresh(tab, 1);
    check("Beam needs a stopped machine", intvvid_tab_picture(tab, &wide, &high) == NULL && intvvid_tab_why(tab)[0] != '\0');
    intvvid_tab_set_view(tab, INTVVID_V_DISPLAY);

    /* ---- stopped: Display == the published frame ---- */
    intvdebug_pause(d);
    check("paused", wait_paused(d, 3000));
    {
        uint64_t serial = 0;
        int same = 1;
        intvvid_tab_refresh(tab, 1);
        pic = intvvid_tab_picture(tab, &wide, &high);
        check("Display (stopped) is 160 x 200", pic && wide == 160 && high == 200);
        check("status says stopped", strstr(intvvid_tab_status(tab), "Stopped") != NULL);
        check("the frame store has a frame", intv_frame_copy(published, &serial) == 1);
        for (int pix = 0; pic && pix < INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT; pix++)
            same &= (pic[pix] & 0xffffffu) == (published[pix] & 0xffffffu);
        check("Display == the published frame at a stop", pic && same);
    }
    {
        char buf[8192];
        const intvvid_reg *regs;
        intvvid_tab_set_view(tab, INTVVID_V_BEAM);
        intvvid_tab_set_overlays(tab, INTVVID_OV_BEAM);
        intvvid_tab_refresh(tab, 1);
        pic = intvvid_tab_picture(tab, &wide, &high);
        check("Beam (stopped) is the 160 x 200 frame", pic && wide == 160 && high == 200);
        check("the beam is somewhere in the frame", intvvid_tab_beam_permille(tab) >= 0 && intvvid_tab_beam_permille(tab) <= 1000);
        intvvid_tab_set_view(tab, INTVVID_V_DISPLAY);
        intvvid_tab_set_overlays(tab, 0);
        check("registers listed", intvvid_tab_regs(tab, &regs) >= 16 && strcmp(regs[0].name, "MOB0") == 0);
        intvvid_tab_text(tab, INTVVID_S_TIMING, buf, sizeof buf);
        check("timing names the beam line", strstr(buf, "Beam\tline") != NULL);
        check("console text: registers", intvvid_tab_text_for(d, "regs", buf, sizeof buf) > 0 && strstr(buf, "MOB0") != NULL);
        check("console text: bad section", intvvid_tab_text_for(d, "nope", buf, sizeof buf) > 0 && strstr(buf, "no section") != NULL);
        check("console text: default has the status", intvvid_tab_text_for(d, "", buf, sizeof buf) > 0 && strstr(buf, "view") != NULL &&
                                                          strstr(buf, "COLSTK") != NULL);
    }

    /* ---- Break on writes ---- */
    {
        intvdebug_bp list[32];
        int first, count, found = 0;
        intvvid_tab_set_zoom(tab, 2);
        intvvid_tab_refresh(tab, 1);
        intvvid_tab_hover(tab, 10 * 2, (INTVVID_BT_Y + 3) * 2);
        check("a BACKTAB card can be broken on", intvvid_tab_can_break(tab));
        first = intvvid_tab_break_on_pixel(tab);
        check("Break on writes returns an id", first >= 0);
        count = intvdebug_bp_list(d, list, 32);
        for (int i = 0; i < count; i++)
            if (list[i].id == first && (list[i].type & INTVDEBUG_BP_WRITE) && list[i].lo == 0x201 && list[i].hi == 0x201) found = 1;
        check("a write breakpoint on the BACKTAB word $0201", found);
        check("a second call adds nothing", intvvid_tab_break_on_pixel(tab) == first && intvdebug_bp_list(d, list, 32) == count);
        intvdebug_bp_remove(d, first);

        /* the MOB registers: IntyBASIC's interrupt routine rewrites the STIC's MOB registers every frame */
        intvvid_tab_set_view(tab, INTVVID_V_DECODED);
        intvvid_tab_set_source(tab, INTVVID_SRC_MOBS);
        intvvid_tab_refresh(tab, 1);
        intvvid_tab_hover(tab, 4, 4);
        check("MOB 0's tile has three breakpoint targets", intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->nbreak == 3);
        first = intvvid_tab_break_on_pixel(tab);
        check("Break on writes for a MOB", first >= 0 && intvdebug_bp_list(d, list, 32) == 3);
        intvdebug_resume(d);
        check("a MOB register write stops the machine", wait_paused(d, 10000));
        {
            intvdebug_stop_info st;
            check("as a watch write inside $0000-$0017", intvdebug_last_stop(d, &st) && st.reason == INTVDEBUG_STOP_WATCH_WRITE &&
                                                              st.addr <= 0x17);
        }
        intvdebug_breakpoint_clear_all(d);
    }

    intvvid_tab_free(tab);
    intvdebug_resume(d);
    {
        uint64_t serial = 0;
        usleep(300000);
        intv_host_capture_get(&first_cap, &serial);
        usleep(300000);
        check("a freed tab stops the capture (no new frame once the last tab is gone)",
              intv_host_capture_get(&second_cap, &serial) == 0);
    }
    intv_host_stop();
    printf(failed ? "vidtab_live_test: FAILED\n" : "vidtab_live_test: all passed\n");
    return failed;
}
