/*
 * intvstic -- the STIC (Standard Television Interface Chip) debugger view:
 * register/mode decode, the 8 MOBs (Movable OBjects, i.e. sprites), and
 * GRAM/GROM card rendering.
 *
 * All register offsets and bit layouts below are read directly out of the
 * staged tree's src/stic/stic.c -- not from any datasheet -- by finding
 * where each field is actually consumed (stic_do_mob for the MOB registers,
 * stic_draw_mobs for position/priority/visibility, stic_ctrl_wr_internal
 * for the $20/$21 mode strobes, and the various stic->raw[0x28..0x32] reads
 * for color stack/border/delay/enable). Cross-check against that file if
 * anything here looks wrong.
 *
 * SCOPE, stated plainly rather than silently narrowed (same practice
 * fujinet-go-msx-desktop's own msxvdp.h documents): this covers registers,
 * mode, the 8 MOBs (decode and render), a GRAM/GROM card sheet, the
 * palette, and the BACKTAB (the 20x12-card background grid) in both its
 * modes. The BACKTAB decode is the one piece re-derived rather than
 * shared with the MOB code: Color Stack and Foreground/Background mode
 * each have their own bit layout and color-cycling rule, taken directly
 * from stic_draw_cstk/stic_draw_fgbg -- including Color Stack mode's
 * "colored squares" special case (card bits 12-11 == 0b10), which packs
 * four 2x2-pixel quadrants into one card instead of an 8x8 bitmap. NOT
 * covered: the STIC's per-frame MOB/BACKTAB collision compositing
 * (mpl_pri/mpl_vsb) and border-color extension bits -- this renders the
 * BACKTAB as the STIC would with no MOBs in front of it, which is what a
 * debugger view of "what does the background actually contain" wants;
 * compositing MOBs on top is a frontend's blit-order concern, not this
 * module's.
 *
 * THREADING / lifetime: same rule as intvdebug.h -- only valid while the
 * machine is paused, since it reads the machine's own struct state (via
 * intv.stic) from a UI thread. Every function here takes the intvdebug*
 * handle for that reason, even though the STIC itself has no debugger
 * object of its own -- it is what enforces the precondition.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTVSTIC_H
#define INTVSTIC_H

#include <stdint.h>

#include "intvdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INTVSTIC_MOB_COUNT 8
#define INTVSTIC_GMEM_SIZE (0x200 * 8) /* GROM (0x000-0x7FF) + GRAM
                                        * (0x800-0xFFF at 64 cards, more
                                        * with a bigger gram_size) worth of
                                        * card bitmap bytes, 8 per card. */

/* A snapshot of everything below is copied out of intv.stic in one call
 * (intvstic_snapshot_get) so every describe/render call afterwards works
 * from a consistent, no-longer-moving copy -- the same reasoning
 * msxdebug_vdp_snapshot documents for the MSX port. */
typedef struct {
    uint32_t raw[0x40];               /* the STIC's own register file */
    uint8_t  gmem[INTVSTIC_GMEM_SIZE]; /* GROM+GRAM card bitmap bytes */
    uint16_t mob_bmp[INTVSTIC_MOB_COUNT][16]; /* decoded 1bpp MOB bitmaps,
                                               * 16 rows x 16 bits, already
                                               * flip/size-adjusted --
                                               * copied from stic->mob_bmp,
                                               * which stic_do_mob (run every
                                               * frame regardless of whether
                                               * anyone is debugging) keeps
                                               * current. */
    uint16_t btab[240];       /* the 20x12 BACKTAB, row-major (stic->btab --
                              * the STIC's own view of it, already
                              * synchronised from system RAM by the time a
                              * frame is rendered, per stic_tick) */
    int mode;      /* 0 = Color Stack, 1 = Foreground/Background
                    * (stic->mode -- the mode actually in effect; $20/$21
                    * writes go to stic->p_mode and take effect next
                    * frame, see stic_ctrl_wr_internal) */
    int gram_size;  /* 0 = 64 cards, 1 = 128, 2 = 256 (TutorVision) */
    uint16_t gram_mask; /* stic->gram_mask -- masks GRAM card indices to
                        * what gram_size actually provides; copied rather
                        * than recomputed so this can never drift from
                        * stic_init's own formula. */
} intvstic_snapshot;

void intvstic_snapshot_get(intvdebug *d, intvstic_snapshot *out);

/* ---- registers / mode --------------------------------------------------
 * One decoded text line per register/field. Returns the length written. */
int intvstic_describe_mode(const intvstic_snapshot *s, char *out, int max);
/* Color stack (raw[0x28-0x2B]), border color (raw[0x2C]), horizontal/
 * vertical delay (raw[0x30]/raw[0x31] & 7), and the BACKTAB dimension bits
 * (raw[0x32]). */
