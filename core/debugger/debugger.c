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
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* cfg/cfg.h's cfg_t embeds every peripheral's struct directly: see machine.h. */
#include "machine.h"
#include "cp1600/op_decode.h"
#include "cp1600/op_exec.h"

#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"
#include "intv_host.h"
#include "intvdebug.h"
#include "srcmap.h"

enum { LINE_NONE = 0, LINE_INTO, LINE_OVER, LINE_OUT };

struct intvdebug {
    int engaged;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int paused;      /* the emulator thread is blocked inside the hook; read and
                      * written with the helpers below, never plainly */
    int step_mode;   /* 1: step_count stays 1 (single-instruction ticks);
                      * 0: step_count is huge (only real breakpoints tick) */
    int line_mode;   /* LINE_*: a by-BASIC-line step is in flight; step_mode
                      * is 1 too, and the hook keeps going until a line starts */
    uint16_t line_r6;/* R6 when that step began */
    uint64_t stop_serial;   /* atomic, see paused */
    unsigned generation;    /* atomic: bumped by anything a UI should redraw for */

    /* A one-shot breakpoint (step over/out, run to) the current run armed;
     * any stop disarms it so it cannot fire later out of the blue. */
    int      oneshot_armed;
    uint16_t oneshot_addr;

    /* The breakpoint/watchpoint table. bp_lock guards it and the hit record
     * (the emulator thread takes it from the watch callback). */
    pthread_mutex_t bp_lock;
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    int      nbp;
    int      next_bp_id;
    unsigned bp_gen;
    int      watch_dirty;       /* the bus registration needs rebuilding */
    int      watch_hit;         /* a watchpoint fired: INTVDEBUG_STOP_WATCH_* */
    int      exec_hit_id;       /* the execute entry that fired (0: none, or a one-shot) */
    int      pause_req;         /* intvdebug_pause asked for this stop */
    intvdebug_stop_info hit;    /* the watchpoint that fired */
    intvdebug_stop_info last;   /* why the machine last stopped */

