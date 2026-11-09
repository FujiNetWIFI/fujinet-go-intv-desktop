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
#include "fujinet/fujibus.h"
#include "fujinet/fujinet.h"
#include "locutus/locutus_adapt.h"
#include "cheat/cheat.h"
#include "cfg/mapping.h"
#include "cfg/cfg.h"

#include "fujitrace.h"
#include "intv_host.h"
#include "intvdebug.h"
#include "srcmap.h"

enum { LINE_NONE = 0, LINE_INTO, LINE_OVER, LINE_OUT };

struct intvdebug {
    int engaged;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int paused;      /* the emulator thread is blocked inside the hook */
    int step_mode;   /* 1: step_count stays 1 (single-instruction ticks);
                      * 0: step_count is huge (only real breakpoints tick) */
    int line_mode;   /* LINE_*: a by-BASIC-line step is in flight; step_mode
                      * is 1 too, and the hook keeps going until a line starts */
    uint16_t line_r6;/* R6 when that step began */
    uint64_t stop_serial;

    /* The breakpoint/watchpoint table. bp_lock guards it and the hit record
     * (the emulator thread takes it from the watch callback). */
    pthread_mutex_t bp_lock;
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    int      nbp;
    int      next_bp_id;
    unsigned bp_gen;
    int      watch_dirty;       /* the bus registration needs rebuilding */
    int      watch_hit;         /* a watchpoint fired: INTVDEBUG_STOP_WATCH_* */
    int      pause_req;         /* intvdebug_pause asked for this stop */
    intvdebug_stop_info hit;    /* the watchpoint that fired */
    intvdebug_stop_info last;   /* why the machine last stopped */

    pthread_mutex_t trace_lock; /* the FujiNet trace's alone */
    fujitrace trace;
};

static struct intvdebug g_debug = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .trace_lock = PTHREAD_MUTEX_INITIALIZER,
    .bp_lock = PTHREAD_MUTEX_INITIALIZER,
    .next_bp_id = 1,
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
static void watch_apply(intvdebug *d);
static int bp_find_exec(const intvdebug *d, uint16_t addr);

/* Was the instruction that just ran a JSR? Its destination's first word is
 * where a callee starts, before the PSHR R5 that every IntyBASIC procedure
 * opens with has deepened the stack, so depth alone can't tell that word from
 * the caller's own next line. */
static int prev_was_call(void)
{
    cp1600_t *const cp = &intv.cp1600;
    const uint16_t a = (uint16_t)cp->oldpc;
    char text[64];

    dasm1600(text, a, 0, CP1600_PK(cp, a), CP1600_PK(cp, (uint16_t)(a + 1)),
             CP1600_PK(cp, (uint16_t)(a + 2)));
    return strstr(text, "JSR") != NULL;
}

