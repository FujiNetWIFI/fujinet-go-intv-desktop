/*
 * vidtab.c -- the Video tab's controller (core/include/intvvidtab.h).
 *
 * Everything here works from an intvvid_capture: the STIC's registers, the
 * BACKTAB, GRAM and the published frame. The Decoded views rebuild their
 * picture from the registers alone through the sticview renderers, so on an
 * unchanged frame the cards are the cards the machine drew.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "intvvidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "intv_host.h"
#include "intvstic.h"

#define FRAME_W INTVVID_W
#define FRAME_H INTVVID_H
#define BT_PIX_W (INTVVID_CARD_COLS * 8)    /* 160 */
#define BT_PIX_H (INTVVID_CARD_ROWS * 8)    /* 96 */
#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_REGS 40
#define MAX_TIMING 16
#define HEAT_FRAMES 24          /* changes are drawn this many captures */
#define MOB_TILE 16
#define MOB_PITCH 18
#define SHEET_COLS 16

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_CARDS, BK_VALUES, BK_MOBS, BK_SHEET, BK_PALETTE };   /* what the base picture is */

#define COL_BLACK 0xff000000u
#define COL_PANEL 0xff202428u

struct intvvid_tab {
    intvdebug *dbg;
    int view, source, zoom, vieww, viewh;
    unsigned ovl;
    int env_section, capture_on;

    /* the machine's state as presented */
    intvvid_capture *cap;       /* working capture */
    intvvid_capture *last;      /* last captured by the emulator thread */
    uint64_t last_serial;
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig;

    /* what changed lately */
    uint16_t prev_btab[INTVVID_CARDS];
    uint32_t prev_mobregs[24];
    int heat[INTVVID_CARDS];
    int mob_heat[8];
    int line_changed[INTVVID_CARD_ROWS];
    int tracked;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int dirty;
    char why[200];

