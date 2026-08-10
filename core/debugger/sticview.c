/*
 * intvstic -- see intvstic.h for the design and exactly what is (and is
 * not) covered.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

/*
 * cfg/cfg.h's cfg_t embeds every peripheral's struct directly, so every one
 * of these headers has to be visible before cfg.h is, in the same order
 * jzintv.c (and core/debugger/debugger.c) uses them.
 */
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

#include "intvstic.h"

void intvstic_snapshot_get(intvdebug *d, intvstic_snapshot *out)
{
    if (!intvdebug_is_paused(d)) {
        memset(out, 0, sizeof(*out));
        return;
    }

    memcpy(out->raw, intv.stic.raw, sizeof(out->raw));
    memcpy(out->gmem, intv.stic.gmem, sizeof(out->gmem));
    memcpy(out->mob_bmp, intv.stic.mob_bmp, sizeof(out->mob_bmp));
    out->mode = intv.stic.mode;
    out->gram_size = intv.stic.gram_size;
}

int intvstic_describe_mode(const intvstic_snapshot *s, char *out, int max)
{
    static const char *const gram_sizes[] = { "64", "128", "256" };
    const char *gs = (s->gram_size >= 0 && s->gram_size <= 2)
                    ? gram_sizes[s->gram_size] : "?";
    return snprintf(out, (size_t)max, "Mode: %s   GRAM: %s cards",
                    s->mode == 1 ? "Foreground/Background" : "Color Stack",
                    gs);
}

int intvstic_describe_registers(const intvstic_snapshot *s, char *out,
                                int max)
{
    int n = 0;
    n += snprintf(out + n, (size_t)(max - n),
                 "Color stack: %u %u %u %u\n",
                 s->raw[0x28] & 0xF, s->raw[0x29] & 0xF,
                 s->raw[0x2A] & 0xF, s->raw[0x2B] & 0xF);
    if (n < max)
        n += snprintf(out + n, (size_t)(max - n),
                     "Border color: %u\n", s->raw[0x2C] & 0xF);
    if (n < max)
        n += snprintf(out + n, (size_t)(max - n),
                     "Horizontal delay: %u\n", s->raw[0x30] & 7);
    if (n < max)
        n += snprintf(out + n, (size_t)(max - n),
                     "Vertical delay: %u\n", s->raw[0x31] & 7);
    if (n < max)
        n += snprintf(out + n, (size_t)(max - n),
                     "BACKTAB: %u columns x %u rows\n",
                     (s->raw[0x32] & 1) ? 19u : 20u,
                     (s->raw[0x32] & 2) ? 32u : 16u);
    return n;
}

int intvstic_format_state(const intvstic_snapshot *s, char *out, int max)
{
    int n = intvstic_describe_mode(s, out, max);
    if (n < max - 1) {
        out[n++] = '\n';
        n += intvstic_describe_registers(s, out + n, max - n);
    }
    return n;
}

void intvstic_mob_get(const intvstic_snapshot *s, int index,
                      intvstic_mob *out)
{
    uint32_t x_reg, y_reg, a_reg;

    if (index < 0 || index >= INTVSTIC_MOB_COUNT) {
        memset(out, 0, sizeof(*out));
        return;
    }

    x_reg = s->raw[index + 0x00];
    y_reg = s->raw[index + 0x08];
    a_reg = s->raw[index + 0x10];

    out->x = (int)(x_reg & 0xFF);
    out->y = (int)((y_reg & 0x7F) * 2);
    out->x_size = (x_reg & 0x0400) ? 2 : 1;
    out->flip_x = (y_reg & 0x0400) ? 1 : 0;
    out->flip_y = (y_reg & 0x0800) ? 1 : 0;
    out->y_res = (y_reg & 0x80) ? 16 : 8;
    out->y_stretch = 1 << ((y_reg >> 8) & 3);
    out->color = (int)(((a_reg >> 9) & 0x08) | (a_reg & 0x07));
    out->gram = (a_reg & 0x800) ? 1 : 0;
    out->priority = (a_reg & 0x2000) ? 1 : 0;
    out->visible = (x_reg & 0x200) ? 1 : 0;
    out->collision = (uint16_t)s->raw[index + 0x18];

    /* Same mask chain stic_do_mob applies: FG/BG mode's 64-card limit,
     * GRAM's own size limit, and double-height MOBs' even-card-only rule. */
    {
        const uint32_t mask_fgbg = s->mode == 1 ? 0x9F8u : ~0U;
        uint32_t mask_gram = ~0U;
        if (a_reg & 0x800) {
            switch (s->gram_size) {
            case 0: mask_gram = 0x3F; break;
            case 1: mask_gram = 0x7F; break;
            default: mask_gram = 0xFF; break;
            }
            /* GRAM card indices in gmem[] start after GROM's 256 cards
             * (2048 bytes in, per stic_init); the mask above is relative
             * to that region, matching stic->gram_mask's own convention. */
        }
        const uint32_t mask_dhgt = out->y_res == 16 ? 0xFF0u : ~0U;
        out->card = (int)(a_reg & 0xFF8u & mask_fgbg & mask_gram & mask_dhgt);
    }
}