static uint32_t debug_tick(periph_t *req, uint32_t cycles)
{
    (void)req;
    (void)cycles;
    intvdebug *const d = &g_debug;

    pthread_mutex_lock(&d->lock);
    if (d->watch_dirty)
        watch_apply(d);   /* a watchpoint was added while running */
    if (intv.cp1600.hit_breakpoint == BK_NONE && !d->step_mode) {
        pthread_mutex_unlock(&d->lock);
        return 0;
    }

    /* A by-line step keeps single-stepping silently until the PC lands on
     * the first word of a BASIC line at an acceptable stack depth (a real
     * breakpoint still stops it, below). The stack grows upward on the
     * CP-1610, so a callee runs at a larger R6 than its caller. */
    if (d->line_mode != LINE_NONE && intv.cp1600.hit_breakpoint == BK_NONE) {
        const uint16_t sp = intv.cp1600.r[6];
        int stop = intvsrcmap_is_line_start(intvsrcmap_shared(),
                                            intv.cp1600.r[7]);
        if (stop && d->line_mode == LINE_OVER && sp > d->line_r6)
            stop = 0;
        if (stop && d->line_mode == LINE_OUT && sp >= d->line_r6)
            stop = 0;
        if (stop && d->line_mode == LINE_OVER && prev_was_call())
            stop = 0;
        if (!stop) {
            pthread_mutex_unlock(&d->lock);
            return 0;
        }
    }
    d->line_mode = LINE_NONE;

    /* Why we are stopping, for the status line and the Breakpoints tab. */
    pthread_mutex_lock(&d->bp_lock);
    memset(&d->last, 0, sizeof(d->last));
    if (d->watch_hit) {
        d->last = d->hit;
        d->last.reason = d->watch_hit;
        d->watch_hit = 0;
    } else if (intv.cp1600.hit_breakpoint != BK_NONE) {
        const int i = bp_find_exec(d, intv.cp1600.r[7]);
        d->last.reason = INTVDEBUG_STOP_BREAKPOINT;
        if (i >= 0 && d->bps[i].enabled && intv.cp1600.hit_breakpoint == BK_BREAKPOINT) {
            d->last.bp_id = d->bps[i].id;
            d->bps[i].hits++;
            d->bp_gen++;
        }
    } else if (d->pause_req) {
        d->last.reason = INTVDEBUG_STOP_PAUSE;
    } else {
        d->last.reason = INTVDEBUG_STOP_STEP;
    }
    d->pause_req = 0;
    d->last.pc = intv.cp1600.r[7];
    if (d->last.reason == INTVDEBUG_STOP_BREAKPOINT || d->last.reason == INTVDEBUG_STOP_STEP ||
        d->last.reason == INTVDEBUG_STOP_PAUSE)
        d->last.addr = d->last.pc;
    d->last.serial = d->stop_serial + 1;
    pthread_mutex_unlock(&d->bp_lock);

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
    /* step_count only feeds steps_remaining when that reaches 0. Pausing out
     * of a free run leaves it at ~step_count minus whatever ran, so a Step
     * would run on until the next timeslice (or breakpoint) instead of one
     * instruction. Restart the countdown here, on the emulator thread:
     * 1 = stop after the next instruction, 0 = unlimited (cp1600_run then
     * refills it from step_count). */
    intv.cp1600.steps_remaining = d->step_mode ? 1 : 0;
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
    d->pause_req = 1;
    d->line_mode = LINE_NONE; /* a pause must stop a by-line step too */
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

/* Stopping at an address a breakpoint is armed on (a step or a by-line stop
 * lands there without the breakpoint having fired) must not make the next
 * run fire it again before executing anything. cp1600_set_breakpt on the
 * current PC arms it for the instruction after this one instead. Only while
 * paused. */
static void step_off_breakpoint(intvdebug *d)
{
    const uint16_t pc = intv.cp1600.r[7];
    if (intvdebug_breakpoint_is_set(d, pc))
        cp1600_set_breakpt(&intv.cp1600, pc, CP1600_BKPT);
}

void intvdebug_step(intvdebug *d)
{
    if (d->paused)
        step_off_breakpoint(d);
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
    /* dasm1600's text leads with the address and the instruction's hex
     * words ("   $50CD  0004 0150 00D5   JSR  R5,$50D5"), so the mnemonic is
     * not at the start. Nothing else in the line can spell JSR. */
    return strstr(text, "JSR") != NULL;
}

void intvdebug_step_over(intvdebug *d)
{
    intvdebug_dasm_line line;

    if (!d->paused)
        return;
    step_off_breakpoint(d);
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
    step_off_breakpoint(d);

    /* Best-effort; see intvdebug.h's own comment on why R5 and why this
     * can silently fail to stop rather than stop somewhere wrong. */
    cp1600_set_breakpt(&intv.cp1600, intv.cp1600.r[5], CP1600_BKPT_ONCE);

    pthread_mutex_lock(&d->lock);
    d->step_mode = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

static void begin_line_step(intvdebug *d, int mode)
{
    if (d->paused)
        step_off_breakpoint(d);
    pthread_mutex_lock(&d->lock);
    if (d->paused) {
        d->line_r6 = intv.cp1600.r[6];
        /* Stopped on a callee's first word, straight after its JSR: the
         * PSHR R5 that opens an IntyBASIC procedure hasn't run, so the
         * stack is one shallower than the body will be. Count the body as
         * this routine's own level, or Over would skip all of it and Out
         * would not stop until the caller's caller. */
        if (mode != LINE_INTO && prev_was_call())
            d->line_r6++;
        d->line_mode = mode;
        d->step_mode = 1;
        d->paused = 0;
        pthread_cond_broadcast(&d->cond);
    }
    pthread_mutex_unlock(&d->lock);
}

void intvdebug_step_line(intvdebug *d)
{
    begin_line_step(d, LINE_INTO);
}

void intvdebug_step_line_over(intvdebug *d)
{
    begin_line_step(d, LINE_OVER);
}

void intvdebug_step_line_out(intvdebug *d)
{
    begin_line_step(d, LINE_OUT);
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

static uint16_t fold_ascii(uint16_t w)
{
    const uint16_t c = w & 0xFF;
    return c >= 'a' && c <= 'z' ? (uint16_t)((w & 0xFF00) | (c - 32)) : w;
}

int intvdebug_search(intvdebug *d, const uint16_t *pattern, int n, uint16_t mask, int flags,
                     uint16_t from, uint16_t to, uint16_t *out, int max)
{
    uint16_t buf[512 + 64];
    uint32_t base, last = to;
    int found = 0, i;

    if (!d->paused)
        return -1;
    if (n < 1 || n > 64 || to < from)
        return 0;
    for (base = from; base <= last && found < max; base += 512) {
        /* a chunk and enough words past it for a match that straddles the end */
        const int want = (int)(last - base + 1) < 512 + n - 1 ? (int)(last - base + 1) : 512 + n - 1;
        int got = intvdebug_read(d, (uint16_t)base, buf, want);
        for (i = 0; i < 512 && i < got && base + (uint32_t)i <= last && found < max; i++) {
            int k;
            if (i + n > got)
                break;
            for (k = 0; k < n; k++) {
                uint16_t w = buf[i + k], p = pattern[k];
                if (flags & INTVDEBUG_SEARCH_NOCASE) {
                    w = fold_ascii(w);
                    p = fold_ascii(p);
                }
                if ((w & mask) != (p & mask))
                    break;
            }
            if (k == n)
                out[found++] = (uint16_t)(base + (uint32_t)i);
        }
    }
    return found;
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

/* ---- the breakpoint table ------------------------------------------------ */

/* One 16-word bin of the bus is the grain of address decode. */
#define WATCH_BINS 4096

static periph_t g_watch;        /* answers on the bins that hold a watchpoint */
static periph_bus_t *g_watch_bus;
static uint16_t g_shadow[65536];/* the last value written, for CHANGED */

static int bp_find(const intvdebug *d, int id)
{
    int i;
    for (i = 0; i < d->nbp; i++)
        if (d->bps[i].id == id)
            return i;
    return -1;
}

static int bp_find_exec(const intvdebug *d, uint16_t addr)
{
    int i;
    for (i = 0; i < d->nbp; i++)
        if ((d->bps[i].type & INTVDEBUG_BP_EXEC) && d->bps[i].lo == addr)
            return i;
    return -1;
}

/* Arms or clears jzIntv's own breakpoint for an EXEC entry. Safe while
 * running (the long-standing behaviour of the toggle). */
static void bp_arm_exec(const intvdebug_bp *bp)
{
    if (bp->enabled)
        cp1600_set_breakpt(&intv.cp1600, bp->lo, CP1600_BKPT);
    else
        cp1600_clr_breakpt(&intv.cp1600, bp->lo, CP1600_BKPT);
}

static uint32_t watch_rd(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data);
static void watch_wr(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data);

static uint32_t watch_peek(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)p; (void)bus; (void)addr; (void)data;
    return ~0U;   /* a peek must not be seen, and must not drive the bus */
}

static void watch_poke(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)p; (void)bus; (void)addr; (void)data;
}

/* Rebuilds which bins our device answers on. Only while the emulator thread
 * is parked or is the caller (the bins are walked by every bus access). */
static void watch_apply(intvdebug *d)
{
    static uint8_t bins[WATCH_BINS];
    periph_bus_t *bus = intv.intv;
    int i, b, any = 0;

    pthread_mutex_lock(&d->bp_lock);
    d->watch_dirty = 0;
    memset(bins, 0, sizeof(bins));
    for (i = 0; i < d->nbp; i++) {
        const intvdebug_bp *bp = &d->bps[i];
        if (!bp->enabled || !(bp->type & (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE)))
            continue;
        for (b = bp->lo >> 4; b <= bp->hi >> 4; b++)
            bins[b] = 1;
        any = 1;
        if (bp->cond == INTVDEBUG_COND_CHANGED) { /* seed the old values */
            cp1600_t *const cp = &intv.cp1600;
            int a;
            for (a = bp->lo; a <= bp->hi; a++)
                g_shadow[a] = (uint16_t)CP1600_PK(cp, (uint16_t)a);
        }
    }
    pthread_mutex_unlock(&d->bp_lock);

    if (!bus)
        return;
    if (g_watch_bus != bus) {   /* a new session built a new bus */
        memset(&g_watch, 0, sizeof(g_watch));
        g_watch.read = watch_rd;
        g_watch.write = watch_wr;
        g_watch.peek = watch_peek;
        g_watch.poke = watch_poke;
        g_watch.addr_base = 0;
        g_watch.addr_mask = 0xFFFF;
        g_watch_bus = bus;
    } else if (g_watch.bus) {
        periph_unregister(bus, &g_watch);
    }
    if (!any)
        return;
    for (b = 0; b < WATCH_BINS; b++) {   /* one registration per run of bins */
        int e = b;
        if (!bins[b])
            continue;
        while (e + 1 < WATCH_BINS && bins[e + 1])
            e++;
        periph_register(bus, &g_watch, (uint32_t)b << 4, ((uint32_t)e << 4) | 15, "watch");
        b = e;
    }
}

/* Called on the emulator thread for each CPU data access on a watched bin. */
static void watch_hit(periph_t *bus, uint32_t addr, uint32_t data, int is_write)
{
    intvdebug *const d = &g_debug;
    const uint16_t a = (uint16_t)addr, v = (uint16_t)data;
    int i;

    if (!d->engaged || !bus || bus->req != &intv.cp1600.periph)
        return;   /* only the CPU's own accesses, and only while engaged */
    pthread_mutex_lock(&d->bp_lock);
    for (i = 0; i < d->nbp; i++) {
        intvdebug_bp *bp = &d->bps[i];
        const int kind = is_write ? INTVDEBUG_BP_WRITE : INTVDEBUG_BP_READ;
        const uint16_t mask = bp->mask ? bp->mask : 0xFFFF;
        int fire = 1;

        if (!bp->enabled || !(bp->type & kind) || a < bp->lo || a > bp->hi)
            continue;
        if (is_write) {
            if (bp->cond == INTVDEBUG_COND_CHANGED)
                fire = (v & mask) != (g_shadow[a] & mask);
            else if (bp->cond == INTVDEBUG_COND_EQ)
                fire = (v & mask) == (bp->value & mask);
            else if (bp->cond == INTVDEBUG_COND_NE)
                fire = (v & mask) != (bp->value & mask);
        }
        if (!fire)
            continue;
        bp->hits++;
        d->bp_gen++;
        if (!d->watch_hit) {   /* the first hit of the instruction names the stop */
            d->watch_hit = is_write ? INTVDEBUG_STOP_WATCH_WRITE : INTVDEBUG_STOP_WATCH_READ;
            d->hit.bp_id = bp->id;
            d->hit.addr = a;
            d->hit.value = v;
            d->hit.old_value = g_shadow[a];
        }
    }
    if (is_write)
        g_shadow[a] = v;
    if (d->watch_hit) {
        intv.cp1600.hit_breakpoint = BK_BREAKPOINT;
        intv.cp1600.steps_remaining = 1;   /* stop after this instruction */
    }
    pthread_mutex_unlock(&d->bp_lock);
}

static uint32_t watch_rd(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)p; (void)data;
    watch_hit(bus, addr, 0, 0);
    return ~0U;   /* merely listening: leave the bus value alone */
}

static void watch_wr(periph_t *p, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)p;
    watch_hit(bus, addr, data, 1);
}