    /* tables */
    intvvid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][32];
    int nregs, regs_primed;
    intvvid_swatch swat[16];
    intvvid_line_row rows[INTVVID_CARD_ROWS];
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hovx, hovy, pinx, piny;         /* -1: none */
    intvvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const intvvid_overlay_info k_overlays[] = {
    { INTVVID_OV_GRID,   "grid",   "Card grid" },
    { INTVVID_OV_MOBS,   "mobs",   "MOB boxes" },
    { INTVVID_OV_BORDER, "border", "Border and blanking" },
    { INTVVID_OV_BEAM,   "beam",   "Beam" },
    { INTVVID_OV_HEAT,   "heat",   "Recent writes" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const intvvid_overlay_info *intvvid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Cards", "MOBs", "Card sheet", "Palette" };
static const char *const k_src_keys[] = { "cards", "mobs", "sheet", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };
static const char *const k_colour_names[16] = {
    "Black", "Blue", "Red", "Tan", "Dark green", "Green", "Yellow", "White",
    "Grey", "Cyan", "Orange", "Brown", "Pink", "Light blue", "Yellow-green", "Purple" };

const char *intvvid_view_name(int val) { return val >= 0 && val < INTVVID_V_COUNT ? k_view_names[val] : ""; }
const char *intvvid_view_key(int val) { return val >= 0 && val < INTVVID_V_COUNT ? k_view_keys[val] : ""; }
const char *intvvid_source_name(int val) { return val >= 0 && val < INTVVID_SRC_COUNT ? k_src_names[val] : ""; }
const char *intvvid_source_key(int val) { return val >= 0 && val < INTVVID_SRC_COUNT ? k_src_keys[val] : ""; }
const char *intvvid_section_name(int str) { return str >= 0 && str < INTVVID_S_COUNT ? k_sec_names[str] : ""; }
const char *intvvid_section_key(int str) { return str >= 0 && str < INTVVID_S_COUNT ? k_sec_keys[str] : ""; }

int intvvid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (!strcasecmp(name, "regs")) return INTVVID_S_REGISTERS;
    for (int sec = 0; sec < INTVVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

const int *intvvid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *pixp, size_t num)
{
    const uint8_t *blu = pixp;
    while (num--) { high ^= *blu++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(intvvid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

static uint32_t pal_rgb(const intvvid_capture *cap, int index, int shift)
{
    const int idx = index & 15;
    return 0xff000000u | (uint32_t)(cap->palette[idx][0] >> shift) << 16 |
           (uint32_t)(cap->palette[idx][1] >> shift) << 8 | (uint32_t)(cap->palette[idx][2] >> shift);
}

/* the false colour of a BACKTAB word: the low byte sets red and a triangle of
 * green, the high bits blue */
static uint32_t value_colour(unsigned word)
{
    const int low = (int)(word & 255), high = (int)((word >> 8) & 63);
    const int tri = low < 128 ? low * 2 : (255 - low) * 2;
    return 0xff000000u | (uint32_t)(40 + low * 215 / 255) << 16 |
           (uint32_t)(40 + tri * 160 / 255) << 8 | (uint32_t)(255 - high * 200 / 63);
}

static int hdly(const intvvid_capture *cap) { return (int)(cap->raw[0x30] & 7); }
static int vdly(const intvvid_capture *cap) { return (int)(cap->raw[0x31] & 7); }
/* where the BACKTAB's first card sits in the 160 x 200 frame */
static int bt_x0(const intvvid_capture *cap) { return INTVVID_BT_X + hdly(cap); }
static int bt_y0(const intvvid_capture *cap) { return INTVVID_BT_Y + 2 * vdly(cap); }

/* ---- the STIC's own numbers (stic_timings.h) ------------------------------------------------- */

static int frame_clocks(const intvvid_capture *cap) { return cap->pal ? 19968 : 14934; }
static int scan_clocks(const intvvid_capture *cap) { return cap->pal ? 64 : 57; }
static int frame_lines(const intvvid_capture *cap) { return frame_clocks(cap) / scan_clocks(cap); }

/* The scan line the frame's first line is drawn on: the first BACKTAB fetch is
 * the GMEM window plus a few clocks after the INTRQ (stic_generate_reqs), and
 * the first card row starts BT_Y lines below the frame's top. An estimate
 * read off that schedule, not a measurement. */
static int top_scanline(const intvvid_capture *cap)
{
    const int clocks = cap->pal ? 7456 + 160 : 3796 + 143;
    return clocks / scan_clocks(cap) - INTVVID_BT_Y + 2 * vdly(cap);
}

/* the scan line the machine has reached (parked), 0 when it is not parked */
static int beam_scan(const intvvid_capture *cap)
{
    if (!cap->live || cap->cycle < cap->last_intrq) return 0;
    return (int)((cap->cycle - cap->last_intrq) / (uint64_t)scan_clocks(cap));
}

/* the frame line the beam is on: <0 before the frame, >= FRAME_H after it */
static int beam_row(const intvvid_capture *cap)
{
    return beam_scan(cap) - top_scanline(cap);
}

/* ---- BACKTAB words ------------------------------------------------------------------------------ */

typedef struct {
    int squares;                /* colour stack mode's "coloured squares" card */
    int card;                   /* index into the card sheet (GROM 0-255, GRAM 256+) */
    int gram;
    int fg, bg;                 /* palette indices; bg is -1 for squares */
    int advance;                /* colour stack mode: this word advances the stack */
    int quad[4];                /* squares: the four 2 x 2 quadrants' colours */
    int stack_index;            /* colour stack position after the word */
} cell_info;

/* Decodes cell `cell` the way stic_draw_cstk / stic_draw_fgbg do, carrying the
 * colour stack through the words before it. */
static void cell_decode(const intvvid_capture *cap, int cell, cell_info *out)
{
    int stack = 0, background = (int)(cap->raw[0x28] & 0xF);

    memset(out, 0, sizeof *out);
    out->fg = out->bg = -1;
    out->card = -1;
    for (int idx = 0; idx <= cell && idx < INTVVID_CARDS; idx++) {
        const unsigned word = cap->btab[idx];
        memset(out, 0, sizeof *out);
        out->fg = out->bg = -1;
        out->card = -1;
        if (cap->mode == 0 && (word & 0x1800) == 0x1000) {
            out->squares = 1;
            out->quad[0] = (int)((word >> 0) & 7);
            out->quad[1] = (int)((word >> 3) & 7);
            out->quad[2] = (int)((word >> 6) & 7);
            out->quad[3] = (int)(((word >> 9) & 3) | ((word >> 11) & 4));
            for (int quad = 0; quad < 4; quad++)
                if (out->quad[quad] == 7) out->quad[quad] = background;
        } else if (cap->mode == 0) {
            unsigned offset;
            if (word & 0x2000) {
                stack = (stack + 1) & 3;
                background = (int)(cap->raw[0x28 + stack] & 0xF);
                out->advance = 1;
            }
            offset = (word & 0xFF8u) & ((word & 0x800u) ? cap->gram_mask : ~0u);
            out->card = (int)(offset >> 3);
            out->gram = offset >= 0x800u;
            out->fg = (int)(((word >> 9) & 0x8u) | (word & 7u));
            out->bg = background;
        } else {
            const unsigned offset = word & 0x9F8u;
            out->card = (int)(offset >> 3);
            out->gram = offset >= 0x800u;
            out->fg = (int)(word & 7u);
            out->bg = (int)(((word >> 9) & 0xBu) | ((word >> 11) & 0x4u));
        }
        out->stack_index = stack;
    }
}

static unsigned card_addr(int card, int *grom)
{
    *grom = card < 256;
    return *grom ? 0x3000u + (unsigned)card * 8u : 0x3800u + (unsigned)(card - 256) * 8u;
}

/* ---- MOBs ------------------------------------------------------------------------------------- */

typedef struct {
    intvstic_mob mob;
    int interact;               /* x_reg bit 8: collides though it may be invisible */
    int box_x, box_y, box_w, box_h;     /* in frame pixels, delays applied */
    int live;                   /* on screen and worth drawing a box for */
} mob_geom;

static const int k_mob_heights[8] = { 8, 16, 16, 32, 32, 64, 64, 128 };

static void mob_geometry(const intvvid_capture *cap, const intvstic_snapshot *snap, int index, mob_geom *out)
{
    const uint32_t xreg = cap->raw[index], yreg = cap->raw[index + 8];
    int rows;

    intvstic_mob_get(snap, index, &out->mob);
    out->interact = (xreg & 0x100) != 0;
    rows = k_mob_heights[(yreg >> 7) & 7];
    /* the MOB plane is image coordinates: frame x = x + 8 - 8, frame y = y - 12 + delay */
    out->box_x = out->mob.x - 8 + hdly(cap);
    out->box_y = out->mob.y - 12 + 2 * vdly(cap);
    out->box_w = (xreg & 0x400) ? 16 : 8;
    out->box_h = rows;
    out->live = out->mob.x != 0 && out->mob.x < 167 && (xreg & 0x300) != 0 && out->mob.y < 208;
}

static uint32_t mob_colour(int index)
{
    static const uint32_t k_mob_cols[8] = { 0xffff5050, 0xff50ff70, 0xff60a0ff, 0xffffe050,
                                            0xffff60ff, 0xff50f0f0, 0xffffa040, 0xffc0c0c0 };
    return k_mob_cols[index & 7];
}

/* ---- new / free / settings -------------------------------------------------------------- */

intvvid_tab *intvvid_tab_new(intvdebug *dbg)
{
    intvvid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->dbg = dbg;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    if (!tab->cap || !tab->last) {
        intvvid_tab_free(tab);
        return NULL;
    }
    tab->view = INTVVID_V_DISPLAY;
    tab->source = INTVVID_SRC_CARDS;
    tab->ovl = INTVVID_OV_MOBS | INTVVID_OV_BEAM;
    tab->zoom = 0;
    tab->vieww = FRAME_W * 2;
    tab->viewh = FRAME_H * 2;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->dirty = 1;

    if ((envv = getenv("INTV_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int i = 0; i < INTVVID_V_COUNT; i++)
            if (!strncasecmp(envv, k_view_keys[i], strlen(envv))) { tab->view = i; break; }
    }
    if ((envv = getenv("INTV_DEBUGGER_VIDEO_SOURCE")) && *envv) {
        for (int i = 0; i < INTVVID_SRC_COUNT; i++)
            if (!strncasecmp(envv, k_src_keys[i], strlen(envv))) { tab->source = i; break; }
    }
    if ((envv = getenv("INTV_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
            if (!strcasecmp(tok, "all")) ovl |= INTVVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ovl |= k_overlays[i].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("INTV_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = INTVVID_S_PIXEL;
    if ((envv = getenv("INTV_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = intvvid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    intv_host_capture_enable(1);
    tab->capture_on = 1;
    return tab;
}

void intvvid_tab_free(intvvid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        intv_host_capture_enable(0);
    free(tab->cap);
    free(tab->last);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void intvvid_tab_set_view(intvvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < INTVVID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int intvvid_tab_view(const intvvid_tab *tab) { return tab ? tab->view : 0; }
void intvvid_tab_set_source(intvvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < INTVVID_SRC_COUNT && val != tab->source) { tab->source = val; tab->dirty = 1; }
}
int intvvid_tab_source(const intvvid_tab *tab) { return tab ? tab->source : 0; }
void intvvid_tab_set_overlays(intvvid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & INTVVID_OV_ALL) != tab->ovl) { tab->ovl = ovl & INTVVID_OV_ALL; tab->dirty = 1; }
}
unsigned intvvid_tab_overlays(const intvvid_tab *tab) { return tab ? tab->ovl : 0; }
void intvvid_tab_set_zoom(intvvid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int intvvid_tab_zoom(const intvvid_tab *tab) { return tab ? tab->zoom : 0; }
void intvvid_tab_set_viewport(intvvid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int intvvid_tab_env_section(const intvvid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

void intvvid_tab_feed(intvvid_tab *tab, const intvvid_capture *cap, int stopped)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->serial != 0;
    tab->stopped = stopped != 0;
    tab->has_live = tab->stopped;
}

/* Take what the machine has. 0 if there is no machine. */
static int acquire(intvvid_tab *tab)
{
    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!intv_host_is_running())
        return 0;
    intv_host_capture_get(tab->last, &tab->last_serial);
    tab->has_fb = tab->last->serial != 0;
    tab->stopped = tab->dbg && intvdebug_is_paused(tab->dbg);
    tab->has_live = tab->stopped;

    if (tab->stopped) {
        /* a parked machine is ours to read: the registers, BACKTAB and GRAM as
         * they stand, and the frame it last published */
        intv_host_capture_fill(tab->cap);
    } else if (tab->has_fb) {
        memcpy(tab->cap, tab->last, sizeof *tab->cap);
    }
    tab->have = 1;
    return 1;
}

static uint64_t signature(const intvvid_tab *tab)
{
    const intvvid_capture *cap = tab->cap;
    uint64_t high = 1469598103934665603ull;
    high = fnv(high, cap->raw, sizeof cap->raw);
    high = fnv(high, cap->gmem, sizeof cap->gmem);
    high = fnv(high, cap->mob_bmp, sizeof cap->mob_bmp);
    high = fnv(high, cap->btab, sizeof cap->btab);
    high = fnv(high, cap->btab_sr, sizeof cap->btab_sr);
    high = fnv(high, cap->vid, sizeof cap->vid);
    high = fnv(high, cap->palette, sizeof cap->palette);
    high = fnv(high, &cap->vid_enable, sizeof cap->vid_enable);
    high = fnv(high, &cap->mode, sizeof cap->mode);
    high = fnv(high, &cap->p_mode, sizeof cap->p_mode);
    high = fnv(high, &cap->cycle, sizeof cap->cycle);
    high = fnv(high, &cap->frame, sizeof cap->frame);
    high = fnv(high, &cap->serial, sizeof cap->serial);
    high = fnv(high, &cap->fifo_wr_ptr, sizeof cap->fifo_wr_ptr);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    high = fnv(high, &tab->has_fb, sizeof tab->has_fb);
    return high;
}

/* What changed since the capture before (BACKTAB words, MOB registers), for
 * the heat overlay and the Lines table. Called once per new capture. */
static void track(intvvid_tab *tab)
{
    const intvvid_capture *cap = tab->cap;
    int row_changes[INTVVID_CARD_ROWS] = { 0 };

    for (int idx = 0; idx < INTVVID_CARDS; idx++) {
        if (tab->tracked && cap->btab[idx] != tab->prev_btab[idx]) {
            tab->heat[idx] = HEAT_FRAMES;
            row_changes[idx / INTVVID_CARD_COLS]++;
        } else if (tab->heat[idx] > 0) {
            tab->heat[idx]--;
        }
        tab->prev_btab[idx] = cap->btab[idx];
    }
    for (int mob = 0; mob < 8; mob++) {
        int moved = 0;
        for (int reg = 0; reg < 3; reg++) {
            if (tab->tracked && cap->raw[mob + reg * 8] != tab->prev_mobregs[mob * 3 + reg]) moved = 1;
            tab->prev_mobregs[mob * 3 + reg] = cap->raw[mob + reg * 8];
        }
        if (moved) tab->mob_heat[mob] = HEAT_FRAMES;
        else if (tab->mob_heat[mob] > 0) tab->mob_heat[mob]--;
    }
    memcpy(tab->line_changed, row_changes, sizeof tab->line_changed);
    tab->tracked = 1;
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static void rgba_to_pixels(const uint8_t *rgba, uint32_t *dst, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = 0xff000000u | (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
}

static int build_decoded(intvvid_tab *tab)
{
    const intvvid_capture *cap = tab->cap;
    intvstic_snapshot snap;
    uint8_t *rgba;

    intvstic_snapshot_from_capture(cap, &snap);
    switch (tab->source) {
    case INTVVID_SRC_MOBS: {
        const int wide = 4 * MOB_PITCH - (MOB_PITCH - MOB_TILE), high = 2 * MOB_PITCH - (MOB_PITCH - MOB_TILE);
        uint8_t tile[MOB_TILE * MOB_TILE * 4];
        if (!ensure(&tab->base, &tab->base_cap, (size_t)wide * high)) return 0;
        for (int pix = 0; pix < wide * high; pix++) tab->base[pix] = COL_PANEL;
        for (int mob = 0; mob < 8; mob++) {
            const int ox = (mob % 4) * MOB_PITCH, oy = (mob / 4) * MOB_PITCH;
            mob_geom geom;
            mob_geometry(cap, &snap, mob, &geom);
            intvstic_render_mob(&snap, mob, tile);
            for (int py = 0; py < MOB_TILE; py++)
                for (int px = 0; px < MOB_TILE; px++) {
                    const uint8_t *src = &tile[(py * MOB_TILE + px) * 4];
                    uint32_t col = COL_PANEL;
                    if (src[3])
                        col = blend(COL_PANEL, 0xff000000u | (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2],
                                    geom.mob.visible ? 256 : 90);
                    tab->base[(size_t)(oy + py) * (size_t)wide + (size_t)(ox + px)] = col;
                }
        }
        tab->basew = wide; tab->baseh = high;
        tab->kind = BK_MOBS;
        return 1;
    }
    case INTVVID_SRC_SHEET: {
        const int cards = intvstic_card_count(&snap), rows = (cards + SHEET_COLS - 1) / SHEET_COLS;
        rgba = malloc((size_t)SHEET_COLS * 8 * (size_t)rows * 8 * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)SHEET_COLS * 8 * (size_t)rows * 8)) { free(rgba); return 0; }
        intvstic_render_cards(&snap, 0, rows, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)SHEET_COLS * 8 * (size_t)rows * 8);
        free(rgba);
        tab->basew = SHEET_COLS * 8; tab->baseh = rows * 8;
        tab->kind = BK_SHEET;
        return 1;
    }
    case INTVVID_SRC_PALETTE:
        if (!ensure(&tab->base, &tab->base_cap, 64 * 64)) return 0;
        for (int py = 0; py < 64; py++)
            for (int px = 0; px < 64; px++)
                tab->base[py * 64 + px] = pal_rgb(cap, (py / 16) * 4 + px / 16, 0);
        tab->basew = 64; tab->baseh = 64;
        tab->kind = BK_PALETTE;
        return 1;
    default:
        rgba = malloc((size_t)BT_PIX_W * BT_PIX_H * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)BT_PIX_W * BT_PIX_H)) { free(rgba); return 0; }
        intvstic_render_backtab(&snap, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)BT_PIX_W * BT_PIX_H);
        free(rgba);
        tab->basew = BT_PIX_W; tab->baseh = BT_PIX_H;
        tab->kind = BK_CARDS;
        return 1;
    }
}

static int build_base(intvvid_tab *tab)
{
    const intvvid_capture *cap = tab->cap;

    if (!tab->has_fb && !tab->has_live) {
        why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
        return 0;
    }
    switch (tab->view) {
    case INTVVID_V_DISPLAY:
    case INTVVID_V_BEAM: {
        const int shift = cap->vid_enable ? 0 : 1;      /* a blanked frame is shown at half brightness, as published */
        int below = FRAME_H;
        if (tab->view == INTVVID_V_BEAM) {
            if (!tab->has_live) {
                why_set(tab, "The Beam view needs the machine stopped: it shows the published frame with the lines the beam has not reached yet dimmed.");
                return 0;
            }
            below = beam_row(cap) + 1;
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)FRAME_W * FRAME_H)) return 0;
        for (int posy = 0; posy < FRAME_H; posy++)
            for (int posx = 0; posx < FRAME_W; posx++) {
                const uint32_t col = pal_rgb(cap, cap->vid[posy * FRAME_W + posx], shift);
                tab->base[(size_t)posy * FRAME_W + (size_t)posx] = posy < below ? col : blend(col, 0x000000, 150);
            }
        tab->basew = FRAME_W; tab->baseh = FRAME_H;
        tab->kind = BK_FRAME;
        return 1;
    }
    case INTVVID_V_DECODED:
        return build_decoded(tab);
    default:
        if (!ensure(&tab->base, &tab->base_cap, (size_t)BT_PIX_W * BT_PIX_H)) return 0;
        for (int cell = 0; cell < INTVVID_CARDS; cell++) {
            const uint32_t col = value_colour(cap->btab_sr[cell]);
            const int ox = (cell % INTVVID_CARD_COLS) * 8, oy = (cell / INTVVID_CARD_COLS) * 8;
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++)
                    tab->base[(size_t)(oy + py) * BT_PIX_W + (size_t)(ox + px)] = col;
        }
        tab->basew = BT_PIX_W; tab->baseh = BT_PIX_H;
        tab->kind = BK_VALUES;
        return 1;
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}
static void frame_rect(canvas *cnv, int posx, int posy, int wide, int high, int thick, uint32_t rgb, int alpha)
{
    for (int edge = 0; edge < thick; edge++) {
        hline(cnv, posy + edge, posx, posx + wide, rgb, alpha);
        hline(cnv, posy + high - 1 - edge, posx, posx + wide, rgb, alpha);
        vline(cnv, posx + edge, posy, posy + high, rgb, alpha);
        vline(cnv, posx + wide - 1 - edge, posy, posy + high, rgb, alpha);
    }
}

static void draw_overlays(intvvid_tab *tab, canvas *cnv, int scale)
{
    const intvvid_capture *cap = tab->cap;
    const int framed = tab->kind == BK_FRAME;
    const int carded = tab->kind == BK_CARDS || tab->kind == BK_VALUES;
    const int cardh = framed ? INTVVID_CARD_LINES : 8;
    const int orgx = framed ? bt_x0(cap) : 0, orgy = framed ? bt_y0(cap) : 0;

    if (!framed && !carded)
        return;
    if (framed && (tab->ovl & INTVVID_OV_BORDER)) {
        const uint32_t tint = 0xff6040c0;
        fill_rect(cnv, 0, 0, FRAME_W * scale, orgy * scale, tint, 70);
        fill_rect(cnv, 0, (orgy + BT_PIX_H * 2) * scale, FRAME_W * scale, (FRAME_H - orgy - BT_PIX_H * 2) * scale, tint, 70);
        fill_rect(cnv, 0, orgy * scale, orgx * scale, BT_PIX_H * 2 * scale, tint, 70);
        fill_rect(cnv, (orgx + BT_PIX_W) * scale, orgy * scale, (FRAME_W - orgx - BT_PIX_W) * scale, BT_PIX_H * 2 * scale, tint, 70);
        if (cap->raw[0x32] & 1)
            fill_rect(cnv, orgx * scale, orgy * scale, 8 * scale, BT_PIX_H * 2 * scale, 0xffd0a020, 90);
        if (cap->raw[0x32] & 2)
            fill_rect(cnv, orgx * scale, orgy * scale, BT_PIX_W * scale, 16 * scale, 0xffd0a020, 90);
    }
    if (tab->ovl & INTVVID_OV_HEAT)
        for (int cell = 0; cell < INTVVID_CARDS; cell++)
            if (tab->heat[cell] > 0)
                fill_rect(cnv, (orgx + (cell % INTVVID_CARD_COLS) * 8) * scale, (orgy + (cell / INTVVID_CARD_COLS) * cardh) * scale,
                          8 * scale, cardh * scale, 0xffff8020, 60 + tab->heat[cell] * 6);
    if ((tab->ovl & INTVVID_OV_GRID) && scale >= 2) {
        for (int col = 0; col <= INTVVID_CARD_COLS; col++)
            vline(cnv, (orgx + col * 8) * scale - (col == INTVVID_CARD_COLS), orgy * scale, (orgy + INTVVID_CARD_ROWS * cardh) * scale, 0xffffffff, 60);
        for (int row = 0; row <= INTVVID_CARD_ROWS; row++)
            hline(cnv, (orgy + row * cardh) * scale - (row == INTVVID_CARD_ROWS), orgx * scale, (orgx + BT_PIX_W) * scale, 0xffffffff, 60);
    }
    if (framed && (tab->ovl & (INTVVID_OV_MOBS | INTVVID_OV_HEAT))) {
        intvstic_snapshot snap;
        intvstic_snapshot_from_capture(cap, &snap);
        for (int mob = 0; mob < 8; mob++) {
            mob_geom geom;
            mob_geometry(cap, &snap, mob, &geom);
            if (!geom.live) continue;
            if (tab->ovl & INTVVID_OV_MOBS)
                frame_rect(cnv, geom.box_x * scale, geom.box_y * scale, geom.box_w * scale, geom.box_h * scale, scale < 3 ? 1 : 2,
                           mob_colour(mob), geom.mob.visible ? 230 : 110);
            if ((tab->ovl & INTVVID_OV_HEAT) && tab->mob_heat[mob] > 0)
                frame_rect(cnv, geom.box_x * scale - 2, geom.box_y * scale - 2, geom.box_w * scale + 4, geom.box_h * scale + 4, 2,
                           0xffff8020, 120 + tab->mob_heat[mob] * 5);
        }
    }
    if ((tab->ovl & INTVVID_OV_BEAM) && tab->has_live && framed) {
        const int posy = beam_row(cap);
        if (posy >= 0 && posy < FRAME_H) hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const intvvid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (tab->basew * k_zooms[i] <= tab->vieww && tab->baseh * k_zooms[i] <= tab->viewh)
                scale = k_zooms[i];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < scale) lower = k_zooms[i];
        scale = lower;
    }
    return scale;
}

static void build_picture(intvvid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void add_break(intvvid_pixel *outp, unsigned lo, unsigned hi)
{
    if (outp->nbreak < 3) {
        outp->break_lo[outp->nbreak] = lo;
        outp->break_hi[outp->nbreak] = hi;
        outp->nbreak++;
    }
}

/* the text for a BACKTAB cell, appended to the pixel's text */
static int cell_text(const intvvid_capture *cap, int cell, char *dst, size_t size)
{
    cell_info info;
    int num = 0;
    const unsigned word = cap->btab[cell], ram = cap->btab_sr[cell];

    cell_decode(cap, cell, &info);
    num += snprintf(dst + num, size - (size_t)num, "BACKTAB cell %d (column %d, row %d), word $%04X at $%04X",
                    cell, cell % INTVVID_CARD_COLS, cell / INTVVID_CARD_COLS, word, 0x200u + (unsigned)cell);
    if (ram != word)
        num += snprintf(dst + num, size - (size_t)num, " (RAM now holds $%04X: the STIC has not fetched it yet)", ram);
    num += snprintf(dst + num, size - (size_t)num, "\n");
    if (info.squares) {
        num += snprintf(dst + num, size - (size_t)num, "Coloured squares: %s, %s / %s, %s",
                        k_colour_names[info.quad[0] & 15], k_colour_names[info.quad[1] & 15],
                        k_colour_names[info.quad[2] & 15], k_colour_names[info.quad[3] & 15]);
    } else {
        int grom;
        const unsigned addr = card_addr(info.card, &grom);
        num += snprintf(dst + num, size - (size_t)num, "%s card %d at $%04X, foreground %s (%d), background %s (%d)%s",
                        grom ? "GROM" : "GRAM", grom ? info.card : info.card - 256, addr,
                        k_colour_names[info.fg & 15], info.fg, k_colour_names[info.bg & 15], info.bg,
                        info.advance ? ", advances the colour stack" : "");
    }
    return num;
}

static void locate(intvvid_tab *tab, int posx, int posy, intvvid_pixel *outp)
{
    const intvvid_capture *cap = tab->cap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    int picx, picy, num;
    intvstic_snapshot snap;

    memset(outp, 0, sizeof *outp);
    outp->frame_x = outp->frame_y = outp->colour = outp->cell = outp->card = outp->mob = -1;
    if (!tab->picw || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;
    intvstic_snapshot_from_capture(cap, &snap);

    num = snprintf(outp->text, sizeof outp->text, "Pixel %d,%d\n", picx, picy);
    switch (tab->kind) {
    case BK_FRAME: {
        const int relx = picx - bt_x0(cap), rely = picy - bt_y0(cap);
        outp->kind = INTVVID_PK_SCREEN;
        outp->frame_x = picx; outp->frame_y = picy;
        outp->colour = cap->vid[picy * FRAME_W + picx] & 15;
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "Colour %s (%d)\n",
                        k_colour_names[outp->colour], outp->colour);
        if (relx >= 0 && relx < BT_PIX_W && rely >= 0 && rely < BT_PIX_H * 2) {
            outp->cell = (rely / INTVVID_CARD_LINES) * INTVVID_CARD_COLS + relx / 8;
            outp->word_addr = 0x200u + (unsigned)outp->cell;
            outp->word = cap->btab[outp->cell];
            outp->markx = bt_x0(cap) + (relx / 8) * 8; outp->marky = bt_y0(cap) + (rely / INTVVID_CARD_LINES) * INTVVID_CARD_LINES;
            outp->markw = 8; outp->markh = INTVVID_CARD_LINES;
            {
                cell_info info;
                cell_decode(cap, outp->cell, &info);
                outp->card = info.card;
            }
            add_break(outp, outp->word_addr, outp->word_addr);
            num += cell_text(cap, outp->cell, outp->text + num, sizeof outp->text - (size_t)num);
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\n");
        } else {
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "Border or blanking: no BACKTAB card here\n");
        }
        for (int mob = 0; mob < 8; mob++) {
            mob_geom geom;
            mob_geometry(cap, &snap, mob, &geom);
            if (!geom.live || picx < geom.box_x || picx >= geom.box_x + geom.box_w ||
                picy < geom.box_y || picy >= geom.box_y + geom.box_h) continue;
            outp->mob_mask |= 1 << mob;
            if (outp->mob < 0) outp->mob = mob;
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                            "MOB %d box: card %d (%s), colour %s, %s, X $%04X Y $%04X A $%04X\n", mob, geom.mob.card >> 3,
                            geom.mob.gram ? "GRAM" : "GROM", k_colour_names[geom.mob.color & 15],
                            geom.mob.visible ? "visible" : "hidden", cap->raw[mob], cap->raw[mob + 8], cap->raw[mob + 16]);
        }
        if (outp->mob >= 0) {
            outp->nbreak = 0;
            add_break(outp, (unsigned)outp->mob, (unsigned)outp->mob);
            add_break(outp, 8u + (unsigned)outp->mob, 8u + (unsigned)outp->mob);
            add_break(outp, 16u + (unsigned)outp->mob, 16u + (unsigned)outp->mob);
        }
        break;
    }
    case BK_CARDS:
    case BK_VALUES: {
        outp->kind = tab->kind == BK_CARDS ? INTVVID_PK_CARD : INTVVID_PK_VALUE;
        outp->cell = (picy / 8) * INTVVID_CARD_COLS + picx / 8;
        outp->word_addr = 0x200u + (unsigned)outp->cell;
        outp->word = tab->kind == BK_VALUES ? cap->btab_sr[outp->cell] : cap->btab[outp->cell];
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        {
            cell_info info;
            cell_decode(cap, outp->cell, &info);
            outp->card = info.card;
        }
        add_break(outp, outp->word_addr, outp->word_addr);
        num += cell_text(cap, outp->cell, outp->text + num, sizeof outp->text - (size_t)num);
        break;
    }
    case BK_MOBS: {
        const int col = picx / MOB_PITCH, row = picy / MOB_PITCH, mob = row * 4 + col;
        if (picx % MOB_PITCH >= MOB_TILE || picy % MOB_PITCH >= MOB_TILE || col > 3 || mob > 7) {
            outp->kind = INTVVID_PK_NONE;
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "Between MOB tiles");
            break;
        }
        {
            mob_geom geom;
            mob_geometry(cap, &snap, mob, &geom);
            outp->kind = INTVVID_PK_MOB;
            outp->mob = mob;
            outp->mob_mask = 1 << mob;
            outp->markx = col * MOB_PITCH; outp->marky = row * MOB_PITCH; outp->markw = MOB_TILE; outp->markh = MOB_TILE;
            add_break(outp, (unsigned)mob, (unsigned)mob);
            add_break(outp, 8u + (unsigned)mob, 8u + (unsigned)mob);
            add_break(outp, 16u + (unsigned)mob, 16u + (unsigned)mob);
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                            "MOB %d: x %d y %d, %dx%d lines%s, card %d (%s), colour %s (%d), %s%s%s\n"
                            "X $%04X  Y $%04X  A $%04X  collisions $%04X",
                            mob, geom.mob.x, geom.mob.y, geom.box_w, geom.box_h, geom.mob.x_size == 2 ? " (double wide)" : "",
                            geom.mob.card >> 3, geom.mob.gram ? "GRAM" : "GROM", k_colour_names[geom.mob.color & 15], geom.mob.color,
                            geom.mob.visible ? "visible" : "hidden", geom.interact ? ", interacting" : "",
                            geom.mob.priority ? ", priority bit set" : "",
                            cap->raw[mob], cap->raw[mob + 8], cap->raw[mob + 16], cap->raw[mob + 24]);
        }
        break;
    }
    case BK_SHEET: {
        const int card = (picy / 8) * SHEET_COLS + picx / 8;
        int grom;
        unsigned addr;
        outp->kind = INTVVID_PK_SHEET;
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        if (card >= intvstic_card_count(&snap)) {
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "No card here (GRAM has %d cards)",
                            intvstic_card_count(&snap) - 256);
            break;
        }
        outp->card = card;
        addr = card_addr(card, &grom);
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "%s card %d at $%04X-$%04X\nBytes:",
                        grom ? "GROM" : "GRAM", grom ? card : card - 256, addr, addr + 7);
        for (int row = 0; row < 8; row++)
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, " %02X", cap->gmem[card * 8 + row]);
        if (!grom) add_break(outp, addr, addr + 7);
        else num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\nGROM is read-only: nothing writes it");
        break;
    }
    default: {
        const int idx = (picy / 16) * 4 + picx / 16;
        outp->kind = INTVVID_PK_PALETTE;
        outp->colour = idx;
        outp->markx = (picx / 16) * 16; outp->marky = (picy / 16) * 16; outp->markw = 16; outp->markh = 16;
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "Colour %d: %s, #%06X",
                        idx, k_colour_names[idx], pal_rgb(cap, idx, 0) & 0xffffff);
        break;
    }
    }
    (void)num;
}