    /* The instruction history ring (see intvdebug.h). One writer: the
     * emulator thread. */
    int      hist_on;
    intvdebug_hist *hist;       /* INTVDEBUG_HISTORY_SIZE, allocated once, never freed */
    volatile uint64_t hist_count;

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

/* ---- the fields a UI thread reads without a lock ---------------------------- */

static int paused_now(const intvdebug *dbg)
{
    return __atomic_load_n(&dbg->paused, __ATOMIC_ACQUIRE);
}

static void paused_set(intvdebug *dbg, int value)
{
    __atomic_store_n(&dbg->paused, value, __ATOMIC_RELEASE);
}

static void bump(intvdebug *dbg)
{
    __atomic_fetch_add(&dbg->generation, 1u, __ATOMIC_RELEASE);
}

unsigned intvdebug_generation(const intvdebug *dbg)
{
    return __atomic_load_n(&dbg->generation, __ATOMIC_ACQUIRE);
}

/* ---- widths, and reading the bus without disturbing it ---------------------- */

int intvdebug_word_width(const intvdebug *dbg, uint16_t addr)
{
    if (addr >= 0x0100 && addr <= 0x01FF)
        return 8;
    if (addr >= 0x3000 && addr <= 0x3FFF)
        return 8;
    if (dbg && addr >= 0x4000 && addr <= 0x47FF && intv.ecs_enable > 0)
        return 8;   /* the ECS's RAM */
    return 16;
}

static uint16_t width_mask(uint16_t addr)
{
    return intvdebug_word_width(&g_debug, addr) == 8 ? 0x00FF : 0xFFFF;
}

static periph_t g_watch;        /* answers on the bins that hold a watchpoint */
static periph_bus_t *g_watch_bus;
static volatile int g_quiet;    /* a debugger poke is in flight: no watchpoint sees it */

/* What the devices on the bus would answer for a read at addr, asked of each
 * directly. periph_peek() would do the same but resets bus->busy and
 * bus->req, which is fatal in the middle of the access that is being
 * watched, so the watch hooks and the expressions come here. The ECS UART's
 * receive byte ($00E1) reads the tape file: it is never asked. */
static uint32_t bus_peek(periph_bus_t *bus, uint16_t addr)
{
    uint32_t data = bus->data_mask;
    const uint32_t bin = ((uint32_t)addr & bus->addr_mask) >> bus->decode_shift;
    int col;

    if (addr == 0x00E1)
        return 0;
    for (col = 0; col < MAX_PERIPH_BIN && bus->rd[col][bin]; col++) {
        periph_t *const dev = bus->rd[col][bin];
        if (dev == &g_watch || dev->busy || !dev->peek)
            continue;
        data &= dev->peek(dev, &bus->periph, ((uint32_t)addr - dev->addr_base) & dev->addr_mask, data);
    }
    return data;
}

static uint16_t machine_word(uint16_t addr)
{
    periph_bus_t *const bus = intv.intv;
    return bus ? (uint16_t)(bus_peek(bus, addr) & 0xFFFF) : 0xFFFF;
}

static uint32_t frame_of(uint64_t cycles)
{
    const uint64_t length = intv.stic.pal ? 19968u : 14934u;
    return (uint32_t)(cycles / length);
}

/* ---- expressions -------------------------------------------------------------- */

typedef struct {
    int check;      /* only validating: every known name answers 0, memory reads 0 */
    int hook;       /* on the emulator thread, inside a hook: the machine may be read */
    int access;     /* value/addr/old are defined */
    uint16_t value, addr, old, from_pc;
    int need_stop;  /* set when a name needed a stopped machine */
} evalenv;

static int env_readable(const evalenv *env)
{
    return env->check || env->hook || paused_now(&g_debug);
}

static int register_index(const char *name)
{
    if ((name[0] == 'r' || name[0] == 'R') && name[1] >= '0' && name[1] <= '7' && !name[2])
        return name[1] - '0';
    if (!strcasecmp(name, "sp"))
        return 6;
    if (!strcasecmp(name, "pc"))
        return 7;
    return -1;
}

static int expr_name(void *user, const char *name, long *out)
{
    evalenv *const env = user;
    cp1600_t *const cpu = &intv.cp1600;
    const int reg = register_index(name);
    uint16_t label;

    if (reg >= 0) {
        if (!env_readable(env)) {
            env->need_stop = 1;
            return 0;
        }
        *out = env->check ? 0
               : reg == 7 && env->access ? (long)env->from_pc : (long)cpu->r[reg];
        return 1;
    }
    if (name[1] == '\0' && strchr("szocidSZOCID", name[0])) {
        if (!env_readable(env)) {
            env->need_stop = 1;
            return 0;
        }
        switch (name[0] | 0x20) {
        case 's': *out = cpu->S ? 1 : 0; break;
        case 'z': *out = cpu->Z ? 1 : 0; break;
        case 'o': *out = cpu->O ? 1 : 0; break;
        case 'c': *out = cpu->C ? 1 : 0; break;
        case 'i': *out = cpu->I ? 1 : 0; break;
        default:  *out = cpu->D ? 1 : 0; break;
        }
        if (env->check)
            *out = 0;
        return 1;
    }
    if (!strcasecmp(name, "frame") || !strcasecmp(name, "cycles")) {
        if (!env_readable(env)) {
            env->need_stop = 1;
            return 0;
        }
        *out = env->check ? 0
               : !strcasecmp(name, "cycles") ? (long)cpu->periph.now : (long)frame_of(cpu->periph.now);
        return 1;
    }
    if (env->access && !strcasecmp(name, "value")) {
        *out = env->value;
        return 1;
    }
    if (env->access && !strcasecmp(name, "addr")) {
        *out = env->addr;
        return 1;
    }
    if (env->access && !strcasecmp(name, "old")) {
        *out = env->old;
        return 1;
    }
    if (env->check && (!strcasecmp(name, "value") || !strcasecmp(name, "addr") || !strcasecmp(name, "old"))) {
        *out = 0;
        return 1;
    }
    if (intvsymtab_lookup_name(intvsymtab_shared(), name, &label)) {
        *out = label;
        return 1;
    }
    return 0;
}

static uint16_t expr_read_word(void *user, uint16_t addr)
{
    evalenv *const env = user;
    if (env->check)
        return 0;
    if (!env_readable(env)) {
        env->need_stop = 1;
        return 0;
    }
    return machine_word(addr);
}

static int eval_env(evalenv *env, const char *text, long *out, char *err, int errsz)
{
    const intvexpr_ctx ctx = { expr_name, expr_read_word, env };
    int status = intvexpr_eval(text, &ctx, out, err, errsz);
    if (status != 0 && env->need_stop && err && errsz > 0)
        snprintf(err, (size_t)errsz, "the machine is running (stop it first)");
    if (status == 0 && env->need_stop) {
        if (err && errsz > 0)
            snprintf(err, (size_t)errsz, "the machine is running (stop it first)");
        status = -1;
    }
    return status;
}

int intvdebug_eval(intvdebug *dbg, const char *text, long *out, char *err, int errsz)
{
    evalenv env;
    (void)dbg;
    memset(&env, 0, sizeof(env));
    return eval_env(&env, text, out, err, errsz);
}

/* Does the condition parse, and name only things that exist? */
static int condition_parses(const char *text)
{
    evalenv env;
    long ignored;
    char err[64];
    if (!text || !text[0])
        return 1;
    memset(&env, 0, sizeof(env));
    env.check = 1;
    return eval_env(&env, text, &ignored, err, sizeof(err)) == 0;
}

/* Evaluates one entry's condition for an access (or, with access == 0, for an
 * execute stop). A condition that fails to evaluate stops rather than hides. */
static int condition_true(const intvdebug_bp *point, int access, uint16_t value, uint16_t old,
                          uint16_t addr, uint16_t from_pc)
{
    if (point->condition[0]) {
        evalenv env;
        long result = 0;
        char err[64];
        memset(&env, 0, sizeof(env));
        env.hook = 1;
        env.access = access;
        env.value = value;
        env.old = old;
        env.addr = addr;
        env.from_pc = from_pc;
        if (eval_env(&env, point->condition, &result, err, sizeof(err)) != 0)
            return 1;
        return result != 0;
    }
    if (access && point->cond != INTVDEBUG_COND_ANY) {
        const uint16_t mask = point->mask ? point->mask : 0xFFFF;
        if (point->cond == INTVDEBUG_COND_CHANGED)
            return (value & mask) != (old & mask);
        if (point->cond == INTVDEBUG_COND_EQ)
            return (value & mask) == (point->value & mask);
        if (point->cond == INTVDEBUG_COND_NE)
            return (value & mask) != (point->value & mask);
    }
    return 1;
}

static void watch_apply(intvdebug *dbg);
static int bp_find_exec(const intvdebug *dbg, uint16_t addr);

/* Strips what dasm1600 puts in front of the mnemonic: the instruction's own
 * address and its opcode words ("   $50CD  0004 0150 00D5   JSR  R5,$50D5"). */
static const char *dasm_body(const char *t)
{
    int words = 0, k;
    while (*t == ' ')
        t++;
    if (*t == '$') {
        while (*t && *t != ' ')
            t++;
        while (*t == ' ')
            t++;
    }
    while (words < 3) {
        for (k = 0; k < 4; k++)
            if (!((t[k] >= '0' && t[k] <= '9') || (t[k] >= 'A' && t[k] <= 'F') ||
                  (t[k] >= 'a' && t[k] <= 'f')))
                break;
        if (k < 4 || (t[4] != ' ' && t[4] != '\0'))
            break;
        t += 4;
        while (*t == ' ')
            t++;
        words++;
    }
    return t;
}

/* Records what the hook was just called for. A breakpoint stop (cycles < 0)
 * is an instruction that has not run, so it records nothing; an interrupt
 * entry records where the interrupted code was; the clock is the CPU's own
 * (periph.now), not tot_cycle, which only catches up at the end of a
 * timeslice. */
static void hist_record(intvdebug *dbg, int cycles)
{
    cp1600_t *const cpu = &intv.cp1600;
    const uint64_t count = dbg->hist_count;
    const unsigned ack = cpu->req_ack_state;
    intvdebug_hist *entry;
    int word;

    if (!dbg->hist || cycles < 0)
        return;
    entry = &dbg->hist[count & (INTVDEBUG_HISTORY_SIZE - 1)];
    if (ack & CP1600_INTAK) {
        entry->event = INTVDEBUG_EV_INTERRUPT;
        entry->pc = machine_word((uint16_t)(cpu->r[6] - 1));   /* the PC it just pushed */
    } else if (ack & CP1600_BUSAK) {
        entry->event = INTVDEBUG_EV_BUS;
        entry->pc = (uint16_t)cpu->r[7];
    } else if (cycles == 0) {
        return;   /* nothing ran: the reset that starts the machine */
    } else {
        entry->event = INTVDEBUG_EV_NONE;
        entry->pc = (uint16_t)cpu->oldpc;
    }
    for (word = 0; word < 3; word++)
        entry->w[word] = machine_word((uint16_t)(entry->pc + word));
    for (word = 0; word < 8; word++)
        entry->r[word] = cpu->r[word];
    entry->flags = (uint16_t)((cpu->S ? 1 : 0) | (cpu->C ? 2 : 0) | (cpu->O ? 4 : 0) | (cpu->Z ? 8 : 0) |
                              (cpu->I ? 16 : 0) | (cpu->D ? 32 : 0));
    entry->cycle = cpu->periph.now;
    entry->frame = frame_of(cpu->periph.now);
    dbg->hist_count = count + 1;
}

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

/* An execute breakpoint fired: does one of the entries at this PC want the
 * stop? A false condition lets the machine run on. Counts the hit and names
 * the entry. With no entry at all (a one-shot, or one just deleted) it stops. */
static int exec_stop_wanted(intvdebug *dbg)
{
    const uint16_t stop_pc = (uint16_t)intv.cp1600.r[7];
    int idx, found = 0, wanted = 0;

    pthread_mutex_lock(&dbg->bp_lock);
    dbg->exec_hit_id = 0;
    for (idx = 0; idx < dbg->nbp; idx++) {
        intvdebug_bp *const point = &dbg->bps[idx];
        if (!(point->type & INTVDEBUG_BP_EXEC) || point->lo != stop_pc || !point->enabled)
            continue;
        found = 1;
        if (condition_true(point, 0, 0, 0, stop_pc, stop_pc)) {
            wanted = 1;
            point->hits++;
            if (!dbg->exec_hit_id)
                dbg->exec_hit_id = point->id;
        }
    }
    if (wanted)
        dbg->bp_gen++;
    pthread_mutex_unlock(&dbg->bp_lock);
    return found ? wanted : 1;
}

static void oneshot_disarm(intvdebug *dbg)
{
    if (dbg->oneshot_armed)
        cp1600_clr_breakpt(&intv.cp1600, dbg->oneshot_addr, CP1600_BKPT_ONCE);
    dbg->oneshot_armed = 0;
}

static void oneshot_arm(intvdebug *dbg, uint16_t addr)
{
    oneshot_disarm(dbg);
    cp1600_set_breakpt(&intv.cp1600, addr, CP1600_BKPT_ONCE);
    dbg->oneshot_addr = addr;
    dbg->oneshot_armed = 1;
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
 * active (dbg->step_mode, which is exactly the flag intvdebug_pause/step set
 * to force every instruction to reach here). Anything else is a no-op
 * return, and free-running costs one extra periph_t-level call per
 * timeslice -- not per instruction. */
static uint32_t debug_tick(periph_t *req, uint32_t cycles)
{
    intvdebug *const dbg = &g_debug;
    cp1600_t *const cpu = &intv.cp1600;
    uint64_t serial;
    (void)req;

    if (dbg->hist_on)
        hist_record(dbg, (int)cycles);   /* the instruction that just ran, lock-free */

    pthread_mutex_lock(&dbg->lock);
    if (dbg->watch_dirty)
        watch_apply(dbg);   /* a watchpoint was added while running */
    if (cpu->hit_breakpoint == BK_NONE && !dbg->step_mode) {
        pthread_mutex_unlock(&dbg->lock);
        return 0;
    }

    /* An execute breakpoint whose condition is false is not a stop: the
     * instruction there has not run, so the next pass executes it. */
    if (cpu->hit_breakpoint == BK_BREAKPOINT && !dbg->watch_hit && !exec_stop_wanted(dbg)) {
        cpu->hit_breakpoint = BK_NONE;
        pthread_mutex_unlock(&dbg->lock);
        return 0;
    }

    /* A by-line step keeps single-stepping silently until the PC lands on
     * the first word of a BASIC line at an acceptable stack depth (a real
     * breakpoint still stops it, below). The stack grows upward on the
     * CP-1610, so a callee runs at a larger R6 than its caller. */
    if (dbg->line_mode != LINE_NONE && cpu->hit_breakpoint == BK_NONE) {
        const uint16_t stack_top = cpu->r[6];
        int stop = intvsrcmap_is_line_start(intvsrcmap_shared(), cpu->r[7]);
        if (stop && dbg->line_mode == LINE_OVER && stack_top > dbg->line_r6)
            stop = 0;
        if (stop && dbg->line_mode == LINE_OUT && stack_top >= dbg->line_r6)
            stop = 0;
        if (stop && dbg->line_mode == LINE_OVER && prev_was_call())
            stop = 0;
        if (!stop) {
            pthread_mutex_unlock(&dbg->lock);
            return 0;
        }
    }
    dbg->line_mode = LINE_NONE;
    oneshot_disarm(dbg);

    /* Why we are stopping, for the status line and the Breakpoints tab. The
     * reason, the PC, the serial and the paused flag are published together
     * under bp_lock, serial first: a poller that sees paused sees all of it. */
    pthread_mutex_lock(&dbg->bp_lock);
    memset(&dbg->last, 0, sizeof(dbg->last));
    if (dbg->watch_hit) {
        dbg->last = dbg->hit;
        dbg->last.reason = dbg->watch_hit;
        dbg->watch_hit = 0;
    } else if (cpu->hit_breakpoint != BK_NONE) {
        dbg->last.reason = INTVDEBUG_STOP_BREAKPOINT;
        if (cpu->hit_breakpoint == BK_BREAKPOINT)
            dbg->last.bp_id = dbg->exec_hit_id;
    } else if (dbg->pause_req) {
        dbg->last.reason = INTVDEBUG_STOP_PAUSE;
    } else {
        dbg->last.reason = INTVDEBUG_STOP_STEP;
    }
    dbg->exec_hit_id = 0;
    dbg->pause_req = 0;
    dbg->last.pc = (uint16_t)cpu->r[7];
    if (dbg->last.reason == INTVDEBUG_STOP_BREAKPOINT || dbg->last.reason == INTVDEBUG_STOP_STEP ||
        dbg->last.reason == INTVDEBUG_STOP_PAUSE) {
        dbg->last.addr = dbg->last.pc;
        dbg->last.from_pc = dbg->last.pc;
    }
    serial = __atomic_load_n(&dbg->stop_serial, __ATOMIC_RELAXED) + 1;
    dbg->last.serial = serial;
    __atomic_store_n(&dbg->stop_serial, serial, __ATOMIC_RELEASE);
    paused_set(dbg, 1);
    pthread_mutex_unlock(&dbg->bp_lock);
    bump(dbg);

    /* Clear it now, not just at the top of the next cp1600_run() call:
     * that call resets it once per invocation, not once per do_instr_tick
     * within it, and cp1600_run can call this hook several times per
     * invocation. Leaving a stale BK_BREAKPOINT/BK_TRACEPOINT here would
     * make every do_instr_tick for the rest of that invocation look like
     * another breakpoint hit, even after resuming. */
    cpu->hit_breakpoint = BK_NONE;
    while (paused_now(dbg))
        pthread_cond_wait(&dbg->cond, &dbg->lock);

    /* Set the run mode for what happens next, decided by whoever woke us
     * (intvdebug_resume clears step_mode, intvdebug_step/step_over/
     * step_out set or clear it as appropriate -- see each). */
    cpu->step_count = (dbg->step_mode || dbg->hist_on) ? 1 : ~0U;
    /* step_count only feeds steps_remaining when that reaches 0. Pausing out
     * of a free run leaves it at ~step_count minus whatever ran, so a Step
     * would run on until the next timeslice (or breakpoint) instead of one
     * instruction. Restart the countdown here, on the emulator thread:
     * 1 = stop after the next instruction, 0 = unlimited (cp1600_run then
     * refills it from step_count). */
    cpu->steps_remaining = (dbg->step_mode || dbg->hist_on) ? 1 : 0;
    pthread_mutex_unlock(&dbg->lock);

    return 0; /* NEVER CYC_MAX -- that aborts cp1600_run's whole timeslice. */
}

static void rearm_hook(void);

void intvdebug_set_engaged(intvdebug *dbg, int engaged)
{
    if (engaged == dbg->engaged)
        return;
    /* The first time anything engages, ask the host to call us back after
     * every cfg_init (a session restart). Not installed unless a debugger is
     * ever used, and Android never builds this file. */
    intv_host_set_post_init_hook(rearm_hook);

    if (engaged) {
        dbg->engaged = 1;
        cp1600_instr_tick(&intv.cp1600, debug_tick, NULL);
        if (dbg->hist_on)
            intv.cp1600.step_count = 1;
        /* A release left steps_remaining counting down from ~0U, so without
         * this a re-engaged tick would not fire for billions of instructions
         * and Pause/Step would lag after the window was closed and reopened.
         * A racy write, like intvdebug_pause's: at worst the tick lands one
         * instruction later. */
        intv.cp1600.steps_remaining = 1;
    } else {
        intvdebug_resume(dbg); /* never leave the emulator thread parked */
        dbg->engaged = 0;
        cp1600_instr_tick(&intv.cp1600, NULL, NULL);
        intv.cp1600.step_count = ~0U;
    }
}

int intvdebug_is_engaged(const intvdebug *dbg)
{
    return dbg->engaged;
}

void intvdebug_pause(intvdebug *dbg)
{
    if (!dbg->engaged)
        return;
    pthread_mutex_lock(&dbg->lock);
    dbg->step_mode = 1;
    dbg->pause_req = 1;
    dbg->line_mode = LINE_NONE; /* a pause must stop a by-line step too */
    pthread_mutex_unlock(&dbg->lock);
    /* Racing a plain int write against the emulator thread's read of
     * step_count is deliberate and harmless here: the worst case is the
     * pause landing one instruction later than requested, never
     * corruption (see intvdebug.h's threading note). */
    intv.cp1600.step_count = 1;
}

void intvdebug_resume(intvdebug *dbg)
{
    pthread_mutex_lock(&dbg->lock);
    if (paused_now(dbg)) {
        dbg->step_mode = 0;
        paused_set(dbg, 0);
        pthread_cond_broadcast(&dbg->cond);
    }
    pthread_mutex_unlock(&dbg->lock);
    bump(dbg);
}

int intvdebug_is_paused(const intvdebug *dbg)
{
    return paused_now(dbg);
}

/* Stopping at an address a breakpoint is armed on (a step or a by-line stop
 * lands there without the breakpoint having fired) must not make the next
 * run fire it again before executing anything. cp1600_set_breakpt on the
 * current PC arms it for the instruction after this one instead. Only while
 * paused. */
static void step_off_breakpoint(intvdebug *dbg)
{
    const uint16_t here = intv.cp1600.r[7];
    if (intvdebug_breakpoint_is_set(dbg, here))
        cp1600_set_breakpt(&intv.cp1600, here, CP1600_BKPT);
}

void intvdebug_step(intvdebug *dbg)
{
    if (paused_now(dbg))
        step_off_breakpoint(dbg);
    pthread_mutex_lock(&dbg->lock);
    if (paused_now(dbg)) {
        dbg->step_mode = 1;
        paused_set(dbg, 0);
        pthread_cond_broadcast(&dbg->cond);
    }
    pthread_mutex_unlock(&dbg->lock);
    bump(dbg);
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

/* Runs free until the one-shot breakpoint (or a real one) stops it. */
static void run_free(intvdebug *dbg)
{
    pthread_mutex_lock(&dbg->lock);
    dbg->step_mode = 0;
    paused_set(dbg, 0);
    pthread_cond_broadcast(&dbg->cond);
    pthread_mutex_unlock(&dbg->lock);
    bump(dbg);
}

void intvdebug_step_over(intvdebug *dbg)
{
    intvdebug_dasm_line line;

    if (!paused_now(dbg))
        return;
    step_off_breakpoint(dbg);
    if (intvdebug_disassemble(dbg, intv.cp1600.r[7], intv.cp1600.D, &line, 1) != 1 ||
        !is_call_mnemonic(line.text)) {
        intvdebug_step(dbg);
        return;
    }

    oneshot_arm(dbg, (uint16_t)(intv.cp1600.r[7] + line.length));
    run_free(dbg);
}

void intvdebug_step_out(intvdebug *dbg)
{
    if (!paused_now(dbg))
        return;
    step_off_breakpoint(dbg);

    /* Best-effort; see intvdebug.h's own comment on why R5 and why this
     * can silently fail to stop rather than stop somewhere wrong. */
    oneshot_arm(dbg, (uint16_t)intv.cp1600.r[5]);
    run_free(dbg);
}

void intvdebug_run_to(intvdebug *dbg, uint16_t addr)
{
    if (!paused_now(dbg))
        return;
    step_off_breakpoint(dbg);
    oneshot_arm(dbg, addr);
    run_free(dbg);
}

static void begin_line_step(intvdebug *dbg, int mode)
{
    if (paused_now(dbg))
        step_off_breakpoint(dbg);
    pthread_mutex_lock(&dbg->lock);
    if (paused_now(dbg)) {
        dbg->line_r6 = intv.cp1600.r[6];
        /* Stopped on a callee's first word, straight after its JSR: the
         * PSHR R5 that opens an IntyBASIC procedure hasn't run, so the
         * stack is one shallower than the body will be. Count the body as
         * this routine's own level, or Over would skip all of it and Out
         * would not stop until the caller's caller. */
        if (mode != LINE_INTO && prev_was_call())
            dbg->line_r6++;
        dbg->line_mode = mode;
        dbg->step_mode = 1;
        paused_set(dbg, 0);
        pthread_cond_broadcast(&dbg->cond);
    }
    pthread_mutex_unlock(&dbg->lock);
    bump(dbg);
}

void intvdebug_step_line(intvdebug *dbg)
{
    begin_line_step(dbg, LINE_INTO);
}

void intvdebug_step_line_over(intvdebug *dbg)
{
    begin_line_step(dbg, LINE_OVER);
}

void intvdebug_step_line_out(intvdebug *dbg)
{
    begin_line_step(dbg, LINE_OUT);
}

uint64_t intvdebug_stop_serial(const intvdebug *dbg)
{
    return __atomic_load_n(&dbg->stop_serial, __ATOMIC_ACQUIRE);
}

void intvdebug_touch(intvdebug *dbg)
{
    bump(dbg);
}

int intvdebug_regs_get(intvdebug *dbg, intvdebug_regs *out)
{
    cp1600_t *const cpu = &intv.cp1600;

    if (!paused_now(dbg))
        return 0;

    memcpy(out->r, cpu->r, sizeof(out->r));
    out->pc = cpu->r[7];
    out->S = cpu->S;
    out->C = cpu->C;
    out->O = cpu->O;
    out->Z = cpu->Z;
    out->I = cpu->I;
    out->D = cpu->D;
    out->interruptible = cpu->I && !cpu->D;
    out->intrq = (cpu->req_ack_state & CP1600_INTRQ) != 0;
    out->busrq = (cpu->req_ack_state & CP1600_BUSRQ) != 0;
    out->cycles = cpu->periph.now;
    out->frame = frame_of(cpu->periph.now);
    return 1;
}

int intvdebug_regs_set(intvdebug *dbg, int reg, unsigned value)
{
    cp1600_t *const cpu = &intv.cp1600;

    if (!paused_now(dbg) || reg < 0 || reg >= INTVDEBUG_REG_COUNT)
        return 0;
    switch (reg) {
    case INTVDEBUG_REG_S: cpu->S = value ? 1 : 0; break;
    case INTVDEBUG_REG_Z: cpu->Z = value ? 1 : 0; break;
    case INTVDEBUG_REG_O: cpu->O = value ? 1 : 0; break;
    case INTVDEBUG_REG_C: cpu->C = value ? 1 : 0; break;
    case INTVDEBUG_REG_I: cpu->I = value ? 1 : 0; break;
    case INTVDEBUG_REG_D: cpu->D = value ? 1 : 0; break;   /* 1: SDBD pending for the next instruction */
    default:
        cpu->r[reg] = (uint16_t)(value & 0xFFFF);
        /* Landing on an armed breakpoint by hand is not a hit: the next run
         * executes that instruction, as it would after a Step. */
        if (reg == INTVDEBUG_REG_PC)
            step_off_breakpoint(dbg);
        break;
    }
    bump(dbg);
    return 1;
}

int intvdebug_read(intvdebug *dbg, uint16_t addr, uint16_t *dst, int count)
{
    cp1600_t *const cpu = &intv.cp1600; /* CP1600_PK/_WR substitute their
                                         * argument textually into `c->...`,
                                         * so &intv.cp1600 (an expression, not
                                         * a plain identifier) would parse as
                                         * &(intv.cp1600->...) -- wrong
                                         * precedence. A local pointer var is
                                         * what every other CP1600_* call site
                                         * in the staged tree uses too. */
    int idx;
    if (!paused_now(dbg))
        return 0;
    for (idx = 0; idx < count; idx++) {
        const uint16_t where = (uint16_t)(addr + idx);
        /* the ECS UART's receive byte reads the tape file */
        dst[idx] = where == 0x00E1 ? 0 : (uint16_t)CP1600_PK(cpu, where);
    }
    return count;
}

int intvdebug_write(intvdebug *dbg, uint16_t addr, const uint16_t *src, int count)
{
    cp1600_t *const cpu = &intv.cp1600;
    int idx;
    if (!paused_now(dbg))
        return 0;
    g_quiet = 1;   /* a poke is not an access the watchpoints should see */
    for (idx = 0; idx < count; idx++)
        CP1600_WR(cpu, (uint16_t)(addr + idx), src[idx]);
    g_quiet = 0;
    bump(dbg);
    return count;
}

static uint16_t fold_ascii(uint16_t w)
{
    const uint16_t c = w & 0xFF;
    return c >= 'a' && c <= 'z' ? (uint16_t)((w & 0xFF00) | (c - 32)) : w;
}

int intvdebug_search(intvdebug *dbg, const uint16_t *pattern, int n, uint16_t mask, int flags,
                     uint16_t from, uint16_t shown, uint16_t *out, int max)
{
    uint16_t buf[512 + 64];
    uint32_t base, last = shown;
    int found = 0, i;

    if (!paused_now(dbg))
        return -1;
    if (n < 1 || n > 64 || shown < from)
        return 0;
    for (base = from; base <= last && found < max; base += 512) {
        /* a chunk and enough words past it for a match that straddles the end */
        const int want = (int)(last - base + 1) < 512 + n - 1 ? (int)(last - base + 1) : 512 + n - 1;
        int got = intvdebug_read(dbg, (uint16_t)base, buf, want);
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

int intvdebug_disassemble(intvdebug *dbg, uint16_t addr, int dbd,
                          intvdebug_dasm_line *out, int max)
{
    cp1600_t *const cp = &intv.cp1600;
    int produced = 0;

    if (!paused_now(dbg))
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
        snprintf(out[produced].text, sizeof(out[produced].text), "%s", dasm_body(buf));

        addr = (uint16_t)(addr + len);
        dbd = 0; /* DBD state is only known at the actual current PC */
        produced++;
    }
    return produced;
}

/* ---- the breakpoint table ------------------------------------------------ */

/* One 16-word bin of the bus is the grain of address decode. */
#define WATCH_BINS 4096

static int bp_find(const intvdebug *dbg, int id)
{
    int idx;
    for (idx = 0; idx < dbg->nbp; idx++)
        if (dbg->bps[idx].id == id)
            return idx;
    return -1;
}

static int bp_find_exec(const intvdebug *dbg, uint16_t addr)
{
    int idx;
    for (idx = 0; idx < dbg->nbp; idx++)
        if ((dbg->bps[idx].type & INTVDEBUG_BP_EXEC) && dbg->bps[idx].lo == addr)
            return idx;
    return -1;
}

/* Arms or clears jzIntv's own breakpoint for an EXEC entry. Safe while
 * running (the long-standing behaviour of the toggle). Arming at the PC the
 * CPU is stopped on arms it for the instruction after this one. */
static void bp_arm_exec(const intvdebug_bp *point)
{
    if (point->enabled)
        cp1600_set_breakpt(&intv.cp1600, point->lo, CP1600_BKPT);
    else
        cp1600_clr_breakpt(&intv.cp1600, point->lo, CP1600_BKPT);
}

/* Arms an EXEC entry so that it fires the first time the PC gets there, even
 * when the PC happens to be there now. A freshly built CPU sits at the reset
 * vector ($1000) before it has run anything, and an entry armed there with
 * bp_arm_exec would be skipped once, as if the machine were stopped on it. */
static void bp_arm_exec_fresh(const intvdebug_bp *point)
{
    cp1600_t *const cpu = &intv.cp1600;
    const uint16_t saved = cpu->r[7];
    cpu->r[7] = (uint16_t)(point->lo ^ 0xFFFFu);   /* anywhere but the breakpoint */
    bp_arm_exec(point);
    cpu->r[7] = saved;
}

static uint32_t watch_rd(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data);
static void watch_wr(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data);

static uint32_t watch_peek(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)dev; (void)bus; (void)addr; (void)data;
    return ~0U;   /* a peek must not be seen, and must not drive the bus */
}

static void watch_poke(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)dev; (void)bus; (void)addr; (void)data;
}

/* Puts the watch device first in every bin it answers, so it runs before the
 * store or the read it is watching: `old` is then read from the devices as
 * they were, whatever order the cartridge (re)registered in. */
static void watch_to_front(periph_bus_t *bus, const uint8_t *bins)
{
    uint32_t bin;
    int col, shift;

    for (bin = 0; bin < WATCH_BINS; bin++) {
        if (!bins[bin])
            continue;
        for (col = 1; col < MAX_PERIPH_BIN && bus->rd[col][bin]; col++)
            if (bus->rd[col][bin] == &g_watch) {
                for (shift = col; shift > 0; shift--)
                    bus->rd[shift][bin] = bus->rd[shift - 1][bin];
                bus->rd[0][bin] = &g_watch;
                break;
            }
        for (col = 1; col < MAX_PERIPH_BIN && bus->wr[col][bin]; col++)
            if (bus->wr[col][bin] == &g_watch) {
                for (shift = col; shift > 0; shift--)
                    bus->wr[shift][bin] = bus->wr[shift - 1][bin];
                bus->wr[0][bin] = &g_watch;
                break;
            }
    }
}

/* Rebuilds which bins our device answers on. Only while the emulator thread
 * is parked or is the caller (the bins are walked by every bus access). */
static void watch_apply(intvdebug *dbg)
{
    static uint8_t bins[WATCH_BINS];
    periph_bus_t *bus = intv.intv;
    int idx, bin, any = 0;

    pthread_mutex_lock(&dbg->bp_lock);
    dbg->watch_dirty = 0;
    memset(bins, 0, sizeof(bins));
    for (idx = 0; idx < dbg->nbp; idx++) {
        const intvdebug_bp *point = &dbg->bps[idx];
        if (!point->enabled || !(point->type & (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE)))
            continue;
        for (bin = point->lo >> 4; bin <= point->hi >> 4; bin++)
            bins[bin] = 1;
        any = 1;
    }
    pthread_mutex_unlock(&dbg->bp_lock);

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
    for (bin = 0; bin < WATCH_BINS; bin++) {   /* one registration per run of bins */
        int last = bin;
        if (!bins[bin])
            continue;
        while (last + 1 < WATCH_BINS && bins[last + 1])
            last++;
        periph_register(bus, &g_watch, (uint32_t)bin << 4, ((uint32_t)last << 4) | 15, "watch");
        bin = last;
    }
    watch_to_front(bus, bins);
}

/* jzIntv's reload path (jzintv.c `reload:`) tears the machine down and runs
 * cfg_init() again, which rebuilds the CPU and the bus: our instruction tick,
 * every EXEC breakpoint and the watch device registration are gone, while
 * `engaged` still says 1 (so set_engaged(1) would be a no-op). Put them back
 * from the tables. Runs on the emulator thread, right after cfg_init, via the
 * hook the jzIntv patch adds; also callable by a host that rebuilt the machine
 * some other way. */
void intvdebug_rearm(intvdebug *dbg)
{
    int idx;

    if (dbg->engaged) {
        cp1600_instr_tick(&intv.cp1600, debug_tick, NULL);
        intv.cp1600.step_count = (dbg->hist_on || dbg->step_mode) ? 1 : ~0U;
        intv.cp1600.steps_remaining = (dbg->hist_on || dbg->step_mode) ? 1 : 0;
    }
    dbg->line_mode = LINE_NONE;      /* a by-line step cannot survive a reboot */
    dbg->oneshot_armed = 0;          /* the CPU that held it is gone */
    dbg->hist_count = 0;             /* the old instructions belong to a dead machine */

    pthread_mutex_lock(&dbg->bp_lock);
    for (idx = 0; idx < dbg->nbp; idx++)
        if (dbg->bps[idx].type & INTVDEBUG_BP_EXEC)
            bp_arm_exec_fresh(&dbg->bps[idx]);
    pthread_mutex_unlock(&dbg->bp_lock);

    g_watch_bus = NULL;            /* the old bus is freed; forget the registration */
    watch_apply(dbg);
    bump(dbg);
}

static void rearm_hook(void)
{
    intvdebug_rearm(&g_debug);
}

/* The decoder reads an instruction's words through the bus like any read when
 * the page is not cached (cartridge RAM, ECS/JLP RAM), and when a breakpoint
 * sits on the instruction. Those are fetches, not data accesses: they sit
 * within the instruction's own three words while its execute slot still holds
 * a decode function. A data read of the instruction's own neighbourhood by an
 * uncached instruction is indistinguishable, and is dropped with them. */
static int is_decode_fetch(uint16_t addr)
{
    cp1600_t *const cpu = &intv.cp1600;
    const uint16_t offset = (uint16_t)(addr - cpu->oldpc);
    cp1600_ins_t *slot;

    if (offset > 2)
        return 0;
    slot = cpu->execute[cpu->oldpc];
    return slot == fn_decode || slot == fn_decode_1st || slot == fn_decode_bkpt || slot == fn_breakpt;
}

/* Called on the emulator thread for each CPU data access on a watched bin,
 * before the access itself (the watch device is first on the bus). */
static void watch_hit(periph_t *bus, uint32_t addr, uint32_t data, int is_write)
{
    intvdebug *const dbg = &g_debug;
    cp1600_t *const cpu = &intv.cp1600;
    const uint16_t where = (uint16_t)addr;
    const int kind = is_write ? INTVDEBUG_BP_WRITE : INTVDEBUG_BP_READ;
    uint16_t value = 0, old = 0;
    int idx, known = 0;

    if (g_quiet || !dbg->engaged || !bus || bus->req != &cpu->periph)
        return;   /* only the CPU's own accesses, and only while engaged */
    if (!is_write && is_decode_fetch(where))
        return;
    pthread_mutex_lock(&dbg->bp_lock);
    for (idx = 0; idx < dbg->nbp; idx++) {
        intvdebug_bp *const point = &dbg->bps[idx];

        if (!point->enabled || !(point->type & kind) || where < point->lo || where > point->hi)
            continue;
        if (!known) {   /* the word as it is now: before the store, or what the read returns */
            const uint16_t mask = width_mask(where);
            old = (uint16_t)(bus_peek((periph_bus_t *)bus, where) & mask);
            value = is_write ? (uint16_t)(data & mask) : old;
            known = 1;
        }
        if (!condition_true(point, 1, value, old, where, (uint16_t)cpu->oldpc))
            continue;
        point->hits++;
        dbg->bp_gen++;
        if (!dbg->watch_hit) {   /* the first hit of the instruction names the stop */
            dbg->watch_hit = is_write ? INTVDEBUG_STOP_WATCH_WRITE : INTVDEBUG_STOP_WATCH_READ;
            dbg->hit.bp_id = point->id;
            dbg->hit.addr = where;
            dbg->hit.value = value;
            dbg->hit.old_value = old;
            dbg->hit.from_pc = (uint16_t)cpu->oldpc;
        }
    }
    if (dbg->watch_hit) {
        cpu->hit_breakpoint = BK_BREAKPOINT;
        cpu->steps_remaining = 1;   /* stop after this instruction */
    }
    pthread_mutex_unlock(&dbg->bp_lock);
}

static uint32_t watch_rd(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)dev; (void)data;
    watch_hit(bus, addr, 0, 0);
    return ~0U;   /* merely listening: leave the bus value alone */
}

static void watch_wr(periph_t *dev, periph_t *bus, uint32_t addr, uint32_t data)
{
    (void)dev;
    watch_hit(bus, addr, data, 1);
}

/* A table change: apply the bus side now if the machine is parked, else the
 * emulator thread does it at its next hook. */
static void bp_changed(intvdebug *dbg, int bus_side)
{
    dbg->bp_gen++;
    bump(dbg);
    if (!bus_side)
        return;
    if (paused_now(dbg))
        watch_apply(dbg);
    else
        dbg->watch_dirty = 1;
}

int intvdebug_bp_add(intvdebug *dbg, const intvdebug_bp *spec)
{
    intvdebug_bp point;
    int id;

    if (!spec || !(spec->type & (INTVDEBUG_BP_EXEC | INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE)))
        return -1;
    point = *spec;
    point.condition[sizeof(point.condition) - 1] = '\0';
    if (!condition_parses(point.condition))
        return -1;
    pthread_mutex_lock(&dbg->bp_lock);
    if (dbg->nbp >= INTVDEBUG_MAX_BREAKPOINTS) {
        pthread_mutex_unlock(&dbg->bp_lock);
        return -1;
    }
    if (point.type & INTVDEBUG_BP_EXEC) {
        point.type = INTVDEBUG_BP_EXEC;   /* code stops are exact addresses */
        point.hi = point.lo;
        point.cond = INTVDEBUG_COND_ANY;
    } else if (point.hi < point.lo) {
        const uint16_t swap = point.lo;
        point.lo = point.hi;
        point.hi = swap;
    }
    if (!(point.type & INTVDEBUG_BP_WRITE) || point.condition[0])
        point.cond = INTVDEBUG_COND_ANY;
    point.id = id = dbg->next_bp_id++;
    point.enabled = 1;
    point.hits = 0;
    dbg->bps[dbg->nbp++] = point;
    pthread_mutex_unlock(&dbg->bp_lock);

    if (point.type & INTVDEBUG_BP_EXEC)
        bp_arm_exec(&point);
    bp_changed(dbg, !(point.type & INTVDEBUG_BP_EXEC));
    return id;
}

int intvdebug_bp_set_condition(intvdebug *dbg, int id, const char *condition)
{
    int idx;

    if (!condition)
        condition = "";
    if (strlen(condition) >= sizeof(dbg->bps[0].condition) || !condition_parses(condition))
        return 0;
    pthread_mutex_lock(&dbg->bp_lock);
    idx = bp_find(dbg, id);
    if (idx < 0) {
        pthread_mutex_unlock(&dbg->bp_lock);
        return 0;
    }
    snprintf(dbg->bps[idx].condition, sizeof(dbg->bps[idx].condition), "%s", condition);
    dbg->bps[idx].cond = INTVDEBUG_COND_ANY;
    pthread_mutex_unlock(&dbg->bp_lock);
    bp_changed(dbg, 0);
    return 1;
}

int intvdebug_bp_remove(intvdebug *dbg, int id)
{
    intvdebug_bp gone;
    int idx, bus_side;

    pthread_mutex_lock(&dbg->bp_lock);
    idx = bp_find(dbg, id);
    if (idx < 0) {
        pthread_mutex_unlock(&dbg->bp_lock);
        return 0;
    }
    gone = dbg->bps[idx];
    memmove(&dbg->bps[idx], &dbg->bps[idx + 1], (size_t)(dbg->nbp - idx - 1) * sizeof(dbg->bps[0]));
    dbg->nbp--;
    pthread_mutex_unlock(&dbg->bp_lock);

    bus_side = !(gone.type & INTVDEBUG_BP_EXEC);
    if (gone.type & INTVDEBUG_BP_EXEC) {
        gone.enabled = 0;
        if (bp_find_exec(dbg, gone.lo) < 0)
            bp_arm_exec(&gone);
    }
    bp_changed(dbg, bus_side);
    return 1;
}

int intvdebug_bp_enable(intvdebug *dbg, int id, int enabled)
{
    intvdebug_bp point;
    int idx;

    pthread_mutex_lock(&dbg->bp_lock);
    idx = bp_find(dbg, id);
    if (idx < 0) {
        pthread_mutex_unlock(&dbg->bp_lock);
        return 0;
    }
    dbg->bps[idx].enabled = enabled ? 1 : 0;
    point = dbg->bps[idx];
    pthread_mutex_unlock(&dbg->bp_lock);
    if (point.type & INTVDEBUG_BP_EXEC)
        bp_arm_exec(&point);
    bp_changed(dbg, !(point.type & INTVDEBUG_BP_EXEC));
    return 1;
}

int intvdebug_bp_clear_hits(intvdebug *dbg)
{
    int idx;
    pthread_mutex_lock(&dbg->bp_lock);
    for (idx = 0; idx < dbg->nbp; idx++)
        dbg->bps[idx].hits = 0;
    dbg->bp_gen++;
    pthread_mutex_unlock(&dbg->bp_lock);
    bump(dbg);
    return 1;
}

int intvdebug_bp_list(intvdebug *dbg, intvdebug_bp *out, int max)
{
    int count;
    pthread_mutex_lock(&dbg->bp_lock);
    count = dbg->nbp < max ? dbg->nbp : max;
    memcpy(out, dbg->bps, (size_t)count * sizeof(out[0]));
    pthread_mutex_unlock(&dbg->bp_lock);
    return count;
}

unsigned intvdebug_bp_generation(const intvdebug *dbg)
{
    return dbg->bp_gen;
}

int intvdebug_breakpoint_toggle(intvdebug *dbg, uint16_t addr)
{
    int idx;
    intvdebug_bp spec;

    pthread_mutex_lock(&dbg->bp_lock);
    idx = bp_find_exec(dbg, addr);
    if (idx >= 0) {
        const int id = dbg->bps[idx].id;
        pthread_mutex_unlock(&dbg->bp_lock);
        intvdebug_bp_remove(dbg, id);
        return 0;
    }
    pthread_mutex_unlock(&dbg->bp_lock);
    memset(&spec, 0, sizeof(spec));
    spec.type = INTVDEBUG_BP_EXEC;
    spec.lo = spec.hi = addr;
    return intvdebug_bp_add(dbg, &spec) < 0 ? -1 : 1;
}

int intvdebug_breakpoint_state(intvdebug *dbg, uint16_t addr)
{
    int idx, state = 0;
    pthread_mutex_lock(&dbg->bp_lock);
    idx = bp_find_exec(dbg, addr);
    if (idx >= 0)
        state = dbg->bps[idx].enabled ? 1 : 2;
    pthread_mutex_unlock(&dbg->bp_lock);
    return state;
}

int intvdebug_breakpoint_is_set(intvdebug *dbg, uint16_t addr)
{
    return intvdebug_breakpoint_state(dbg, addr) == 1;
}

void intvdebug_breakpoint_clear_all(intvdebug *dbg)
{
    int idx;
    pthread_mutex_lock(&dbg->bp_lock);
    for (idx = 0; idx < dbg->nbp; idx++)
        if (dbg->bps[idx].type & INTVDEBUG_BP_EXEC)
            cp1600_clr_breakpt(&intv.cp1600, dbg->bps[idx].lo, CP1600_BKPT);
    dbg->nbp = 0;
    pthread_mutex_unlock(&dbg->bp_lock);
    bp_changed(dbg, 1);
}

int intvdebug_breakpoint_list(intvdebug *dbg, uint16_t *out, int max)
{
    int idx, count = 0;
    pthread_mutex_lock(&dbg->bp_lock);
    for (idx = 0; idx < dbg->nbp && count < max; idx++)
        if ((dbg->bps[idx].type & INTVDEBUG_BP_EXEC) && dbg->bps[idx].enabled) {
            int slot = count++;
            while (slot > 0 && out[slot - 1] > dbg->bps[idx].lo) { /* ascending */
                out[slot] = out[slot - 1];
                slot--;
            }
            out[slot] = dbg->bps[idx].lo;
        }
    pthread_mutex_unlock(&dbg->bp_lock);
    return count;
}

int intvdebug_last_stop(intvdebug *dbg, intvdebug_stop_info *out)
{
    pthread_mutex_lock(&dbg->bp_lock);
    *out = dbg->last;
    pthread_mutex_unlock(&dbg->bp_lock);
    return out->reason != INTVDEBUG_STOP_NONE;
}

/* ---- why it stopped ------------------------------------------------------------- */

static int describe_stop(const intvdebug *dbg, const intvdebug_stop_info *stop, char *dst, int dstsz)
{
    const int wide = intvdebug_word_width(dbg, stop->addr) == 16;

    switch (stop->reason) {
    case INTVDEBUG_STOP_PAUSE:
        return snprintf(dst, (size_t)dstsz, "paused at $%04X", stop->pc);
    case INTVDEBUG_STOP_STEP:
        return snprintf(dst, (size_t)dstsz, "step at $%04X", stop->pc);
    case INTVDEBUG_STOP_BREAKPOINT:
        if (stop->bp_id > 0)
            return snprintf(dst, (size_t)dstsz, "breakpoint: exec $%04X", stop->pc);
        return snprintf(dst, (size_t)dstsz, "stopped at $%04X", stop->pc);
    case INTVDEBUG_STOP_WATCH_READ:
        return snprintf(dst, (size_t)dstsz, wide ? "read $%04X = $%04X at $%04X" : "read $%04X = $%02X at $%04X",
                        stop->addr, stop->value, stop->from_pc);
    case INTVDEBUG_STOP_WATCH_WRITE:
        return snprintf(dst, (size_t)dstsz,
                        wide ? "wrote $%04X = $%04X (was $%04X) at $%04X"
                             : "wrote $%04X = $%02X (was $%02X) at $%04X",
                        stop->addr, stop->value, stop->old_value, stop->from_pc);
    default:
        return snprintf(dst, (size_t)dstsz, "stopped at $%04X", stop->pc);
    }
}

int intvdebug_stop_text(intvdebug *dbg, char *dst, int dstsz, int *address)
{
    intvdebug_snapshot snap;
    int len;

    if (!dst || dstsz <= 0)
        return 0;
    intvdebug_stop_snapshot(dbg, &snap);
    len = (int)strlen(snap.text);
    snprintf(dst, (size_t)dstsz, "%s", snap.text);
    if (address)
        *address = snap.address;
    return len < dstsz ? len : dstsz - 1;
}

int intvdebug_stop_snapshot(intvdebug *dbg, intvdebug_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&dbg->lock);
    pthread_mutex_lock(&dbg->bp_lock);
    out->paused = paused_now(dbg);
    out->serial = __atomic_load_n(&dbg->stop_serial, __ATOMIC_ACQUIRE);
    out->stop = dbg->last;
    pthread_mutex_unlock(&dbg->bp_lock);
    pthread_mutex_unlock(&dbg->lock);
    if (out->paused && out->stop.reason != INTVDEBUG_STOP_NONE) {
        describe_stop(dbg, &out->stop, out->text, (int)sizeof(out->text));
        out->address = out->stop.pc;
    } else {
        snprintf(out->text, sizeof(out->text), "running");
        out->address = -1;
    }
    return out->paused;
}

