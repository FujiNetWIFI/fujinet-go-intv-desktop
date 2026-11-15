/*
 * The Cart & I/O and Sound getters (see intvdebug.h): plain reads of the
 * machine's own structures, with no bus traffic, so nothing here can disturb
 * a running or stopped machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "machine.h"

#include "intv_host.h"
#include "intvdebug.h"

extern const int32_t ay8910_vol[16];   /* ay8910.c: the amplitude table */
extern int jlp_accel_on;   /* jlp.c: the $9F80-$9FFF accelerators are switched on */

static void pad_get(const pad_t *src, int present, intvdebug_pad *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->present = present;
    if (!present)
        return;
    memcpy(dst->side, src->side, sizeof(dst->side));
    memcpy(dst->output, src->io, sizeof(dst->output));
    memcpy(dst->left, src->l, sizeof(dst->left));
    memcpy(dst->right, src->r, sizeof(dst->right));
    memcpy(dst->keys, src->k, sizeof(dst->keys));
}

int intvdebug_io_get(intvdebug *dbg, intvdebug_io *out)
{
    const ivoice_t *const voice = &intv.ivoice;
    int idx;

    (void)dbg;
    memset(out, 0, sizeof(*out));
    if (!intv_host_is_running())
        return 0;

    out->ecs = intv.ecs_enable > 0;
    pad_get(&intv.pad0, 1, &out->pad[0]);
    pad_get(&intv.pad1, out->ecs, &out->pad[1]);
    for (idx = 0; idx < 3; idx++) {
        out->ecs_page[idx] = intv.ecs.rom[idx].page_sel;
        out->ecs_page_own[idx] = intv.ecs.rom[idx].page;
    }

    out->ivoice = voice->periph.bus != NULL;
    if (out->ivoice) {
        out->ivoice_lrq = voice->lrq;
        out->ivoice_ald = voice->ald;
        out->ivoice_pc = voice->pc;
        out->ivoice_halted = voice->halted;
        out->ivoice_fifo_sel = voice->fifo_sel;
        out->ivoice_mode = voice->mode;
        out->ivoice_page = voice->page;
        out->ivoice_fifo_head = voice->fifo_head;
        out->ivoice_fifo_tail = voice->fifo_tail;
        memcpy(out->ivoice_fifo, voice->fifo, sizeof(out->ivoice_fifo));
    }

    out->jlp = intv.jlp.periph.bus != NULL;
    if (out->jlp) {
        out->jlp_mb_hole = intv.jlp.mb_hole;
        out->jlp_flash = intv.jlp.sg_file != NULL || intv.jlp.sg_img != NULL;
        out->jlp_flash_bytes = intv.jlp.sg_bytes;
        out->jlp_accel = jlp_accel_on;
    }
    out->locutus = intv.locutus.periph.bus != NULL;
    out->fujinet_mailbox = intv.fujinet.mailbox_active ? 1 : 0;
    return 1;
}

int intvdebug_sound_get(intvdebug *dbg, int unit, intvdebug_psg *out)
{
    const ay8910_t *psg;
    int voice;

    memset(out, 0, sizeof(*out));
    if (!intvdebug_is_paused(dbg))
        return 0;
    if (unit == 0)
        psg = &intv.psg0;
    else if (unit == 1 && intv.ecs_enable > 0)
        psg = &intv.psg1;
    else
        return 0;
    if (!psg->periph.bus)
        return 0;

    out->present = 1;
    memcpy(out->reg, psg->reg, sizeof(out->reg));
    for (voice = 0; voice < 3; voice++) {
        const int amplitude = psg->reg[11 + voice];
        out->period[voice] = (psg->reg[voice] | (psg->reg[voice + 4] << 8)) & 0x0FFF;
        out->hz[voice] = out->period[voice] ? 111860.8 / out->period[voice] : 0.0;
        out->tone_on[voice] = !((psg->reg[8] >> voice) & 1);
        out->noise_on[voice] = !((psg->reg[8] >> (voice + 3)) & 1);
        out->level[voice] = amplitude & 0x0F;
        out->envelope[voice] = (amplitude & 0x30) != 0;
        out->channel_state[voice] = psg->chan[voice] & 1;
    }
    out->noise_period = psg->reg[9] & 0x1F;
    out->env_period = (psg->reg[3] | (psg->reg[7] << 8)) & 0xFFFF;
    out->env_shape = psg->reg[10] & 0x0F;
    out->env_level = 0;
    for (voice = 0; voice < 16; voice++)   /* env_vol holds the chip's amplitude table value */
        if (ay8910_vol[voice] <= psg->env_vol)
            out->env_level = voice;
    return 1;
}