int intvvid_tab_hover(intvvid_tab *tab, int posx, int posy)
{
    char old[sizeof tab->hover.text];
    if (!tab) return 0;
    snprintf(old, sizeof old, "%s", tab->hover.valid ? tab->hover.text : "");
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return strcmp(old, tab->hover.valid ? tab->hover.text : "") != 0;
}

void intvvid_tab_pin(intvvid_tab *tab, int posx, int posy)
{
    intvvid_pixel probe;
    if (!tab) return;
    locate(tab, posx, posy, &probe);
    if (!probe.valid) return;
    if (tab->pin.valid && probe.markx == tab->pin.markx && probe.marky == tab->pin.marky &&
        probe.markw == tab->pin.markw && probe.markh == tab->pin.markh) {
        intvvid_tab_unpin(tab);
        return;
    }
    tab->pin = probe;
    tab->pinx = posx; tab->piny = posy;
}

void intvvid_tab_unpin(intvvid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const intvvid_pixel *intvvid_tab_pixel(const intvvid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int intvvid_tab_marks(const intvvid_tab *tab, int hover[4], int pin[4])
{
    int ret = 0, scale;
    if (!tab) return 0;
    scale = tab->effzoom ? tab->effzoom : 1;
    if (tab->hover.valid) {
        hover[0] = tab->hover.markx * scale; hover[1] = tab->hover.marky * scale;
        hover[2] = tab->hover.markw * scale; hover[3] = tab->hover.markh * scale;
        ret |= 1;
    }
    if (tab->pin.valid) {
        pin[0] = tab->pin.markx * scale; pin[1] = tab->pin.marky * scale;
        pin[2] = tab->pin.markw * scale; pin[3] = tab->pin.markh * scale;
        ret |= 2;
    }
    return ret;
}

int intvvid_tab_can_break(const intvvid_tab *tab)
{
    const intvvid_pixel *pixp = intvvid_tab_pixel(tab);
    return tab && tab->dbg && pixp && pixp->valid && pixp->nbreak > 0;
}

/* A write breakpoint on lo..hi, unless one already covers it; its id or -1. */
static int write_break_range(intvdebug *dbg, unsigned lo, unsigned hi)
{
    intvdebug_bp list[128];
    intvdebug_bp spec;
    int count = intvdebug_bp_list(dbg, list, 128);
    for (int i = 0; i < count; i++)
        if ((list[i].type & INTVDEBUG_BP_WRITE) && list[i].lo <= lo && list[i].hi >= hi)
            return list[i].id;
    memset(&spec, 0, sizeof spec);
    spec.type = INTVDEBUG_BP_WRITE;
    spec.lo = (uint16_t)lo;
    spec.hi = (uint16_t)hi;
    spec.cond = INTVDEBUG_COND_ANY;
    return intvdebug_bp_add(dbg, &spec);
}

int intvvid_tab_break_on_pixel(intvvid_tab *tab)
{
    const intvvid_pixel *pixp = intvvid_tab_pixel(tab);
    int first = -1;

    if (!intvvid_tab_can_break(tab)) return -1;
    for (int i = 0; i < pixp->nbreak; i++) {
        const int made = write_break_range(tab->dbg, pixp->break_lo[i], pixp->break_hi[i]);
        if (first < 0) first = made;
    }
    return first;
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(intvvid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(intvvid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...)
{
    intvvid_reg *reg;
    va_list args;
    int prev_age;
    if (*num >= MAX_REGS) return;
    reg = &tab->regs[*num];
    prev_age = reg->age;
    reg->name = name;
    snprintf(reg->value, sizeof reg->value, "%s", value);
    va_start(args, fmt);
    vsnprintf(reg->meaning, sizeof reg->meaning, fmt, args);
    va_end(args);
    if (!tab->regs_primed || strcmp(tab->prev_val[*num], reg->value) != 0)
        reg->age = tab->regs_primed ? 0 : 99;
    else
        reg->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(tab->prev_val[*num], reg->value, sizeof tab->prev_val[*num]);
    (*num)++;
}

static void build_tables(intvvid_tab *tab)
{
    const intvvid_capture *cap = tab->cap;
    static const char *const k_mob_names[8] = { "MOB0", "MOB1", "MOB2", "MOB3", "MOB4", "MOB5", "MOB6", "MOB7" };
    intvstic_snapshot snap;
    int num = 0, visible = 0;
    char val[48];
    long uses[16] = { 0 };

    intvstic_snapshot_from_capture(cap, &snap);

    /* registers (ages carry over from the previous build) */
    for (int mob = 0; mob < 8; mob++) {
        mob_geom geom;
        mob_geometry(cap, &snap, mob, &geom);
        visible += geom.mob.visible && geom.live;
        snprintf(val, sizeof val, "%04X %04X %04X %04X", cap->raw[mob] & 0xFFFF, cap->raw[mob + 8] & 0xFFFF,
                 cap->raw[mob + 16] & 0xFFFF, cap->raw[mob + 24] & 0xFFFF);
        reg_set(tab, &num, k_mob_names[mob], val,
                "x %d y %d, %dx%d lines, card %d %s, fg %d%s%s%s%s%s  (X Y A, collisions)",
                geom.mob.x, geom.mob.y, geom.box_w, geom.box_h, geom.mob.card >> 3, geom.mob.gram ? "GRAM" : "GROM",
                geom.mob.color, geom.mob.visible ? "" : ", hidden", geom.interact ? ", interacting" : "",
                geom.mob.flip_x ? ", flip X" : "", geom.mob.flip_y ? ", flip Y" : "", geom.mob.priority ? ", priority" : "");
    }
    snprintf(val, sizeof val, "%X %X %X %X", cap->raw[0x28] & 0xF, cap->raw[0x29] & 0xF, cap->raw[0x2A] & 0xF, cap->raw[0x2B] & 0xF);
    reg_set(tab, &num, "COLSTK", val, "colour stack $28-$2B: %s, %s, %s, %s", k_colour_names[cap->raw[0x28] & 15],
            k_colour_names[cap->raw[0x29] & 15], k_colour_names[cap->raw[0x2A] & 15], k_colour_names[cap->raw[0x2B] & 15]);
    snprintf(val, sizeof val, "%X", cap->raw[0x2C] & 0xF);
    reg_set(tab, &num, "BORDER", val, "border colour $2C: %s", k_colour_names[cap->raw[0x2C] & 15]);
    snprintf(val, sizeof val, "H%u V%u", cap->raw[0x30] & 7, cap->raw[0x31] & 7);
    reg_set(tab, &num, "DELAY", val, "horizontal $30 and vertical $31 delay: the picture moves %d pixels right, %d lines down",
            hdly(cap), 2 * vdly(cap));
    snprintf(val, sizeof val, "%X", cap->raw[0x32] & 3);
    reg_set(tab, &num, "EDGE", val, "border extension $32: %s%s%s", (cap->raw[0x32] & 1) ? "left edge +8 pixels" : "",
            (cap->raw[0x32] & 3) == 3 ? ", " : "", (cap->raw[0x32] & 2) ? "top edge +8 rows" : (cap->raw[0x32] & 1) ? "" : "none");
    snprintf(val, sizeof val, cap->mode == cap->p_mode ? "%s" : "%s > %s", cap->mode ? "FGBG" : "CS", cap->p_mode ? "FGBG" : "CS");
    reg_set(tab, &num, "MODE", val, "%s%s", cap->mode ? "foreground/background" : "colour stack",
            cap->mode != cap->p_mode ? " (the other mode takes effect next frame)" : "");
    snprintf(val, sizeof val, "%s", cap->vid_enable ? "on" : "off");
    reg_set(tab, &num, "DISPLAY", val, "%s", cap->vid_enable ? "the program enabled the display this frame ($20 strobe)"
                                                              : "display blanked (no $20 strobe): the frame is shown dimmed");
    snprintf(val, sizeof val, "%d", intvstic_card_count(&snap) - 256);
    reg_set(tab, &num, "GRAM", val, "GRAM cards (gram size %d, mask $%03X)", cap->gram_size, cap->gram_mask);
    snprintf(val, sizeof val, "%d/%d", cap->fifo_wr_ptr, INTVVID_CARDS);
    reg_set(tab, &num, "FETCH", val, "BACKTAB words the STIC has fetched this frame (bus request %d)", cap->busrq_count);
    tab->nregs = num;
    tab->regs_primed = 1;

    /* colours: how much of the published frame each takes, and what names them */
    for (int pix = 0; pix < FRAME_W * FRAME_H; pix++) uses[cap->vid[pix] & 15]++;
    for (int idx = 0; idx < 16; idx++) {
        intvvid_swatch *swat = &tab->swat[idx];
        int len = 0;
        swat->index = idx;
        swat->name = k_colour_names[idx];
        swat->rgb = pal_rgb(cap, idx, 0);
        swat->uses = uses[idx];
        swat->role[0] = '\0';
        for (int stack = 0; stack < 4; stack++)
            if ((int)(cap->raw[0x28 + stack] & 0xF) == idx)
                len += snprintf(swat->role + len, sizeof swat->role - (size_t)len, "%sstack %d", len ? ", " : "", stack);
        if ((int)(cap->raw[0x2C] & 0xF) == idx)
            len += snprintf(swat->role + len, sizeof swat->role - (size_t)len, "%sborder", len ? ", " : "");
        for (int mob = 0; mob < 8 && len < (int)sizeof swat->role - 8; mob++) {
            mob_geom geom;
            mob_geometry(cap, &snap, mob, &geom);
            if (geom.live && geom.mob.visible && geom.mob.color == idx)
                len += snprintf(swat->role + len, sizeof swat->role - (size_t)len, "%sMOB%d", len ? ", " : "", mob);
        }
    }

    /* lines: the 12 card rows */
    {
        int stack = 0, background = (int)(cap->raw[0x28] & 0xF);
        for (int row = 0; row < INTVVID_CARD_ROWS; row++) {
            intvvid_line_row *out = &tab->rows[row];
            memset(out, 0, sizeof *out);
            out->row = row;
            out->line = bt_y0(cap) + row * INTVVID_CARD_LINES;
            out->addr = 0x200u + (unsigned)row * INTVVID_CARD_COLS;
            out->background = cap->mode == 0 ? background : -1;
            out->fetched = cap->live ? cap->fifo_wr_ptr >= (row + 1) * INTVVID_CARD_COLS : 1;
            out->changed = tab->line_changed[row];
            for (int col = 0; col < INTVVID_CARD_COLS; col++) {
                const unsigned word = cap->btab[row * INTVVID_CARD_COLS + col];
                out->word[col] = (uint16_t)word;
                out->used += word != 0;
                if (cap->mode == 0 && (word & 0x1800) != 0x1000 && (word & 0x2000)) {
                    stack = (stack + 1) & 3;
                    background = (int)(cap->raw[0x28 + stack] & 0xF);
                }
            }
        }
    }

    /* timing */
    {
        const int lines = frame_lines(cap);
        const double rate = cap->pal ? 1000000.0 : 894886.25;
        tab->ntiming = 0;
#define TLINE(...) do { if (tab->ntiming < MAX_TIMING) { \
        snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], __VA_ARGS__); \
        tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming]; tab->ntiming++; } } while (0)
        TLINE("Standard\t%s", cap->pal ? "PAL" : "NTSC");
        TLINE("Frame\t%d clocks, %d lines of %d clocks, %.2f Hz", frame_clocks(cap), lines, scan_clocks(cap),
              rate / (double)frame_clocks(cap));
        TLINE("Captured\tframe %llu", (unsigned long long)cap->frame);
        if (cap->live && cap->cycle >= cap->last_intrq)
            TLINE("Beam\tline %d of %d (frame row %d), clock %llu into the frame", beam_scan(cap), lines, beam_row(cap),
                  (unsigned long long)(cap->cycle - cap->last_intrq));
        else
            TLINE("Beam\tonly when the machine is stopped");
        TLINE("INTRQ\tevery frame, held %d clocks", cap->pal ? 6400 : 2907);
        TLINE("Register window\tSTIC writes are taken for %d clocks after the INTRQ, then dropped", cap->pal ? 6400 - 7 : 2900);
        TLINE("GRAM window\tcard writes are taken for %d clocks", cap->pal ? 7456 : 3796);
        TLINE("BACKTAB fetch\t12 bus requests, one every 16 lines; %d of %d words fetched", cap->fifo_wr_ptr, INTVVID_CARDS);
        TLINE("Frame top\tline %d of the raster (estimated from the fetch schedule)", top_scanline(cap));
        TLINE("MOBs on screen\t%d", visible);
        TLINE("Machine\t%s", tab->stopped ? "stopped" : "running");
#undef TLINE
    }

    snprintf(tab->status, sizeof tab->status,
             "%s view, display %s, %s mode, %d MOB%s on screen\n%s",
             k_view_names[tab->view], cap->vid_enable ? "on" : "off", cap->mode ? "foreground/background" : "colour stack",
             visible, visible == 1 ? "" : "s",
             tab->stopped ? "Stopped" : "Running: showing the last published frame");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned intvvid_tab_refresh(intvvid_tab *tab, int force)
{
    uint64_t sig;
    unsigned ret = 0;
    int fresh;

    if (!tab) return 0;
    if (!acquire(tab)) {
        why_set(tab, "The machine is not running.");
        tab->picw = tab->pich = 0;
        return INTVVID_TAB_OFF | INTVVID_TAB_PICTURE | INTVVID_TAB_TABLES;
    }
    if (!tab->has_fb && !tab->has_live) {
        why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
        tab->picw = tab->pich = 0;
        return INTVVID_TAB_OFF | INTVVID_TAB_PICTURE | INTVVID_TAB_TABLES;
    }
    sig = signature(tab);
    fresh = force || sig != tab->sig;
    if (fresh) {
        if (sig != tab->sig) track(tab);
        tab->sig = sig;
    }
    if (fresh || tab->dirty) {
        build_picture(tab);
        tab->dirty = 0;
        ret |= INTVVID_TAB_PICTURE;
    }
    if (fresh) {
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        build_tables(tab);
        ret |= INTVVID_TAB_TABLES;
    }
    return ret;
}