/* ---- history ---------------------------------------------------------------- */

void intvdebug_history_enable(intvdebug *dbg, int on)
{
    if (on && !dbg->hist)
        dbg->hist = calloc(INTVDEBUG_HISTORY_SIZE, sizeof(intvdebug_hist));
    if (on && !dbg->hist)
        return;
    dbg->hist_on = on ? 1 : 0;
    if (dbg->engaged) {
        /* every instruction must reach the hook while it records */
        intv.cp1600.step_count = (on || dbg->step_mode) ? 1 : ~0U;
    }
}

int intvdebug_history_enabled(const intvdebug *dbg)
{
    return dbg->hist_on;
}

void intvdebug_history_clear(intvdebug *dbg)
{
    dbg->hist_count = 0;
}

uint64_t intvdebug_history_count(const intvdebug *dbg)
{
    return dbg->hist_count;
}

int intvdebug_history_copy(intvdebug *dbg, intvdebug_hist *out, int max)
{
    const uint64_t total = dbg->hist_count;
    uint64_t have = total < INTVDEBUG_HISTORY_SIZE ? total : INTVDEBUG_HISTORY_SIZE, first;
    int count, idx;

    if (!dbg->hist || max < 1)
        return 0;
    count = have < (uint64_t)max ? (int)have : max;
    first = total - (uint64_t)count;
    for (idx = 0; idx < count; idx++)
        out[idx] = dbg->hist[(first + (uint64_t)idx) & (INTVDEBUG_HISTORY_SIZE - 1)];
    return count;
}

