/*
 * intvdebug -- see intvdebug.h for the design (built on cp1600_instr_tick
 * and jzIntv's own native breakpoint mechanism, cp1600_set_breakpt).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * cfg/cfg.h's cfg_t embeds every peripheral's struct directly (not through
 * pointers), so every one of these headers has to be visible before cfg.h
 * is, in the same order jzintv.c (and core/jzintv/intv_host.c) uses them.
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
#include "debug/debug_dasm1600.h"
#include "event/event.h"
#include "ivoice/ivoice.h"
#include "jlp/jlp.h"
#include "fujinet/fujinet.h"
#include "locutus/locutus_adapt.h"
#include "cheat/cheat.h"
#include "cfg/mapping.h"
#include "cfg/cfg.h"

#include "intvdebug.h"

struct intvdebug {
    int engaged;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int paused;      /* the emulator thread is blocked inside the hook */
    int step_mode;   /* 1: step_count stays 1 (single-instruction ticks);
                      * 0: step_count is huge (only real breakpoints tick) */
    uint64_t stop_serial;

    uint16_t breakpoints[INTVDEBUG_MAX_BREAKPOINTS];
    int      breakpoint_count;
};

static struct intvdebug g_debug = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

intvdebug *intvdebug_get(void)
{
    return &g_debug;
}

/* Called synchronously on the EMULATOR thread, from inside cp1600_run's
 * do_instr_tick (see intvdebug.h's header comment). Blocks right here
 * until intvdebug_resume/step/step_over/step_out wakes it -- that block IS
 * the pause, there is no other place progress could be stopped.
 *
 * Engaging registers this hook, but that alone must NOT stop anything:
 * cp1600_run() reaches its do_instr_tick label -- and so calls this --
 * once per outer timeslice regardless of step_count (whenever the inner
 * instruction loop merely runs out of cycles to spend, not just when
 * steps_remaining hits 0), so a routine, nothing-actually-requested
 * invocation happens roughly every periph-tick batch even while running
 * free. Block only when there is an actual reason to: a real or one-shot
 * breakpoint just fired (cp1600->hit_breakpoint), or single-step mode is
 * active (d->step_mode, which is exactly the flag intvdebug_pause/step set
 * to force every instruction to reach here). Anything else is a no-op
 * return, and free-running costs one extra periph_t-level call per
 * timeslice -- not per instruction. */
static uint32_t debug_tick(periph_t *req, uint32_t cycles)
{
    (void)req;
    (void)cycles;
    intvdebug *const d = &g_debug;

    pthread_mutex_lock(&d->lock);
    if (intv.cp1600.hit_breakpoint == BK_NONE && !d->step_mode) {
        pthread_mutex_unlock(&d->lock);
        return 0;
    }

    d->paused = 1;
    d->stop_serial++;
    /* Clear it now, not just at the top of the next cp1600_run() call:
     * that call resets it once per invocation, not once per do_instr_tick
     * within it, and cp1600_run can call this hook several times per
     * invocation. Leaving a stale BK_BREAKPOINT/BK_TRACEPOINT here would
     * make every do_instr_tick for the rest of that invocation look like
     * another breakpoint hit, even after resuming. */
    intv.cp1600.hit_breakpoint = BK_NONE;
    while (d->paused)
        pthread_cond_wait(&d->cond, &d->lock);

    /* Set the run mode for what happens next, decided by whoever woke us
     * (intvdebug_resume clears step_mode, intvdebug_step/step_over/
     * step_out set or clear it as appropriate -- see each). */
    intv.cp1600.step_count = d->step_mode ? 1 : ~0U;
    pthread_mutex_unlock(&d->lock);

    return 0; /* NEVER CYC_MAX -- that aborts cp1600_run's whole timeslice. */
}

void intvdebug_set_engaged(intvdebug *d, int engaged)
{
    if (engaged == d->engaged)
        return;

    if (engaged) {
        d->engaged = 1;
        cp1600_instr_tick(&intv.cp1600, debug_tick, NULL);
    } else {
        intvdebug_resume(d); /* never leave the emulator thread parked */
        d->engaged = 0;
        cp1600_instr_tick(&intv.cp1600, NULL, NULL);
        intv.cp1600.step_count = ~0U;
    }
}

int intvdebug_is_engaged(const intvdebug *d)
{
    return d->engaged;
}

void intvdebug_pause(intvdebug *d)
{
    if (!d->engaged)
        return;
    pthread_mutex_lock(&d->lock);
    d->step_mode = 1;
    pthread_mutex_unlock(&d->lock);
    /* Racing a plain int write against the emulator thread's read of
     * step_count is deliberate and harmless here: the worst case is the
     * pause landing one instruction later than requested, never
     * corruption (see intvdebug.h's threading note). */
    intv.cp1600.step_count = 1;
}

void intvdebug_resume(intvdebug *d)
{
    pthread_mutex_lock(&d->lock);
    if (d->paused) {
        d->step_mode = 0;
        d->paused = 0;
        pthread_cond_broadcast(&d->cond);
    }
    pthread_mutex_unlock(&d->lock);
}

int intvdebug_is_paused(const intvdebug *d)
{
    return d->paused;
}

void intvdebug_step(intvdebug *d)
{
    pthread_mutex_lock(&d->lock);
    if (d->paused) {
        d->step_mode = 1;
        d->paused = 0;
        pthread_cond_broadcast(&d->cond);
    }
    pthread_mutex_unlock(&d->lock);
}