const uint32_t *intvvid_tab_picture(intvvid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}
const char *intvvid_tab_why(const intvvid_tab *tab) { return tab ? tab->why : ""; }
int intvvid_tab_effective_zoom(const intvvid_tab *tab) { return tab ? tab->effzoom : 1; }
const char *intvvid_tab_status(const intvvid_tab *tab) { return tab ? tab->status : ""; }
int intvvid_tab_regs(const intvvid_tab *tab, const intvvid_reg **out)
{
    if (out) *out = tab->regs;
    return tab->nregs;
}
int intvvid_tab_swatches(const intvvid_tab *tab, const intvvid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const intvvid_pixel *pixp = intvvid_tab_pixel(tab);
    intvvid_swatch *swat = ((intvvid_tab *)tab)->swat;
    for (int i = 0; i < 16; i++)
        swat[i].inspected = pixp && pixp->valid && pixp->colour == i;
    if (out) *out = tab->swat;
    return 16;
}
int intvvid_tab_lines(const intvvid_tab *tab, const intvvid_line_row **out)
{
    if (out) *out = tab->rows;
    return tab->have ? INTVVID_CARD_ROWS : 0;
}
int intvvid_tab_timing(const intvvid_tab *tab, const char *const **lines)
{
    if (lines) *lines = (const char *const *)tab->timing_ptr;
    return tab->ntiming;
}
int intvvid_tab_beam_permille(const intvvid_tab *tab)
{
    if (!tab || !tab->have || !tab->cap->live) return 0;
    return beam_scan(tab->cap) * 1000 / frame_lines(tab->cap);
}