/* A table change: apply the bus side now if the machine is parked, else the
 * emulator thread does it at its next hook. */
static void bp_changed(intvdebug *d, int bus_side)
{
    d->bp_gen++;
    if (!bus_side)
        return;
    if (d->paused)
        watch_apply(d);
    else
        d->watch_dirty = 1;
}

int intvdebug_bp_add(intvdebug *d, const intvdebug_bp *spec)
{
    intvdebug_bp bp;
    int id;

    if (!spec || !(spec->type & (INTVDEBUG_BP_EXEC | INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE)))
        return -1;
    pthread_mutex_lock(&d->bp_lock);
    if (d->nbp >= INTVDEBUG_MAX_BREAKPOINTS) {
        pthread_mutex_unlock(&d->bp_lock);
        return -1;
    }
    bp = *spec;
    if (bp.type & INTVDEBUG_BP_EXEC) {
        bp.type = INTVDEBUG_BP_EXEC;   /* code stops are exact addresses */
        bp.hi = bp.lo;
        bp.cond = INTVDEBUG_COND_ANY;
    } else if (bp.hi < bp.lo) {
        const uint16_t t = bp.lo;
        bp.lo = bp.hi;
        bp.hi = t;
    }
    if (!(bp.type & INTVDEBUG_BP_WRITE))
        bp.cond = INTVDEBUG_COND_ANY;
    bp.id = id = d->next_bp_id++;
    bp.enabled = 1;
    bp.hits = 0;
    d->bps[d->nbp++] = bp;
    pthread_mutex_unlock(&d->bp_lock);

    if (bp.type & INTVDEBUG_BP_EXEC)
        bp_arm_exec(&bp);
    bp_changed(d, !(bp.type & INTVDEBUG_BP_EXEC));
    return id;
}

