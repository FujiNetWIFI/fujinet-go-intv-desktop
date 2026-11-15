/*
 * intvvidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and five tables. Modelled on the Studio II
 * (studio2vidtab.h) and CoCo controllers, so a frontend that has one ports
 * the other by changing names:
 *
 *   - the machine is only COPIED: a finished frame (intvvidcap.h) is captured
 *     once per frame on the emulator thread, and a parked machine is read
 *     live. Decoding, scaling and overlays run on the caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from intvvid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The STIC draws a whole frame at once from its registers, the BACKTAB it
 * fetched and GRAM; register writes outside the vertical blank are dropped. So
 * one capture a frame says everything the frame was made from. What moves
 * during a frame is the BACKTAB fetch (a bus request every 16 scan lines):
 * the Beam view and the Lines table show how far a parked machine got.
 *
 * Views: Display is the frame the machine published (160 x 200, the border
 * included); Decoded redraws one source from the registers: the BACKTAB cards
 * (160 x 96, no MOBs), the 8 MOBs, the GROM/GRAM card sheet or the palette;
 * Beam is the Display with the lines the beam has not reached yet dimmed
 * (stopped only); Values shows each BACKTAB word as a flat false colour of its
 * value, from system RAM.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTVVIDTAB_H
#define INTVVIDTAB_H

#include <stdint.h>

#include "intvdebug.h"
#include "intvvidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct intvvid_tab intvvid_tab;

enum intvvid_view {
    INTVVID_V_DISPLAY = 0,      /* the published frame, 160 x 200 */
    INTVVID_V_DECODED,          /* one source rebuilt from the registers (see intvvid_source) */
    INTVVID_V_BEAM,             /* the Display, the lines below the beam dimmed (stopped) */
    INTVVID_V_VALUES,           /* each BACKTAB word as a flat false colour (160 x 96) */
    INTVVID_V_COUNT
};

/* What the Decoded view redraws. */
enum intvvid_source {
    INTVVID_SRC_CARDS = 0,      /* the BACKTAB in its mode, no MOBs: 160 x 96 */
    INTVVID_SRC_MOBS,           /* the 8 MOBs at native size, 4 x 2 tiles */
    INTVVID_SRC_SHEET,          /* every GROM and GRAM card, 16 a row */
    INTVVID_SRC_PALETTE,        /* the 16 colours, 4 x 4 */
    INTVVID_SRC_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    INTVVID_OV_GRID   = 1u << 0,    /* the card grid (Display, Cards, Values, Beam) */
    INTVVID_OV_MOBS   = 1u << 1,    /* each MOB's box (Display, Beam) */
    INTVVID_OV_BORDER = 1u << 2,    /* the border and blanking, tinted (Display, Beam) */
    INTVVID_OV_BEAM   = 1u << 3,    /* where the beam is (stopped) */
    INTVVID_OV_HEAT   = 1u << 4,    /* BACKTAB words and MOBs that changed lately */
    INTVVID_OV_ALL    = 0x1Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, mobs, ... */
    const char *label;          /* the checkbox text */
} intvvid_overlay_info;
const intvvid_overlay_info *intvvid_overlays(int *count);
const char *intvvid_view_name(int view);            /* "Display", ... */
const char *intvvid_view_key(int view);             /* "display", ... (env) */
const char *intvvid_source_name(int source);        /* "Cards", ... */
const char *intvvid_source_key(int source);         /* "cards", ... */

/* Right-hand sections. */
enum { INTVVID_S_PIXEL = 0, INTVVID_S_REGISTERS, INTVVID_S_COLOURS,
       INTVVID_S_LINES, INTVVID_S_TIMING, INTVVID_S_COUNT };
const char *intvvid_section_name(int section);
const char *intvvid_section_key(int section);

/* dbg is the debugger the tab asks whether the machine is parked and adds
 * write breakpoints to; NULL (tests) makes every Break on... return -1 and the
 * machine count as running. The tab asks the host to capture frames until it
 * is freed. */