int intvvid_tab_text(intvvid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
#define PUT(...) do { if (num < dstsz) num += snprintf(dst + num, (size_t)(dstsz - num), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!tab || !tab->have) { PUT("The machine is not running.\n"); return num < dstsz ? num : dstsz - 1; }
    switch (section) {
    case INTVVID_S_REGISTERS:
        for (int i = 0; i < tab->nregs; i++)
            PUT("%-7s %-19s %s\n", tab->regs[i].name, tab->regs[i].value, tab->regs[i].meaning);
        break;
    case INTVVID_S_COLOURS:
        for (int i = 0; i < 16; i++)
            PUT("%2d %-13s #%06X  %6ld pixels  %s\n", i, tab->swat[i].name, tab->swat[i].rgb & 0xffffff, tab->swat[i].uses,
                tab->swat[i].role);
        break;
    case INTVVID_S_LINES:
        for (int i = 0; i < INTVVID_CARD_ROWS; i++) {
            const intvvid_line_row *ret = &tab->rows[i];
            PUT("row %2d line %3d $%04X %2d used %2d changed", ret->row, ret->line, ret->addr, ret->used, ret->changed);
            if (ret->background >= 0) PUT("  bg %d", ret->background);
            if (!ret->fetched) PUT("  not fetched yet");
            PUT("\n    ");
            for (int col = 0; col < INTVVID_CARD_COLS; col++) PUT("%04X ", ret->word[col]);
            PUT("\n");
        }
        break;
    case INTVVID_S_TIMING:
        for (int i = 0; i < tab->ntiming; i++) PUT("%s\n", tab->timing_buf[i]);
        break;
    default:
        PUT("%s\n", tab->status);
        if (intvvid_tab_pixel(tab)) PUT("%s\n", intvvid_tab_pixel(tab)->text);
        break;
    }
#undef PUT
    return num < dstsz ? num : dstsz - 1;
}