static int hist_text(const intvdebug_hist *h, char *dst, int dstsz, int dbd, int *length)
{
    char buf[64];
    const int words = dasm1600(buf, h->pc, dbd, h->w[0], h->w[1], h->w[2]);
    if (length)
        *length = words < 1 ? 1 : words;
    snprintf(dst, (size_t)dstsz, "%s", dasm_body(buf));
    return (int)strlen(dst);
}

int intvdebug_history_format(const intvdebug_hist *h, char *dst, int dstsz)
{
    char text[64];
    if (h->event == INTVDEBUG_EV_INTERRUPT)
        return snprintf(dst, (size_t)dstsz, "%04X  (interrupt)", h->pc);
    if (h->event == INTVDEBUG_EV_BUS)
        return snprintf(dst, (size_t)dstsz, "%04X  (bus grant)", h->pc);
    hist_text(h, text, sizeof(text), 0, NULL);
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-22s R0 %04X R1 %04X R2 %04X R3 %04X R4 %04X R5 %04X R6 %04X  S%d C%d O%d Z%d I%d",
                    h->pc, text, h->r[0], h->r[1], h->r[2], h->r[3], h->r[4], h->r[5], h->r[6],
                    h->flags & 1, (h->flags >> 1) & 1, (h->flags >> 2) & 1, (h->flags >> 3) & 1,
                    (h->flags >> 4) & 1);
}

