/*
 * vidtab_test -- the Video tab's controller (core/debugger/vidtab.c) on
 * synthetic captures: no machine runs, the test feeds the captures a machine
 * would take. Views and their sizes, zoom and Fit, the Decoded sources, the
 * Beam view (stopped only), the inspector's cell and MOB lookup, hover and
 * pin, the tables and their change tracking, the text sections, the heat
 * overlay, and a fuzz over random captures that must never crash or return a
 * picture of the wrong size.
 *
 * No setenv/unsetenv or POSIX-only calls: this builds under MinGW too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvvidcap.h"
#include "intvvidtab.h"

static int failures;
static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good) failures++;
}

static intvvid_capture frame_a, frame_b, scratch;

static uint32_t palette_rgb(const intvvid_capture *cap, int idx, int shift)
{
    return 0xff000000u | (uint32_t)(cap->palette[idx][0] >> shift) << 16 | (uint32_t)(cap->palette[idx][1] >> shift) << 8 |
           (uint32_t)(cap->palette[idx][2] >> shift);
}

/* A frame a machine could publish: foreground/background mode, one card on
 * the first BACKTAB cell, MOB 2 on screen, vertical stripes of colour 0-15
 * in the frame. */
static void make_frame(intvvid_capture *cap, uint64_t serial)
{
    memset(cap, 0, sizeof *cap);
    for (int idx = 0; idx < 16; idx++) {
        cap->palette[idx][0] = (uint8_t)(idx * 16);
        cap->palette[idx][1] = (uint8_t)(255 - idx * 16);
        cap->palette[idx][2] = (uint8_t)(idx * 8 + 5);
    }
    for (int posy = 0; posy < INTVVID_H; posy++)
        for (int posx = 0; posx < INTVVID_W; posx++)
            cap->vid[posy * INTVVID_W + posx] = (uint8_t)((posx / 10) & 15);
    cap->vid_enable = 1;
    cap->mode = cap->p_mode = 1;            /* foreground/background */
    cap->gram_size = 1;
    cap->gram_mask = 0x7F8;
    cap->last_intrq = 100000;
    cap->next_intrq = 100000 + 14934;
    cap->cycle = cap->last_intrq;
    cap->frame = serial;
    cap->serial = serial;
    cap->raw[0x28] = 1; cap->raw[0x29] = 2; cap->raw[0x2A] = 3; cap->raw[0x2B] = 4;
    cap->raw[0x2C] = 7;
    /* card 5 (GROM), foreground 3, background 2: only the left pixel of each row is set */
    for (int row = 0; row < 8; row++) cap->gmem[5 * 8 + row] = 0x80;
    cap->btab[0] = (uint16_t)((5 << 3) | 3 | 0x400);
    cap->btab_sr[0] = cap->btab[0];
    cap->btab[41] = 0x1234; cap->btab_sr[41] = 0x1234;
    /* MOB 2: x 40, y 20 lines (reg 10), 8 wide, visible, card 6, colour 5 */
    cap->raw[2] = 0x200 | 40;
    cap->raw[2 + 8] = 10 | 0x80;            /* 16 rows */
    cap->raw[2 + 16] = (6u << 3) | 5;
    cap->raw[2 + 24] = 0x0003;
}

/* A mid-frame stop: the beam on frame row `row`. */
static void park_at(intvvid_capture *cap, int row)
{
    cap->live = 1;
    cap->cycle = cap->last_intrq + (uint64_t)(row + 65) * 57u + 10u;
    cap->fifo_wr_ptr = 60;
    cap->busrq_count = 4;
}

static unsigned rng_state = 12345;
static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFF;
}