/* Does `text` name a JSR-family instruction? All four variants
 * (JSR/JSRE/JSRD/JSRi -- see the staged tree's src/debug/debug_dasm1600.c)
 * share this prefix and nothing else in the CP-1610 mnemonic set starts
 * with it. */
static int is_call_mnemonic(const char *text)
{
    return strncmp(text, "JSR", 3) == 0;
}

void intvdebug_step_over(intvdebug *d)
{
    intvdebug_dasm_line line;

    if (!d->paused)
        return;
    if (intvdebug_disassemble(d, intv.cp1600.r[7], intv.cp1600.D, &line, 1) != 1 ||
        !is_call_mnemonic(line.text)) {
        intvdebug_step(d);
        return;
    }

    cp1600_set_breakpt(&intv.cp1600,
                       (uint16_t)(intv.cp1600.r[7] + line.length),
                       CP1600_BKPT_ONCE);

    pthread_mutex_lock(&d->lock);
    d->step_mode = 0; /* run free until the one-shot breakpoint fires */
    d->paused = 0;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

void intvdebug_step_out(intvdebug *d)
{
    if (!d->paused)
        return;

    /* Best-effort; see intvdebug.h's own comment on why R5 and why this
     * can silently fail to stop rather than stop somewhere wrong. */
    cp1600_set_breakpt(&intv.cp1600, intv.cp1600.r[5], CP1600_BKPT_ONCE);

    pthread_mutex_lock(&d->lock);
    d->step_mode = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

uint64_t intvdebug_stop_serial(const intvdebug *d)
{
    return d->stop_serial;
}

int intvdebug_regs_get(intvdebug *d, intvdebug_regs *out)
{
    if (!d->paused)
        return 0;

    memcpy(out->r, intv.cp1600.r, sizeof(out->r));
    out->pc = intv.cp1600.r[7];
    out->S = intv.cp1600.S;
    out->C = intv.cp1600.C;
    out->O = intv.cp1600.O;
    out->Z = intv.cp1600.Z;
    out->I = intv.cp1600.I;
    out->D = intv.cp1600.D;
    return 1;
}

int intvdebug_read(intvdebug *d, uint16_t addr, uint16_t *dst, int n)
{
    cp1600_t *const cp = &intv.cp1600; /* CP1600_PK/_WR substitute their
                                        * argument textually into `c->...`,
                                        * so &intv.cp1600 (an expression, not
                                        * a plain identifier) would parse as
                                        * &(intv.cp1600->...) -- wrong
                                        * precedence. A local pointer var is
                                        * what every other CP1600_* call site
                                        * in the staged tree uses too. */
    int i;
    if (!d->paused)
        return 0;
    for (i = 0; i < n; i++)
        dst[i] = (uint16_t)CP1600_PK(cp, (uint16_t)(addr + i));
    return n;
}

int intvdebug_write(intvdebug *d, uint16_t addr, const uint16_t *src, int n)
{
    cp1600_t *const cp = &intv.cp1600;
    int i;
    if (!d->paused)
        return 0;
    for (i = 0; i < n; i++)
        CP1600_WR(cp, (uint16_t)(addr + i), src[i]);
    return n;
}

int intvdebug_disassemble(intvdebug *d, uint16_t addr, int dbd,
                          intvdebug_dasm_line *out, int max)
{
    cp1600_t *const cp = &intv.cp1600;
    int produced = 0;

    if (!d->paused)
        return 0;

    while (produced < max) {
        uint32_t w1 = CP1600_PK(cp, addr);
        uint32_t w2 = CP1600_PK(cp, (uint16_t)(addr + 1));
        uint32_t w3 = CP1600_PK(cp, (uint16_t)(addr + 2));
        char buf[64];
        int len = dasm1600(buf, addr, dbd, w1, w2, w3);
        if (len < 1)
            len = 1;

        out[produced].addr = addr;
        out[produced].length = len;
        snprintf(out[produced].text, sizeof(out[produced].text), "%s", buf);

        addr = (uint16_t)(addr + len);
        dbd = 0; /* DBD state is only known at the actual current PC */
        produced++;
    }
    return produced;
}

int intvdebug_breakpoint_toggle(intvdebug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->breakpoint_count; i++) {
        if (d->breakpoints[i] == addr) {
            cp1600_clr_breakpt(&intv.cp1600, addr, CP1600_BKPT);
            memmove(&d->breakpoints[i], &d->breakpoints[i + 1],
                   (size_t)(d->breakpoint_count - i - 1) * sizeof(uint16_t));
            d->breakpoint_count--;
            return 0;
        }
    }
    if (d->breakpoint_count >= INTVDEBUG_MAX_BREAKPOINTS)
        return -1;
    cp1600_set_breakpt(&intv.cp1600, addr, CP1600_BKPT);
    d->breakpoints[d->breakpoint_count++] = addr;
    return 1;
}

int intvdebug_breakpoint_is_set(intvdebug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->breakpoint_count; i++)
        if (d->breakpoints[i] == addr)
            return 1;
    return 0;
}

void intvdebug_breakpoint_clear_all(intvdebug *d)
{
    int i;
    for (i = 0; i < d->breakpoint_count; i++)
        cp1600_clr_breakpt(&intv.cp1600, d->breakpoints[i], CP1600_BKPT);
    d->breakpoint_count = 0;
}

int intvdebug_breakpoint_list(intvdebug *d, uint16_t *out, int max)
{
    int n = d->breakpoint_count < max ? d->breakpoint_count : max;
    memcpy(out, d->breakpoints, (size_t)n * sizeof(uint16_t));
    return n;
}