void intvstic_render_mob(const intvstic_snapshot *s, int index,
                         uint8_t *rgba)
{
    intvstic_mob mob;
    uint8_t r, g, b;
    int x, y;

    intvstic_mob_get(s, index, &mob);
    /* intv.gfx.palette isn't part of the snapshot (it changes far less
     * often than STIC registers and reading it needs no pause -- it is set
     * once at gfx_init and only reset on a palette-switch request), read
     * directly here. */
    r = intv.gfx.palette.color[mob.color][0];
    g = intv.gfx.palette.color[mob.color][1];
    b = intv.gfx.palette.color[mob.color][2];

    for (y = 0; y < 16; y++) {
        const uint16_t bits = s->mob_bmp[index][y];
        for (x = 0; x < 16; x++) {
            /* mob_bmp's bit 15 is the leftmost pixel (see stic_do_mob's own
             * bit_remap tables, which produce MSB-first 16-bit rows). */
            const int set = (bits >> (15 - x)) & 1;
            uint8_t *px = &rgba[(y * 16 + x) * 4];
            px[0] = r; px[1] = g; px[2] = b;
            px[3] = set ? 0xFF : 0x00;
        }
    }
}

int intvstic_card_count(const intvstic_snapshot *s)
{
    static const int gram_cards[] = { 64, 128, 256 };
    const int gram = (s->gram_size >= 0 && s->gram_size <= 2)
                    ? gram_cards[s->gram_size] : 64;
    return 256 + gram; /* GROM is always 256 cards (2048 bytes, 8/card) */
}

void intvstic_render_cards(const intvstic_snapshot *s, int first_card,
                           int rows, uint8_t *rgba)
{
    const int total = intvstic_card_count(s);
    const int cols = INTVSTIC_CARDS_PER_ROW;
    const int sheet_w = cols * 8;
    int row, col, y, x;

    memset(rgba, 0, (size_t)(sheet_w * rows * 8 * 4));

    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            const int card = first_card + row * cols + col;
            if (card < 0 || card >= total)
                continue;
            for (y = 0; y < 8; y++) {
                /* MSB-first raster, the same convention every other 1bpp
                 * character-cell chip of this era uses (bit 7 = leftmost
                 * pixel). */
                const uint8_t byte = s->gmem[card * 8 + y];
                for (x = 0; x < 8; x++) {
                    const int set = (byte >> (7 - x)) & 1;
                    const int px_x = col * 8 + x;
                    const int px_y = row * 8 + y;
                    uint8_t *px =
                        &rgba[(px_y * sheet_w + px_x) * 4];
                    const uint8_t v = set ? 0xFF : 0x00;
                    px[0] = v; px[1] = v; px[2] = v; px[3] = 0xFF;
                }
            }
        }
    }
}

void intvstic_render_palette(intvdebug *d, uint8_t *rgba)
{
    int i, y, x;

    if (!intvdebug_is_paused(d)) {
        memset(rgba, 0, (size_t)(INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4));
        return;
    }

    for (i = 0; i < 16; i++) {
        const uint8_t r = intv.gfx.palette.color[i][0];
        const uint8_t g = intv.gfx.palette.color[i][1];
        const uint8_t b = intv.gfx.palette.color[i][2];
        for (y = 0; y < INTVSTIC_PAL_CELL; y++) {
            for (x = 0; x < INTVSTIC_PAL_CELL; x++) {
                uint8_t *px = &rgba[(y * (16 * INTVSTIC_PAL_CELL) +
                                     i * INTVSTIC_PAL_CELL + x) * 4];
                px[0] = r; px[1] = g; px[2] = b; px[3] = 0xFF;
            }
        }
    }
}