intvvid_tab *intvvid_tab_new(intvdebug *dbg);
void         intvvid_tab_free(intvvid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     intvvid_tab_set_view(intvvid_tab *tab, int view);
int      intvvid_tab_view(const intvvid_tab *tab);
void     intvvid_tab_set_source(intvvid_tab *tab, int source);
int      intvvid_tab_source(const intvvid_tab *tab);
void     intvvid_tab_set_overlays(intvvid_tab *tab, unsigned ovl);
unsigned intvvid_tab_overlays(const intvvid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of
 * those that fits both ways) */
void     intvvid_tab_set_zoom(intvvid_tab *tab, int zoom);
int      intvvid_tab_zoom(const intvvid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     intvvid_tab_set_viewport(intvvid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *intvvid_zooms(int *count);

/*
 * The scripted-capture variables, read once by intvvid_tab_new:
 *   INTV_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   INTV_DEBUGGER_VIDEO_SOURCE    cards mobs sheet palette   (the Decoded view's source)
 *   INTV_DEBUGGER_VIDEO_OVERLAYS  grid,mobs,border,beam,heat | all | none
 *   INTV_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   INTV_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, INTVVID_S_PIXEL when unset.
 */
int intvvid_tab_env_section(const intvvid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define INTVVID_TAB_PICTURE 1u  /* repaint the picture (intvvid_tab_picture) */
#define INTVVID_TAB_TABLES  2u  /* refill the status line and the sections */
#define INTVVID_TAB_OFF     4u  /* no machine: show intvvid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned intvvid_tab_refresh(intvvid_tab *tab, int force);

/* For tests: use this capture instead of the machine. stopped: the machine
 * counts as parked, so the Beam view works (cap->live says the capture is a
 * parked machine's, with cap->cycle the beam). */
void intvvid_tab_feed(intvvid_tab *tab, const intvvid_capture *cap, int stopped);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: intvvid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *intvvid_tab_picture(intvvid_tab *tab, int *wide, int *high);
const char     *intvvid_tab_why(const intvvid_tab *tab);
int             intvvid_tab_effective_zoom(const intvvid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *intvvid_tab_status(const intvvid_tab *tab);        /* two lines, '\n' between */

/* STIC registers, grouped: MOB 0-7 (X, Y, attribute, collision), the colour
 * stack, border, delays, edge extension, mode and display state. */
typedef struct {
    const char *name;           /* "MOB0", "COLSTK", ... */
    char value[32];             /* "0642 0390 2005 0000" */
    char meaning[160];
    int age;                    /* refreshes since it last changed (INTVVID_REG_FADE+: not recent) */
} intvvid_reg;
#define INTVVID_REG_FADE 12
int intvvid_tab_regs(const intvvid_tab *tab, const intvvid_reg **out);

/* The 16 colours. */
typedef struct {
    int index;                  /* 0..15 */
    const char *name;           /* "Black (0)" */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the published frame with this colour */
    char role[48];              /* "colour stack 0, border", "" when unused by the registers */
    int inspected;              /* the inspected pixel has it */
} intvvid_swatch;
int intvvid_tab_swatches(const intvvid_tab *tab, const intvvid_swatch **out);

/* The Lines table: one entry for each of the 12 card rows. */
typedef struct {
    int row;                    /* card row 0..11 */
    int line;                   /* first frame line of the row (the 160 x 200 frame) */
    unsigned addr;              /* system RAM address of its first word, $0200 + 20 * row */
    int used;                   /* words that are not 0 */
    int changed;                /* words that differ from the previous capture */
    int background;             /* colour stack background at the row's start (colour stack mode), else -1 */
    int fetched;                /* the STIC has fetched the row this frame (parked machine), else 1 */
    uint16_t word[INTVVID_CARD_COLS];
} intvvid_line_row;
int intvvid_tab_lines(const intvvid_tab *tab, const intvvid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  intvvid_tab_timing(const intvvid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  intvvid_tab_beam_permille(const intvvid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  intvvid_tab_text(intvvid_tab *tab, int section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  intvvid_section_from_name(const char *name);
/* The console's `video`: attaches to the machine for the call, no tab needed.
 * section NULL or "" is the status and the registers. Returns the length. */
int  intvvid_tab_text_for(intvdebug *dbg, const char *section, char *dst, int dstsz);

/* ---- hover, pin ---------------------------------------------------------------------- */
enum { INTVVID_PK_NONE = 0, INTVVID_PK_SCREEN, INTVVID_PK_CARD, INTVVID_PK_MOB,
       INTVVID_PK_SHEET, INTVVID_PK_PALETTE, INTVVID_PK_VALUE };

typedef struct {
    int valid;
    int picx, picy;             /* picture pixel (base picture, before the zoom) */
    int kind;                   /* INTVVID_PK_* */
    int markx, marky, markw, markh;     /* the object under the pointer, in picture pixels */
    int frame_x, frame_y;       /* the 160 x 200 frame pixel, -1 when the view has none */
    int colour;                 /* palette index of the pixel, -1 if none */
    int cell;                   /* BACKTAB cell 0..239 under it, -1 */
    int card;                   /* the card it shows, -1 */
    unsigned word_addr;         /* where its BACKTAB word lives ($0200 + cell), 0 if none */
    uint16_t word;
    int mob;                    /* the front MOB whose box holds it, -1 */
    int mob_mask;               /* every MOB box that holds it, bit per MOB */
    int nbreak;                 /* write breakpoint targets (up to 3 ranges) */
    unsigned break_lo[3], break_hi[3];
    uint32_t rgb;               /* 0xFFRRGGBB */
    char text[640];             /* ready-made inspector text */
} intvvid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  intvvid_tab_hover(intvvid_tab *tab, int posx, int posy);
void intvvid_tab_pin(intvvid_tab *tab, int posx, int posy);      /* a click; the same cell unpins */
void intvvid_tab_unpin(intvvid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const intvvid_pixel *intvvid_tab_pixel(const intvvid_tab *tab);
/* rectangles x,y,w,h in picture pixels (zoom applied) for the marks drawn over
 * the picture; bit0 of the result: hover valid, bit1: pin valid */
int  intvvid_tab_marks(const intvvid_tab *tab, int hover[4], int pin[4]);
/* "Break on writes": write breakpoints on what the inspected pixel is made
 * from: a MOB's X, Y and attribute registers ($0000-$0017), a BACKTAB word
 * ($0200 + cell) or a GRAM card's 8 bytes ($3800 +). The BACKTAB word is the
 * one the CPU writes; the STIC drops writes outside the vertical blank but the
 * watch fires on the CPU's store either way. -1 for GROM, the border,
 * a palette swatch, or without a debugger. Not added twice. The first
 * breakpoint's id, or -1. */
int  intvvid_tab_break_on_pixel(intvvid_tab *tab);
/* 1 when "Break on writes" has something to break on now */
int  intvvid_tab_can_break(const intvvid_tab *tab);

#ifdef __cplusplus
}
#endif

#endif