int main(void)
{
    intvvid_tab *tab = intvvid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;

    check(tab != NULL, "tab");
    check(intvvid_tab_view(tab) == INTVVID_V_DISPLAY, "starts on Display (no env set)");
    check(intvvid_tab_zoom(tab) == 0, "starts on Fit");
    check(intvvid_tab_overlays(tab) == (INTVVID_OV_MOBS | INTVVID_OV_BEAM), "default overlays");
    {
        int count;
        const intvvid_overlay_info *info = intvvid_overlays(&count);
        unsigned all = 0;
        for (int i = 0; i < count; i++) all |= info[i].bit;
        check(count == 5 && all == INTVVID_OV_ALL, "five overlays, one bit each, covering OV_ALL");
        check(intvvid_section_from_name("reg") == INTVVID_S_REGISTERS && intvvid_section_from_name("t") == INTVVID_S_TIMING &&
              intvvid_section_from_name("zzz") == -1, "sections by prefix");
    }

    /* ---- no machine, nothing fed ---- */
    check((intvvid_tab_refresh(tab, 1) & INTVVID_TAB_OFF) != 0, "no machine: OFF");
    check(intvvid_tab_picture(tab, &wide, &high) == NULL && strstr(intvvid_tab_why(tab), "not running") != NULL,
          "no machine: no picture, and why");
    check(intvvid_tab_break_on_pixel(tab) == -1 && !intvvid_tab_can_break(tab), "no debugger: no Break on writes");

    /* ---- Display ---- */
    make_frame(&frame_a, 1);
    intvvid_tab_feed(tab, &frame_a, 0);
    intvvid_tab_set_zoom(tab, 1);
    intvvid_tab_set_overlays(tab, 0);
    check((intvvid_tab_refresh(tab, 1) & INTVVID_TAB_PICTURE) != 0, "refresh");
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 160 && high == 200, "Display is 160 x 200");
    {
        int same = 1;
        for (int posy = 0; posy < 200; posy++)
            for (int posx = 0; posx < 160; posx++)
                same &= pic[posy * 160 + posx] == palette_rgb(&frame_a, frame_a.vid[posy * 160 + posx], 0);
        check(same, "Display pixels are the frame through the palette");
    }
    frame_a.vid_enable = 0;
    intvvid_tab_feed(tab, &frame_a, 0);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && pic[15 * 160 + 25] == palette_rgb(&frame_a, frame_a.vid[15 * 160 + 25], 1),
          "a blanked frame is shown at half brightness, as published");
    frame_a.vid_enable = 1;
    intvvid_tab_feed(tab, &frame_a, 0);

    /* zoom and Fit */
    intvvid_tab_set_zoom(tab, 3);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 480 && high == 600 && intvvid_tab_effective_zoom(tab) == 3, "zoom 3");
    check(pic[7 * 480 + 20] == palette_rgb(&frame_a, frame_a.vid[2 * 160 + 6], 0), "zoomed pixels repeat");
    intvvid_tab_set_zoom(tab, 0);
    intvvid_tab_set_viewport(tab, 700, 450);
    intvvid_tab_refresh(tab, 1);
    check(intvvid_tab_effective_zoom(tab) == 2, "Fit picks the largest zoom that fits (700 x 450 -> 2)");
    intvvid_tab_set_viewport(tab, 100, 100);
    intvvid_tab_refresh(tab, 1);
    check(intvvid_tab_effective_zoom(tab) == 1, "Fit never goes below 1");
    intvvid_tab_set_zoom(tab, 4);
    intvvid_tab_set_viewport(tab, 5000, 5000);
    intvvid_tab_refresh(tab, 1);

    /* ---- Beam: only when stopped ---- */
    intvvid_tab_set_zoom(tab, 1);
    intvvid_tab_set_view(tab, INTVVID_V_BEAM);
    intvvid_tab_refresh(tab, 1);
    check(intvvid_tab_picture(tab, &wide, &high) == NULL && strstr(intvvid_tab_why(tab), "stopped") != NULL,
          "Beam on a running machine: no picture, says why");
    make_frame(&frame_b, 2);
    park_at(&frame_b, 100);
    intvvid_tab_feed(tab, &frame_b, 1);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 160 && high == 200, "Beam is the 160 x 200 frame");
    check(pic[50 * 160 + 25] == palette_rgb(&frame_b, frame_b.vid[50 * 160 + 25], 0), "above the beam: the frame as it is");
    check(pic[150 * 160 + 25] != palette_rgb(&frame_b, frame_b.vid[150 * 160 + 25], 0) &&
          ((pic[150 * 160 + 25] >> 8) & 255) < ((palette_rgb(&frame_b, frame_b.vid[150 * 160 + 25], 0) >> 8) & 255),
          "below the beam: dimmed");
    check(intvvid_tab_beam_permille(tab) > 400 && intvvid_tab_beam_permille(tab) < 800, "beam position as permille of the frame");
    intvvid_tab_set_overlays(tab, INTVVID_OV_BEAM);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(((pic[100 * 160 + 25] >> 16) & 255) > 150 && ((pic[100 * 160 + 25] >> 8) & 255) < 120, "the Beam overlay draws a red line");
    intvvid_tab_set_overlays(tab, 0);
    intvvid_tab_set_view(tab, INTVVID_V_DISPLAY);

    /* ---- Values ---- */
    intvvid_tab_feed(tab, &frame_a, 0);
    intvvid_tab_set_view(tab, INTVVID_V_VALUES);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 160 && high == 96, "Values is 160 x 96");
    check(pic[0] == pic[7 * 160 + 7] && pic[0] != pic[8] && pic[8] == pic[8 * 160 + 8 + 1 - 1] ,
          "a cell is one flat colour, neighbouring words differ");
    check(pic[8] == pic[9 * 160 + 15], "two empty cells are the same colour");

    /* ---- Decoded sources ---- */
    intvvid_tab_set_view(tab, INTVVID_V_DECODED);
    intvvid_tab_set_source(tab, INTVVID_SRC_CARDS);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 160 && high == 96, "Cards is 160 x 96");
    check(pic && pic[0] == palette_rgb(&frame_a, 3, 0) && pic[1] == palette_rgb(&frame_a, 2, 0) && pic[7] == palette_rgb(&frame_a, 2, 0),
          "Cards: the card's set bit is its foreground (3), the rest its background (2)");
    intvvid_tab_set_source(tab, INTVVID_SRC_MOBS);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 70 && high == 34, "MOBs is 4 x 2 tiles");
    intvvid_tab_set_source(tab, INTVVID_SRC_SHEET);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 128 && high == 24 * 8, "Sheet: 256 GROM + 128 GRAM cards, 16 a row");
    check(pic && pic[5 * 8 + 0] == 0xffffffffu && pic[5 * 8 + 1] == 0xff000000u, "Sheet: card 5's pattern, white on black");
    intvvid_tab_set_source(tab, INTVVID_SRC_PALETTE);
    intvvid_tab_refresh(tab, 1);
    pic = intvvid_tab_picture(tab, &wide, &high);
    check(pic && wide == 64 && high == 64 && pic[0] == palette_rgb(&frame_a, 0, 0) && pic[16] == palette_rgb(&frame_a, 1, 0) &&
          pic[48 * 64 + 48] == palette_rgb(&frame_a, 15, 0), "Palette: 4 x 4 swatches");

    /* ---- inspector: Display ---- */
    intvvid_tab_set_view(tab, INTVVID_V_DISPLAY);
    intvvid_tab_set_zoom(tab, 2);
    intvvid_tab_set_overlays(tab, 0);
    intvvid_tab_refresh(tab, 1);
    check(intvvid_tab_hover(tab, 10 * 2, (INTVVID_BT_Y + 3) * 2) == 1, "hover reports a change");
    {
        const intvvid_pixel *pix = intvvid_tab_pixel(tab);
        check(pix && pix->valid && pix->kind == INTVVID_PK_SCREEN, "hover on the frame is a screen pixel");
        check(pix && pix->frame_x == 10 && pix->frame_y == INTVVID_BT_Y + 3 && pix->colour == 1, "its frame position and colour");
        check(pix && pix->cell == 1 && pix->word_addr == 0x201, "it is over BACKTAB cell 1 ($0201)");
        check(pix && pix->nbreak == 1 && pix->break_lo[0] == 0x201, "Break on writes targets the BACKTAB word");
        check(pix && pix->markx == 8 && pix->marky == INTVVID_BT_Y && pix->markw == 8 && pix->markh == 16, "the mark is the card's box");
    }
    intvvid_tab_hover(tab, 3 * 2, (INTVVID_BT_Y + 2) * 2);
    {
        const intvvid_pixel *pix = intvvid_tab_pixel(tab);
        check(pix && pix->cell == 0 && pix->card == 5 && strstr(pix->text, "GROM card 5 at $3028") != NULL, "cell 0 shows GROM card 5 at $3028");
        check(pix && strstr(pix->text, "foreground") != NULL, "and its colours");
    }
    intvvid_tab_hover(tab, 2, 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->cell == -1 && intvvid_tab_pixel(tab)->nbreak == 0 &&
          strstr(intvvid_tab_pixel(tab)->text, "Border") != NULL, "the border has no card and nothing to break on");
    /* MOB 2: x 40 -> frame x 32, y 20 -> frame y 8, 8 wide, 16 tall */
    intvvid_tab_hover(tab, 34 * 2, 10 * 2);
    {
        const intvvid_pixel *pix = intvvid_tab_pixel(tab);
        check(pix && pix->mob == 2 && (pix->mob_mask & 4) && pix->nbreak == 3 && pix->break_lo[0] == 2 && pix->break_lo[1] == 10 &&
              pix->break_lo[2] == 18, "inside MOB 2's box: its X, Y and attribute registers are the break targets");
        check(pix && strstr(pix->text, "MOB 2 box") != NULL, "and the text names it");
    }
    intvvid_tab_hover(tab, -1, -1);
    check(intvvid_tab_pixel(tab) == NULL, "the pointer left: no pixel");
    intvvid_tab_pin(tab, 10 * 2, (INTVVID_BT_Y + 3) * 2);
    {
        int hover[4], pinr[4];
        const int bits = intvvid_tab_marks(tab, hover, pinr);
        check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->cell == 1 && (bits & 2) && !(bits & 1), "pin keeps the pixel");
        check((bits & 2) && pinr[0] == 16 && pinr[1] == INTVVID_BT_Y * 2 && pinr[2] == 16 && pinr[3] == 32, "marks are scaled by the zoom");
    }
    intvvid_tab_hover(tab, 2, 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->cell == 1, "a pin wins over a hover");
    intvvid_tab_pin(tab, 11 * 2, (INTVVID_BT_Y + 5) * 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->cell == -1, "pinning the same card again unpins (the hover shows)");
    intvvid_tab_hover(tab, -1, -1);

    /* ---- inspector: Decoded ---- */
    intvvid_tab_set_view(tab, INTVVID_V_DECODED);
    intvvid_tab_set_source(tab, INTVVID_SRC_SHEET);
    intvvid_tab_refresh(tab, 1);
    intvvid_tab_hover(tab, 8 * 2 + 1, 2 * 8 * 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->kind == INTVVID_PK_SHEET && intvvid_tab_pixel(tab)->card == 33 &&
          intvvid_tab_pixel(tab)->nbreak == 0 && strstr(intvvid_tab_pixel(tab)->text, "GROM is read-only") != NULL,
          "Sheet: a GROM card is read-only, nothing to break on");
    intvvid_tab_hover(tab, 0, 16 * 8 * 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->card == 256 && intvvid_tab_pixel(tab)->nbreak == 1 &&
          intvvid_tab_pixel(tab)->break_lo[0] == 0x3800 && intvvid_tab_pixel(tab)->break_hi[0] == 0x3807,
          "Sheet: GRAM card 0 is $3800-$3807");
    intvvid_tab_set_source(tab, INTVVID_SRC_PALETTE);
    intvvid_tab_refresh(tab, 1);
    intvvid_tab_hover(tab, 16 * 2 + 2, 2);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->kind == INTVVID_PK_PALETTE && intvvid_tab_pixel(tab)->colour == 1,
          "Palette: swatch 1");
    intvvid_tab_set_source(tab, INTVVID_SRC_MOBS);
    intvvid_tab_refresh(tab, 1);
    intvvid_tab_hover(tab, 2 * 18 * 2 + 4, 3);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->kind == INTVVID_PK_MOB && intvvid_tab_pixel(tab)->mob == 2 &&
          intvvid_tab_pixel(tab)->nbreak == 3, "MOBs: tile 2 and its three registers");
    intvvid_tab_hover(tab, 16 * 2 + 1, 3);
    check(intvvid_tab_pixel(tab) && intvvid_tab_pixel(tab)->kind == INTVVID_PK_NONE && intvvid_tab_pixel(tab)->nbreak == 0,
          "MOBs: the gap between tiles is nothing");
    intvvid_tab_hover(tab, -1, -1);

    /* ---- tables ---- */
    intvvid_tab_set_view(tab, INTVVID_V_DISPLAY);
    intvvid_tab_feed(tab, &frame_a, 0);
    intvvid_tab_refresh(tab, 1);
    {
        const intvvid_reg *regs;
        const intvvid_swatch *swat;
        const intvvid_line_row *rows;
        const char *const *timing;
        long total = 0;
        int count = intvvid_tab_regs(tab, &regs);
        check(count >= 16, "registers listed");
        check(strcmp(regs[2].name, "MOB2") == 0 && strcmp(regs[2].value, "0228 008A 0035 0003") == 0, "MOB2: X Y A and collisions");
        check(strstr(regs[2].meaning, "x 40") != NULL && strstr(regs[2].meaning, "card 6 GROM") != NULL, "MOB2 meaning is decoded");
        {
            int found = 0;
            for (int i = 0; i < count; i++)
                if (!strcmp(regs[i].name, "MODE") && !strcmp(regs[i].value, "FGBG")) found = 1;
            check(found, "MODE shows foreground/background");
        }
        check(intvvid_tab_swatches(tab, &swat) == 16, "16 swatches");
        for (int i = 0; i < 16; i++) total += swat[i].uses;
        check(total == 160 * 200, "colour uses add up to the frame");
        check(swat[7].rgb == palette_rgb(&frame_a, 7, 0) && strstr(swat[7].role, "border") != NULL &&
              strstr(swat[1].role, "stack 0") != NULL, "swatch colours and roles (border 7, colour stack 0 = 1)");
        check(intvvid_tab_lines(tab, &rows) == 12 && rows[0].used == 1 && rows[2].used == 1 && rows[1].used == 0 &&
              rows[3].addr == 0x200u + 60 && rows[1].line == INTVVID_BT_Y + 16 && rows[0].word[0] == frame_a.btab[0],
              "Lines: 12 card rows with their words ($0200 + 20 per row)");
        count = intvvid_tab_timing(tab, &timing);
        check(count >= 8 && strstr(timing[0], "NTSC") != NULL && strstr(timing[1], "14934") != NULL, "timing: NTSC, 14934 clocks");
        frame_b = frame_a;
        frame_b.pal = 1;
        intvvid_tab_feed(tab, &frame_b, 0);
        intvvid_tab_refresh(tab, 1);
        intvvid_tab_timing(tab, &timing);
        check(strstr(timing[0], "PAL") != NULL && strstr(timing[1], "19968") != NULL && strstr(timing[1], "312") != NULL, "timing: PAL, 19968 clocks, 312 lines");
    }

    /* ---- change tracking and the heat overlay ---- */
    make_frame(&frame_a, 10);
    intvvid_tab_feed(tab, &frame_a, 0);
    intvvid_tab_refresh(tab, 1);
    frame_b = frame_a;
    frame_b.serial = frame_b.frame = 11;
    frame_b.raw[2] = 0x200 | 44;            /* MOB 2 moved */
    frame_b.btab[3] = 0x0041;                /* a word changed in row 0 */
    frame_b.btab_sr[3] = 0x0041;
    frame_b.btab[45] = 0x0050;               /* and one in row 2 */
    intvvid_tab_set_zoom(tab, 1);
    intvvid_tab_set_overlays(tab, 0);
    intvvid_tab_feed(tab, &frame_b, 0);
    intvvid_tab_refresh(tab, 1);
    {
        const intvvid_reg *regs;
        const intvvid_line_row *rows;
        static uint32_t plain[160 * 200];
        const uint32_t *pix;
        intvvid_tab_regs(tab, &regs);
        check(regs[2].age == 0 && regs[3].age > 0, "a register that changed has age 0, the others older");
        intvvid_tab_lines(tab, &rows);
        check(rows[0].changed == 1 && rows[1].changed == 0 && rows[2].changed == 1, "Lines: changed words per row");
        pix = intvvid_tab_picture(tab, &wide, &high);
        memcpy(plain, pix, sizeof plain);
        intvvid_tab_set_overlays(tab, INTVVID_OV_HEAT);
        intvvid_tab_refresh(tab, 1);
        pix = intvvid_tab_picture(tab, &wide, &high);
        check(memcmp(plain, pix, sizeof plain) != 0, "Recent writes tints the changed words");
        check(pix[(INTVVID_BT_Y + 3) * 160 + 3 * 8 + 2] != plain[(INTVVID_BT_Y + 3) * 160 + 3 * 8 + 2], "...on the changed cell");
        check(pix[(INTVVID_BT_Y + 3) * 160 + 6 * 8 + 2] == plain[(INTVVID_BT_Y + 3) * 160 + 6 * 8 + 2], "...and not on an untouched one");
        intvvid_tab_set_overlays(tab, INTVVID_OV_MOBS);
        intvvid_tab_refresh(tab, 1);
        pix = intvvid_tab_picture(tab, &wide, &high);
        check(memcmp(plain, pix, sizeof plain) != 0, "MOB boxes draw");
        intvvid_tab_set_overlays(tab, INTVVID_OV_BORDER);
        intvvid_tab_refresh(tab, 1);
        pix = intvvid_tab_picture(tab, &wide, &high);
        check(pix[1 * 160 + 80] != plain[1 * 160 + 80] && pix[(INTVVID_BT_Y + 20) * 160 + 80] == plain[(INTVVID_BT_Y + 20) * 160 + 80],
              "the border overlay tints the border and not the cards");
        intvvid_tab_set_overlays(tab, INTVVID_OV_GRID);
        intvvid_tab_set_zoom(tab, 4);
        intvvid_tab_refresh(tab, 1);
        pix = intvvid_tab_picture(tab, &wide, &high);
        check(pix && wide == 640 && pix[(INTVVID_BT_Y * 4 + 5) * 640 + 0] != palette_rgb(&frame_b, frame_b.vid[(INTVVID_BT_Y + 1) * 160], 0),
              "the grid draws a line on a card edge");
        intvvid_tab_set_overlays(tab, 0);
    }

    /* ---- text sections ---- */
    {
        char buf[16384];
        int num = intvvid_tab_text(tab, INTVVID_S_REGISTERS, buf, sizeof buf);
        check(num > 100 && strstr(buf, "MOB0") != NULL && strstr(buf, "COLSTK") != NULL && strstr(buf, "DELAY") != NULL, "text: registers");
        num = intvvid_tab_text(tab, INTVVID_S_COLOURS, buf, sizeof buf);
        check(strstr(buf, "Tan") != NULL && strstr(buf, "pixels") != NULL, "text: colours");
        num = intvvid_tab_text(tab, INTVVID_S_LINES, buf, sizeof buf);
        check(strstr(buf, "row 11") != NULL && strstr(buf, "$02DC") != NULL, "text: lines");
        num = intvvid_tab_text(tab, INTVVID_S_TIMING, buf, sizeof buf);
        check(strstr(buf, "NTSC") != NULL && strstr(buf, "BACKTAB fetch") != NULL, "text: timing");
        num = intvvid_tab_text(tab, INTVVID_S_PIXEL, buf, sizeof buf);
        check(strstr(buf, "view") != NULL, "text: pixel/status");
        num = intvvid_tab_text(tab, INTVVID_S_TIMING, buf, 40);
        check(num < 40 && (int)strlen(buf) == num, "text: a small buffer is respected");
        check(strstr(intvvid_tab_status(tab), "Display view") != NULL, "status line");
    }

    /* ---- parked, mid-fetch ---- */
    make_frame(&frame_a, 20);
    park_at(&frame_a, 30);
    intvvid_tab_feed(tab, &frame_a, 1);
    intvvid_tab_refresh(tab, 1);
    {
        const intvvid_line_row *rows;
        intvvid_tab_lines(tab, &rows);
        check(rows[0].fetched && rows[2].fetched && !rows[3].fetched, "Lines: rows the STIC has not fetched yet (60 words = 3 rows)");
    }

    /* ---- fuzz: random captures, views, sources, zooms, hovers ---- */
    {
        static const int k_zoom[6] = { 0, 1, 2, 3, 8, 16 };
        int ok = 1;
        intvvid_tab_set_overlays(tab, INTVVID_OV_ALL);
        for (int round = 0; round < 300; round++) {
            unsigned char *raw = (unsigned char *)&scratch;
            int stopped = (int)(rnd() & 1);
            for (size_t i = 0; i < sizeof scratch; i++) raw[i] = (unsigned char)rnd();
            scratch.mode = (int)(rnd() & 1);
            scratch.p_mode = (int)(rnd() & 1);
            scratch.gram_size = (int)(rnd() % 3);
            scratch.gram_mask = (uint16_t)(rnd() & 0xFF8);
            scratch.pal = (int)(rnd() & 1);
            scratch.vid_enable = (int)(rnd() & 1);
            scratch.live = stopped;
            scratch.fifo_wr_ptr = (int)(rnd() % 300);
            scratch.cycle = scratch.last_intrq + (rnd() * 3u);
            scratch.serial = (uint64_t)round + 100;
            for (int i = 0; i < INTVVID_W * INTVVID_H; i++) scratch.vid[i] = (uint8_t)rnd();
            intvvid_tab_feed(tab, &scratch, stopped);
            intvvid_tab_set_view(tab, (int)(rnd() % INTVVID_V_COUNT));
            intvvid_tab_set_source(tab, (int)(rnd() % INTVVID_SRC_COUNT));
            intvvid_tab_set_zoom(tab, k_zoom[rnd() % 6]);
            intvvid_tab_set_viewport(tab, (int)(rnd() % 1500) + 1, (int)(rnd() % 1500) + 1);
            intvvid_tab_refresh(tab, 1);
            pic = intvvid_tab_picture(tab, &wide, &high);
            if (pic) {
                const int scale = intvvid_tab_effective_zoom(tab);
                if (wide <= 0 || high <= 0 || wide % scale || high % scale) ok = 0;
                intvvid_tab_hover(tab, (int)(rnd() % (unsigned)(wide + 20)) - 10, (int)(rnd() % (unsigned)(high + 20)) - 10);
                if (rnd() & 1) intvvid_tab_pin(tab, (int)(rnd() % (unsigned)wide), (int)(rnd() % (unsigned)high));
                {
                    int hover[4], pinr[4];
                    intvvid_tab_marks(tab, hover, pinr);
                }
            } else if (intvvid_tab_why(tab)[0] == '\0') {
                ok = 0;
            }
            {
                char buf[8192];
                intvvid_tab_text(tab, (int)(rnd() % INTVVID_S_COUNT), buf, sizeof buf);
            }
        }
        check(ok, "fuzz: 300 random captures: pictures keep their size, a missing picture says why");
    }

    intvvid_tab_free(tab);
    printf(failures ? "vidtab_test: %d FAILED\n" : "vidtab_test: all passed\n", failures);
    return failures ? 1 : 0;
}