/* A branch, jump or call by what it did, not by its name: anything that left
 * the PC somewhere other than the next instruction (PULR R7, MVI@ Rx,R7,
 * MVII ..,R7, ADDR/INCR R7 included), plus the conditional branches that
 * fell through. An immediate after SDBD is one word longer, so both lengths
 * count as falling through. */
static int hist_is_branch(const intvdebug_hist *h)
{
    char text[64];
    int plain, doubled;

    hist_text(h, text, sizeof(text), 0, &plain);
    hist_text(h, text, sizeof(text), 1, &doubled);
    if (h->r[7] != (uint16_t)(h->pc + plain) && h->r[7] != (uint16_t)(h->pc + doubled))
        return 1;
    hist_text(h, text, sizeof(text), 0, NULL);
    return text[0] == 'B' || text[0] == 'J';   /* B, BC, BEXT, ... J, JR, JSR ... not taken */
}

int intvdebug_history_match(const intvdebug_hist *h, int filter, uint16_t lo, uint16_t hi)
{
    if ((filter & INTVDEBUG_HIST_RANGE) && (h->pc < lo || h->pc > hi))
        return 0;
    if (filter & INTVDEBUG_HIST_BRANCHES) {
        if (h->event)
            return 0;
        return hist_is_branch(h);
    }
    return 1;
}