int intvdebug_bp_remove(intvdebug *d, int id)
{
    intvdebug_bp gone;
    int i, bus_side;

    pthread_mutex_lock(&d->bp_lock);
    i = bp_find(d, id);
    if (i < 0) {
        pthread_mutex_unlock(&d->bp_lock);
        return 0;
    }
    gone = d->bps[i];
    memmove(&d->bps[i], &d->bps[i + 1], (size_t)(d->nbp - i - 1) * sizeof(d->bps[0]));
    d->nbp--;
    pthread_mutex_unlock(&d->bp_lock);

    bus_side = !(gone.type & INTVDEBUG_BP_EXEC);
    if (gone.type & INTVDEBUG_BP_EXEC) {
        gone.enabled = 0;
        if (bp_find_exec(d, gone.lo) < 0)
            bp_arm_exec(&gone);
    }
    bp_changed(d, bus_side);
    return 1;
}

int intvdebug_bp_enable(intvdebug *d, int id, int enabled)
{
    intvdebug_bp bp;
    int i;

    pthread_mutex_lock(&d->bp_lock);
    i = bp_find(d, id);
    if (i < 0) {
        pthread_mutex_unlock(&d->bp_lock);
        return 0;
    }
    d->bps[i].enabled = enabled ? 1 : 0;
    bp = d->bps[i];
    pthread_mutex_unlock(&d->bp_lock);
    if (bp.type & INTVDEBUG_BP_EXEC)
        bp_arm_exec(&bp);
    bp_changed(d, !(bp.type & INTVDEBUG_BP_EXEC));
    return 1;
}