int intvstic_describe_registers(const intvstic_snapshot *s, char *out,
                                int max);
/* The whole decode as monospace text -- mode then registers. Returns the
 * length written. */
int intvstic_format_state(const intvstic_snapshot *s, char *out, int max);

/* ---- MOBs (Movable OBjects) ---------------------------------------------
 * index: 0-7. Decoded straight from stic_do_mob/stic_draw_mobs's own bit
 * extraction in the staged tree -- see this file's header. */
typedef struct {
    int x, y;             /* pixel position: x_reg & 0xFF, (y_reg & 0x7F)*2 */
    int card;              /* GRAM/GROM card index (gr_idx in stic_do_mob --
                            * already has the FG/BG-mode 64-card mask, the
                            * GRAM-size mask, and the double-height
                            * even-card mask applied) */
    int color;              /* 4-bit foreground color, 0-15 */
    int x_size;              /* 1 or 2 (double width, x_reg bit 10) */
    int y_stretch;           /* 1, 2, 4, or 8 ((y_reg >> 8) & 3 -> 1<<that) */
    int y_res;               /* 8 or 16 (y_reg bit 7) -- native card rows
                             * before y_stretch is applied */
    int flip_x, flip_y;       /* y_reg bits 10, 11 */
    int gram;                 /* 1 if card is from GRAM, 0 GROM (a_reg bit 11) */
    int priority;             /* 1 = in front of BACKTAB (a_reg bit 13) */
    int visible;               /* x_reg bit 9 */
    uint16_t collision;        /* raw[mob+0x18] -- bits 0-7 = collided with
                                * MOB 0-7, bit 8 = collided with border, bit
                                * 9 = collided with foreground BACKTAB */
} intvstic_mob;

void intvstic_mob_get(const intvstic_snapshot *s, int index,
                      intvstic_mob *out);

/* Renders MOB `index`'s 16x16 (native resolution, before x_size/y_stretch)
 * bitmap into rgba (RGBA8888, 16*16*4 bytes) using its own foreground color
 * against a transparent (alpha 0) background -- a caller applying
 * x_size/y_stretch is just a blit-time scale, not a re-render. */
void intvstic_render_mob(const intvstic_snapshot *s, int index,
                         uint8_t *rgba);

/* ---- BACKTAB (the 160x96-pixel background card grid) --------------------
 * rgba must hold INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4 bytes.
 * Renders exactly what stic_draw_cstk/stic_draw_fgbg would, including
 * Color Stack mode's per-card color-stack-advance bit and its "colored
 * squares" special case -- see this file's header for what is and is not
 * covered. */
#define INTVSTIC_BACKTAB_COLS 20
#define INTVSTIC_BACKTAB_ROWS 12
#define INTVSTIC_BACKTAB_W (INTVSTIC_BACKTAB_COLS * 8)
#define INTVSTIC_BACKTAB_H (INTVSTIC_BACKTAB_ROWS * 8)
void intvstic_render_backtab(const intvstic_snapshot *s, uint8_t *rgba);

/* ---- GRAM/GROM card sheet ------------------------------------------------
 * A flat sheet of every card at native 8x8, in card order (GROM cards
 * 0-255 first -- GROM is a fixed 2048 bytes at 8 bytes/card, per
 * stic_init's own `for (i=0;i<2048;i++) stic->gmem[i]=grom_img[i];` --
 * then GRAM's, however many gram_size makes available).
 * rows*8 columns of 8x8 cards; rgba must hold cols*8 * rows*8 * 4 bytes.
 * Rendered in the STIC's own foreground/background convention (a card's
 * bits are 1bpp; this renders bit=1 as white and bit=0 as black, since a
 * card has no color of its own -- that comes from whatever MOB or BACKTAB
 * entry references it). */
#define INTVSTIC_CARDS_PER_ROW 16
void intvstic_render_cards(const intvstic_snapshot *s, int first_card,
                           int rows, uint8_t *rgba);
int  intvstic_card_count(const intvstic_snapshot *s); /* GROM (256) + GRAM
                                                        * (64/128/256, per
                                                        * gram_size) */

/* ---- palette -------------------------------------------------------------
 * The 16 colors actually in use for rendering (intv.gfx.palette -- the same
 * table gfx_desktop.c expands frames through), not a separately hardcoded
 * table, so this always matches what the main display shows. 16 swatches,
 * cell x cell each. */
#define INTVSTIC_PAL_CELL 32
void intvstic_render_palette(intvdebug *d, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* INTVSTIC_H */