void intvdebug_trace_enable(intvdebug *dbg, int on)
{
    intvdebug_history_enable(dbg, on);
}

int intvdebug_trace_enabled(const intvdebug *dbg)
{
    return intvdebug_history_enabled(dbg);
}

void intvdebug_trace_clear(intvdebug *dbg)
{
    intvdebug_history_clear(dbg);
}

int intvdebug_trace_count(const intvdebug *dbg)
{
    const uint64_t total = dbg->hist_count;
    return (int)(total < INTVDEBUG_HISTORY_SIZE ? total : INTVDEBUG_HISTORY_SIZE);
}

int intvdebug_trace_read(intvdebug *dbg, intvdebug_trace *out, int max)
{
    const uint64_t total = dbg->hist_count;
    int count, idx;

    if (!dbg->hist || max < 1)
        return 0;
    count = intvdebug_trace_count(dbg) < max ? intvdebug_trace_count(dbg) : max;
    for (idx = 0; idx < count; idx++)
        out[idx] = dbg->hist[(total - 1 - (uint64_t)idx) & (INTVDEBUG_HISTORY_SIZE - 1)];
    return count;
}

int intvdebug_trace_format(const intvdebug_trace *entry, char *dst, int dstsz)
{
    return intvdebug_history_format(entry, dst, dstsz);
}