int intvdebug_bp_clear_hits(intvdebug *d)
{
    int i;
    pthread_mutex_lock(&d->bp_lock);
    for (i = 0; i < d->nbp; i++)
        d->bps[i].hits = 0;
    d->bp_gen++;
    pthread_mutex_unlock(&d->bp_lock);
    return 1;
}

int intvdebug_bp_list(intvdebug *d, intvdebug_bp *out, int max)
{
    int n;
    pthread_mutex_lock(&d->bp_lock);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bps, (size_t)n * sizeof(out[0]));
    pthread_mutex_unlock(&d->bp_lock);
    return n;
}

unsigned intvdebug_bp_generation(const intvdebug *d)
{
    return d->bp_gen;
}

int intvdebug_breakpoint_toggle(intvdebug *d, uint16_t addr)
{
    int i;
    intvdebug_bp spec;

    pthread_mutex_lock(&d->bp_lock);
    i = bp_find_exec(d, addr);
    if (i >= 0) {
        const int id = d->bps[i].id;
        pthread_mutex_unlock(&d->bp_lock);
        intvdebug_bp_remove(d, id);
        return 0;
    }
    pthread_mutex_unlock(&d->bp_lock);
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_EXEC;
    spec.lo = spec.hi = addr;
    return intvdebug_bp_add(d, &spec) < 0 ? -1 : 1;
}

