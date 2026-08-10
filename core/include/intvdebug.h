/*
 * intvdebug -- the CP-1610 debugger engine.
 *
 * Built on jzIntv's own hook, not a stepped path of our own: cp1600_t (the
 * staged tree's src/cp1600/cp1600.h) offers cp1600_instr_tick(), a callback
 * invoked between instructions whenever cp1600->steps_remaining reaches 0
 * and gets refilled from cp1600->step_count. jzIntv's own console debugger
 * (src/debug/debug.c) already registers itself there unconditionally at
 * cfg_init() time; intvdebug_set_engaged(d, 1) simply re-registers OUR OWN
 * hook in its place (a single function pointer, not a chain) -- safe,
 * because the console debugger is never reachable here (nothing offers it
 * a stdin to read commands from).
 *
 * Disengaged, step_count is set huge so the hook fires rarely -- a session
 * that never opens the debugger pays almost nothing. Engaged, step_count is
 * 1, so the hook fires after every instruction (needed to honour
 * breakpoints and expose live state); intvdebug_pause blocks the calling
 * (emulator) thread right there inside the hook until resumed, which is
 * exactly how the CPU's own progress is what stops during a pause -- there
 * is no separate "paused" flag the run loop checks elsewhere.
 *
 * THREADING. Everything here is called from a UI thread except where noted.
 * While the machine is paused the emulator thread is parked inside the
 * hook, so the machine is quiescent and reading its memory/registers from
 * another thread is safe; that is why those calls are only valid while
 * paused, and say so. Nothing here is safe to call while it is running.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTVDEBUG_H
#define INTVDEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct intvdebug intvdebug;

/* Enough for a UI that toggles them by clicking a disassembly line. */
#define INTVDEBUG_MAX_BREAKPOINTS 64

/* The CP-1610's programmer-visible state. r[7] is also the PC (the CPU has
 * no separate PC register -- R7 fills that role), duplicated into pc for
 * convenience/clarity, matching how a frontend would want to display it. */
typedef struct {
    uint16_t r[8];   /* R0-R7; r[6] is SP, r[7] is PC by CP-1610 convention */
    uint16_t pc;     /* == r[7] */
    int S, C, O, Z, I; /* status flags: Sign, Carry, Overflow, Zero,
                        * Interrupt-enable (matches cp1600_t's own field
                        * names in cp1600.h) */
    int D;           /* Double-Byte-Data countdown (a CP-1610 quirk for
                      * split immediate encoding); mostly of interest to
                      * the disassembler, exposed for completeness. */
} intvdebug_regs;

/* Engage or release the debugger. See this file's header for exactly what
 * engaging costs and why releasing is safe even mid-session. Releasing
 * resumes first if paused. */
void intvdebug_set_engaged(intvdebug *d, int engaged);
int  intvdebug_is_engaged(const intvdebug *d);

/* Pause at the next instruction boundary, or resume. intvdebug_pause
 * returns immediately -- the machine stops shortly after (at most one
 * instruction later), and the stop serial below is how a UI notices. */
void intvdebug_pause(intvdebug *d);
void intvdebug_resume(intvdebug *d);
int  intvdebug_is_paused(const intvdebug *d);

/* Run exactly one instruction, then pause again. */
void intvdebug_step(intvdebug *d);

/* Step over a subroutine call: if the instruction at PC is a JSR/JSRE/JSRD/
 * JSRi, run until it returns (a one-shot breakpoint on the following
 * instruction); otherwise this is intvdebug_step. Only meaningful while
 * paused. */
void intvdebug_step_over(intvdebug *d);

/* Run until control leaves the current subroutine. Approximate in the way
 * every debugger's is for a register-return architecture -- the CP-1610 has
 * no hardware call stack, and JSR's destination register (conventionally
 * R5 for EXEC and most game software, but not fixed by the ISA) is not
 * recoverable once already inside the routine. This breaks on whatever R5
 * currently holds; a routine entered through a different register, or one
 * that overwrites R5 before returning through it, defeats this -- it falls
 * back to running free rather than stopping somewhere wrong. Only
 * meaningful while paused. */
void intvdebug_step_out(intvdebug *d);

/* Bumped every time the machine stops. A UI polls this on its frame clock
 * and refreshes when it changes, rather than being called back on the
 * emulator thread. */
uint64_t intvdebug_stop_serial(const intvdebug *d);

/* ---- inspection: valid only while paused --------------------------------
 * Each returns 0 and touches nothing when the machine is running. */
int intvdebug_regs_get(intvdebug *d, intvdebug_regs *out);

/* Reads n words starting at addr (16-bit address space) through
 * periph_peek -- the same non-destructive read path jzIntv's own debugger
 * uses (src/debug/debug.c's debug_dispmem), so memory-mapped hardware
 * behaves as an inspector expects without side effects like FIFO advance.
 * Returns the number read. */
int intvdebug_read(intvdebug *d, uint16_t addr, uint16_t *dst, int n);
/* Writes n words starting at addr, through the same bus path a real CPU
 * write would take (side effects included -- this is a poke, not a peek).
 * Returns n. */
int intvdebug_write(intvdebug *d, uint16_t addr, const uint16_t *src, int n);

typedef struct {
    uint16_t addr;
    int      length;         /* instruction length in words (1-3) */
    char     text[64];       /* disassembled mnemonic + operands */
} intvdebug_dasm_line;

/* Disassembles up to `max` instructions starting at addr, walking forward
 * by each one's length. dbd (Double-Byte-Data) only matters when addr ==
 * the current PC while paused -- pass intvdebug_regs.D there, 0 otherwise
 * (matches every other CP-1610 disassembly UI's convention: DBD state
 * elsewhere in memory cannot be known without simulating execution).
 * Returns how many were produced -- 0 while running, since it reads the
 * machine's memory. This is what a frontend fills its disassembly list
 * from; it never has to know the encoding. */
int intvdebug_disassemble(intvdebug *d, uint16_t addr, int dbd,
                          intvdebug_dasm_line *out, int max);

/* ---- breakpoints -------------------------------------------------------- */
/* Returns 1 when the breakpoint is now set, 0 when it was cleared, -1 when
 * the table is full. Safe while running. */
int  intvdebug_breakpoint_toggle(intvdebug *d, uint16_t addr);
int  intvdebug_breakpoint_is_set(intvdebug *d, uint16_t addr);
void intvdebug_breakpoint_clear_all(intvdebug *d);
/* Copies the set addresses into out (ascending); returns how many. */
int  intvdebug_breakpoint_list(intvdebug *d, uint16_t *out, int max);

/* The debugger is a process-wide singleton, like cfg_t intv itself (see
 * core/jzintv/intv_host.h) -- one machine, one debugger. Returns the same
 * object every time; never NULL. */
intvdebug *intvdebug_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTVDEBUG_H */