int intvdebug_trace_match(const intvdebug_trace *entry, int filter, uint16_t lo, uint16_t hi)
{
    return intvdebug_history_match(entry, filter, lo, hi);
}

/* ---- the memory map and the cartridge ---------------------------------------------- */

/* What a device is called in the map: its function, not its registration. */
static const char *map_name(const char *name)
{
    static const struct { const char *from, *shown; } names[] = {
        { "PSG0 AY8914", "PSG0" },     { "PSG1 AY8914", "ECS PSG1" },
        { "Pad Pair 0", "Pads" },      { "Pad Pair 1", "ECS pads" },
        { "INTV2PC #0", "Pads" },      { "INTV2PC #1", "ECS pads" },
        { "CGC #0", "Pads" },          { "CGC #1", "ECS pads" },
        { "Int. Voice", "Intellivoice" }, { "Scratch RAM", "Scratch RAM" },
    };
    size_t idx;
    for (idx = 0; idx < sizeof(names) / sizeof(names[0]); idx++)
        if (!strcmp(name, names[idx].from))
            return names[idx].shown;
    return name;
}

/* Is `name` already one of the " + "-joined parts of `joined`? */
static int has_part(const char *joined, const char *name)
{
    const size_t length = strlen(name);
    const char *where = joined;
    while ((where = strstr(where, name)) != NULL) {
        const int starts = where == joined || where[-1] == ' ';
        const int ends = where[length] == '\0' || where[length] == ' ';
        if (starts && ends)
            return 1;
        where += length;
    }
    return 0;
}