int intvdebug_breakpoint_state(intvdebug *d, uint16_t addr)
{
    int i, state = 0;
    pthread_mutex_lock(&d->bp_lock);
    i = bp_find_exec(d, addr);
    if (i >= 0)
        state = d->bps[i].enabled ? 1 : 2;
    pthread_mutex_unlock(&d->bp_lock);
    return state;
}

int intvdebug_breakpoint_is_set(intvdebug *d, uint16_t addr)
{
    return intvdebug_breakpoint_state(d, addr) == 1;
}

void intvdebug_breakpoint_clear_all(intvdebug *d)
{
    int i;
    pthread_mutex_lock(&d->bp_lock);
    for (i = 0; i < d->nbp; i++)
        if (d->bps[i].type & INTVDEBUG_BP_EXEC)
            cp1600_clr_breakpt(&intv.cp1600, d->bps[i].lo, CP1600_BKPT);
    d->nbp = 0;
    pthread_mutex_unlock(&d->bp_lock);
    bp_changed(d, 1);
}

int intvdebug_breakpoint_list(intvdebug *d, uint16_t *out, int max)
{
    int i, n = 0;
    pthread_mutex_lock(&d->bp_lock);
    for (i = 0; i < d->nbp && n < max; i++)
        if ((d->bps[i].type & INTVDEBUG_BP_EXEC) && d->bps[i].enabled) {
            int k = n++;
            while (k > 0 && out[k - 1] > d->bps[i].lo) { /* ascending */
                out[k] = out[k - 1];
                k--;
            }
            out[k] = d->bps[i].lo;
        }
    pthread_mutex_unlock(&d->bp_lock);
    return n;
}

int intvdebug_last_stop(intvdebug *d, intvdebug_stop_info *out)
{
    pthread_mutex_lock(&d->bp_lock);
    *out = d->last;
    pthread_mutex_unlock(&d->bp_lock);
    return out->reason != INTVDEBUG_STOP_NONE;
}

/* ---- FujiNet trace ------------------------------------------------------- */

/* fn_sock's tap: on the emulator thread, inside each send/recv. */
static void fujinet_tap(int dir, const uint8_t *buf, size_t len)
{
    pthread_mutex_lock(&g_debug.trace_lock);
    fujitrace_feed(&g_debug.trace, dir, buf, len);
    pthread_mutex_unlock(&g_debug.trace_lock);
}

/* The trace is always on: it is cheap, and it is most useful about what
 * happened before anyone thought to look. It installs itself as the program
 * loads, because the session cannot call in here: Android's fujinet-go-intv
 * builds core/src/session.c without core/debugger/. Every frontend links
 * this file through its debugger window, so that is enough. */
__attribute__((constructor)) static void fujinet_tap_install(void)
{
    fn_sock_set_trace(fujinet_tap);
}

int intvdebug_fujinet_trace(intvdebug *d, intvdebug_fuji_packet *out, int max)
{
    int n;
    pthread_mutex_lock(&d->trace_lock);
    n = fujitrace_copy(&d->trace, out, max);
    pthread_mutex_unlock(&d->trace_lock);
    return n;
}

void intvdebug_fujinet_trace_clear(intvdebug *d)
{
    pthread_mutex_lock(&d->trace_lock);
    fujitrace_clear(&d->trace);
    pthread_mutex_unlock(&d->trace_lock);
}

/* Reads the peripheral without stopping the machine: single bytes and ints,
 * shown, never acted on, so a torn read costs one stale line. */