int intvvid_tab_text_for(intvdebug *dbg, const char *section, char *dst, int dstsz)
{
    intvvid_tab *tab = intvvid_tab_new(dbg);
    int sec = section && *section ? intvvid_section_from_name(section) : INTVVID_S_REGISTERS, num;

    if (!dst || dstsz <= 0) { intvvid_tab_free(tab); return 0; }
    dst[0] = '\0';
    if (!tab) return 0;
    if (section && *section && sec < 0) {
        num = snprintf(dst, (size_t)dstsz, "video: no section '%s' (pixel registers colours lines timing)\n", section);
        intvvid_tab_free(tab);
        return num < dstsz ? num : dstsz - 1;
    }
    intvvid_tab_refresh(tab, 1);
    if (!tab->have || (!tab->has_fb && !tab->has_live)) {
        num = snprintf(dst, (size_t)dstsz, "%s\n", tab->why[0] ? tab->why : "The machine is not running.");
        intvvid_tab_free(tab);
        return num < dstsz ? num : dstsz - 1;
    }
    num = 0;
    if (!section || !*section)
        num = intvvid_tab_text(tab, INTVVID_S_PIXEL, dst, dstsz);
    if (num < dstsz - 1)
        num += intvvid_tab_text(tab, sec, dst + num, dstsz - num);
    intvvid_tab_free(tab);
    return num < dstsz ? num : dstsz - 1;
}