/* The devices that answer in one bin, named and joined ("PSG0 + Pads"); the
 * hooks of the machine itself ([Graphics], [Event] ...), the CPU's cache
 * snoop and our own watch device are not memory. */
static void bin_signature(const periph_bus_t *bus, uint32_t bin, char *dst, int dstsz,
                          int *readable, int *writable)
{
    char joined[128] = "";
    int col, pass, parts = 0;

    *readable = *writable = 0;
    for (pass = 0; pass < 2; pass++)
        for (col = 0; col < MAX_PERIPH_BIN; col++) {
            const periph_t *const dev = pass == 0 ? bus->rd[col][bin] : bus->wr[col][bin];
            const char *name;
            if (!dev)
                break;
            if (dev == &g_watch || dev->name[0] == '[' || !strncmp(dev->name, "CP-1610", 7))
                continue;
            if (pass == 0)
                *readable = 1;
            else
                *writable = 1;
            name = map_name(dev->name);
            if (has_part(joined, name) || parts >= 3)
                continue;
            snprintf(joined + strlen(joined), sizeof(joined) - strlen(joined), "%s%s", parts ? " + " : "", name);
            parts++;
        }
    snprintf(dst, (size_t)dstsz, "%.31s", joined[0] ? joined : "unmapped");
}

int intvdebug_memory_map(intvdebug *dbg, intvdebug_region *out, int max)
{
    periph_bus_t *const bus = intv.intv;
    const uint32_t bins = bus ? (bus->addr_mask >> bus->decode_shift) + 1 : 0;
    char name[32], run_name[32] = "";
    int readable, writable, run_read = 0, run_write = 0, count = 0, have = 0;
    uint32_t bin, start = 0;

    if (!paused_now(dbg) || !bus || bins > 4096)
        return 0;
    for (bin = 0; bin <= bins; bin++) {
        if (bin < bins)
            bin_signature(bus, bin, name, (int)sizeof(name), &readable, &writable);
        /* a run ends when what answers changes (or the map does) */
        if (have && (bin == bins || strcmp(name, run_name) || readable != run_read || writable != run_write)) {
            if (count < max) {
                out[count].lo = (uint16_t)(start << bus->decode_shift);
                out[count].hi = (uint16_t)((bin << bus->decode_shift) - 1);
                out[count].readable = (uint8_t)run_read;
                out[count].writable = (uint8_t)run_write;
                snprintf(out[count].name, sizeof(out[count].name), "%s", run_name);
                count++;
            }
            have = 0;
        }
        if (bin < bins && !have) {
            have = 1;
            start = bin;
            run_read = readable;
            run_write = writable;
            snprintf(run_name, sizeof(run_name), "%s", name);
        }
    }
    return count;
}

void intvdebug_cart_info(intvdebug *dbg, intvdebug_cart *out)
{
    (void)dbg;
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", intv.fn_game ? intv.fn_game : "");
    snprintf(out->name, sizeof(out->name), "%s", intv.cart_name ? intv.cart_name : "");
    out->year = intv.cart_year;
    out->fujinet_mailbox = intv.fujinet.mailbox_active ? 1 : 0;
    out->ecs = intv.ecs_enable > 0 ? 1 : 0;   /* -1 is "automatic", and resolves to off unless the cart asks */
    out->jlp = intv.jlp.periph.bus != NULL;
    out->ivoice = intv.ivoice.periph.bus != NULL;
    out->pal = intv.stic.pal ? 1 : 0;
}

int intvdebug_cart_summary(intvdebug *dbg, char *dst, int dstsz)
{
    intvdebug_cart cart;
    const char *slash;
    int len;

    intvdebug_cart_info(dbg, &cart);
    slash = strrchr(cart.path, '/');
    if (!slash)
        slash = strrchr(cart.path, '\\');
    len = snprintf(dst, (size_t)dstsz, "Cartridge: %s", cart.name[0] ? cart.name : slash ? slash + 1
                                                       : cart.path[0] ? cart.path : "(none)");
    if (cart.year > 0 && len < dstsz)
        len += snprintf(dst + len, (size_t)(dstsz - len), " (%d)", cart.year);
    if (len < dstsz)
        len += snprintf(dst + len, (size_t)(dstsz - len), "%s%s%s%s%s", cart.ecs ? ", ECS" : "",
                        cart.jlp ? ", JLP" : "", cart.ivoice ? ", Intellivoice" : "",
                        cart.fujinet_mailbox ? ", FujiNet mailbox" : "", cart.pal ? ", PAL" : ", NTSC");
    return len < dstsz ? len : dstsz - 1;
}

/* ---- symbols ----------------------------------------------------------------------- */

typedef struct { uint16_t addr; char name[48]; int index; } sym_pick;

static int sym_pick_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
    const sym_pick *lhs = lhs_ptr, *rhs = rhs_ptr;
    if (lhs->addr != rhs->addr)
        return lhs->addr < rhs->addr ? -1 : 1;
    return strcasecmp(lhs->name, rhs->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    const size_t len = strlen(needle);
    if (!len)
        return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, len))
            return 1;
    return 0;
}

int intvdebug_symbols_filter(intvdebug *dbg, const char *text, int *out, int max)
{
    intvsymtab *const table = intvsymtab_shared();
    const int total = table ? intvsymtab_count(table) : 0;
    sym_pick *pick;
    int count = 0, idx;

    (void)dbg;
    if (!out || max <= 0 || total <= 0)
        return 0;
    pick = malloc((size_t)total * sizeof(*pick));
    if (!pick)
        return 0;
    for (idx = 0; idx < total; idx++) {
        uint16_t addr;
        char name[48];
        if (intvsymtab_enum(table, idx, &addr, name, sizeof(name), NULL, 0) &&
            contains_nocase(name, text ? text : "")) {
            pick[count].addr = addr;
            snprintf(pick[count].name, sizeof(pick[count].name), "%s", name);
            pick[count].index = idx;
            count++;
        }
    }
    qsort(pick, (size_t)count, sizeof(*pick), sym_pick_cmp);
    if (count > max)
        count = max;
    for (idx = 0; idx < count; idx++)
        out[idx] = pick[idx].index;
    free(pick);
    return count;
}

/* ---- hooks the prompt and the Video tab share ----------------------------------------- */

const char *const intvdebug_reg_names[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "pc", "sp", "s", "z", "o", "c", "i", "d",
    "frame", "cycles", "value", "addr", "old", NULL,
};

static const char *(*g_video_text)(const char *section);

void intvdebug_set_video_text(const char *(*callback)(const char *section))
{
    g_video_text = callback;
}

const char *(*intvdebug_video_text_hook(void))(const char *section)
{
    return g_video_text;
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
#define MB(x) (callback->ram[(x) - FUJI_MB_BASE])
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
    const fujinet_t *const callback = &intv.fujinet;
    const unsigned state = (unsigned)callback->state;
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
    if (!callback->periph.tick)
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
    for (i = 0; i < (int)sizeof(path) - 1 && callback->last_boot_path[i]; i++)
        path[i] = callback->last_boot_path[i] >= 0x20 && callback->last_boot_path[i] < 0x7F
                      ? callback->last_boot_path[i] : '.';
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
        callback->port,
        callback->mailbox_active ? "active" : "off (the cart's memory map covers it)",
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