#define MB(x) (fn->ram[(x) - FUJI_MB_BASE])
int intvdebug_fujinet_state(intvdebug *d, char *dst, int dstsz)
{
    static const char *const link_names[] = {
        "disconnected", "connecting", "connected, idle",
        "connected, sending", "connected, awaiting the reply",
    };
    static const char *const status_names[] = { "IDLE", "BUSY", "OK", "ERR" };
    static const char *const err_names[] = {
        "OK", "no link", "timeout", "bad frame", "too big",
    };
    static const char *const boot_names[] = {
        "idle", "opening", "transferring", "mapping",
    };
    const fujinet_t *const fn = &intv.fujinet;
    const unsigned state = (unsigned)fn->state;
    const unsigned status = MB(FUJI_MB_STATUS);
    const unsigned err = MB(FUJI_MB_ERR);
    const unsigned boot = MB(FUJI_MB_BOOT_STATE);
    const unsigned reply = MB(FUJI_MB_REPLY_CMD);
    const unsigned m0 = MB(FUJI_MB_MAGIC0), m1 = MB(FUJI_MB_MAGIC1);
    char boot_text[48], path[201];
    int i;
    (void)d;

    if (!intv_host_is_running())
        return snprintf(dst, (size_t)dstsz, "The Intellivision is off.\n");
    if (!fn->periph.tick)
        return snprintf(dst, (size_t)dstsz, "No FujiNet mailbox this session.\n");

    if (boot == FUJI_BOOT_FAILED)
        snprintf(boot_text, sizeof(boot_text), "failed, reason %u",
                 (unsigned)MB(FUJI_MB_BOOT_ERR));
    else if (boot < sizeof(boot_names) / sizeof(boot_names[0]))
        snprintf(boot_text, sizeof(boot_text), "%s, %u%%", boot_names[boot],
                 (unsigned)MB(FUJI_MB_BOOT_PCT));
    else
        snprintf(boot_text, sizeof(boot_text), "%02X", boot);
    /* The guest sent it: printable ASCII only. */
    for (i = 0; i < (int)sizeof(path) - 1 && fn->last_boot_path[i]; i++)
        path[i] = fn->last_boot_path[i] >= 0x20 && fn->last_boot_path[i] < 0x7F
                      ? fn->last_boot_path[i] : '.';
    path[i] = '\0';

    return snprintf(dst, (size_t)dstsz,
        "Link        %s\n"
        "BoIP port   %d\n"
        "Mailbox     $9C00, %s\n"
        "Magic       %c%c, protocol %u\n"
        "\n"
        "SEQ         %02X\n"
        "ACKSEQ      %02X\n"
        "Device      %02X\n"
        "Command     %02X\n"
        "Params      %u\n"
        "TX length   %u\n"
        "\n"
        "Status      %s\n"
        "Error       %s\n"
        "Reply       %02X%s\n"
        "RX length   %u\n"
        "LINK cell   %u\n"
        "\n"
        "Boot        %s\n"
        "Boot path   %s\n",
        state < sizeof(link_names) / sizeof(link_names[0]) ? link_names[state] : "?",
        fn->port,
        fn->mailbox_active ? "active" : "off (the cart's memory map covers it)",
        m0 >= 0x20 && m0 < 0x7F ? (int)m0 : '.',
        m1 >= 0x20 && m1 < 0x7F ? (int)m1 : '.',
        (unsigned)MB(FUJI_MB_PROTO_VER),
        (unsigned)MB(FUJI_MB_SEQ), (unsigned)MB(FUJI_MB_ACKSEQ),
        (unsigned)MB(FUJI_MB_DEVICE), (unsigned)MB(FUJI_MB_CMD),
        (unsigned)MB(FUJI_MB_NPARAM),
        (unsigned)(MB(FUJI_MB_TXLEN_LO) | (MB(FUJI_MB_TXLEN_HI) << 8)),
        status < 4 ? status_names[status] : "?",
        err < sizeof(err_names) / sizeof(err_names[0]) ? err_names[err] : "?",
        reply, reply == FUJICMD_ACK ? " ACK" : reply == FUJICMD_NAK ? " NAK" : "",
        (unsigned)(MB(FUJI_MB_RXLEN_LO) | (MB(FUJI_MB_RXLEN_HI) << 8)),
        (unsigned)MB(FUJI_MB_LINK), boot_text,
        path[0] ? path : "(none)");
}
#undef MB
