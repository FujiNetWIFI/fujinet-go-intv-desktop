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
 * paused, and say so. Nothing here is safe to call while it is running,
 * except the FujiNet trace and state near the end, which have a lock of
 * their own.
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
#define INTVDEBUG_MAX_BREAKPOINTS 128

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

/* ---- stepping by IntyBASIC line ------------------------------------------
 * These use the shared source map (core/debugger/srcmap.h): they run until
 * the PC reaches the first word of a BASIC line, then pause. Code with no
 * BASIC line behind it (the IntyBASIC runtime, EXEC) is run through. With no
 * map loaded they never stop on their own; Pause still works. Only
 * meaningful while paused.
 *   step_line       stops on the next line, going into GOSUBs and ISRs.
 *   step_line_over  the same, but skips whatever a line calls: only lines at
 *                   the starting stack depth or shallower count.
 *   step_line_out   runs until the current routine has returned to its
 *                   caller (the stack is shallower than where it started).
 * The CP-1610 stack grows upward, so "deeper" is a larger R6. */
void intvdebug_step_line(intvdebug *d);
void intvdebug_step_line_over(intvdebug *d);
void intvdebug_step_line_out(intvdebug *d);

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

/* ---- search: valid only while paused ---------------------------------------
 * Finds the runs of words from..to (inclusive) where every word w matches the
 * pattern word p under `mask`: (w & mask) == (p & mask). A hex search uses
 * mask 0xFFFF; text is searched as one character per word with mask 0x00FF
 * (the scratchpad and GROM are 8 bits wide), and INTVDEBUG_SEARCH_NOCASE folds
 * ASCII letters. Writes the start address of each match into out (up to max)
 * and returns how many were found, or -1 while the machine is running. */
#define INTVDEBUG_SEARCH_NOCASE 1
int intvdebug_search(intvdebug *d, const uint16_t *pattern, int n, uint16_t mask, int flags,
                     uint16_t from, uint16_t to, uint16_t *out, int max);

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

/* ---- breakpoints and watchpoints ----------------------------------------
 * One table. An EXEC entry stops before the instruction at lo runs (it is
 * jzIntv's own breakpoint). A READ or WRITE entry watches the words lo..hi
 * and stops AFTER the instruction that touched them (PC is then on the next
 * instruction); intvdebug_last_stop says what was touched. Only the CPU's own
 * data accesses are seen: an instruction fetch never goes through the bus, so
 * a READ entry cannot catch code being fetched (use EXEC for that), and the
 * STIC's and DMA's reads are not counted. Ids are never reused.
 *
 * A WRITE entry may carry a condition on the value written; the scratchpad
 * ($0100-$01EF) is 8 bits wide, so compare with a mask. Safe to call while
 * running: the bus change is applied by the emulator thread at its next
 * hook (or at once when paused). */
#define INTVDEBUG_BP_EXEC  1
#define INTVDEBUG_BP_READ  2
#define INTVDEBUG_BP_WRITE 4

enum {
    INTVDEBUG_COND_ANY = 0,
    INTVDEBUG_COND_CHANGED,  /* the value written differs from the old one */
    INTVDEBUG_COND_EQ,       /* (value & mask) == (cond value & mask) */
    INTVDEBUG_COND_NE
};

typedef struct {
    int      id;          /* assigned by intvdebug_bp_add */
    int      type;        /* INTVDEBUG_BP_* bits; EXEC alone for code */
    uint16_t lo, hi;      /* hi == lo for EXEC */
    int      enabled;
    unsigned hits;
    int      cond;        /* INTVDEBUG_COND_*, WRITE only */
    uint16_t value, mask; /* for COND_EQ / COND_NE */
} intvdebug_bp;

/* Adds an entry (type, lo, hi, cond, value, mask are read; mask 0 = 0xFFFF).
 * Returns its id, or -1 when the table is full or the spec is empty. */
int  intvdebug_bp_add(intvdebug *d, const intvdebug_bp *spec);
int  intvdebug_bp_remove(intvdebug *d, int id);
int  intvdebug_bp_enable(intvdebug *d, int id, int enabled);
int  intvdebug_bp_clear_hits(intvdebug *d);
/* Copies the table (oldest first); returns how many. */
int  intvdebug_bp_list(intvdebug *d, intvdebug_bp *out, int max);
/* Bumped by every change to the table or a hit count, for a UI to poll. */
unsigned intvdebug_bp_generation(const intvdebug *d);

/* The execute-breakpoint shortcuts the CPU and BASIC views use. */
/* Returns 1 when the breakpoint is now set, 0 when it was cleared, -1 when
 * the table is full. Safe while running. */
int  intvdebug_breakpoint_toggle(intvdebug *d, uint16_t addr);
/* 1 when an enabled EXEC breakpoint is at addr. */
int  intvdebug_breakpoint_is_set(intvdebug *d, uint16_t addr);
/* 0 none, 1 enabled, 2 disabled: a disabled one is drawn as a hollow dot. */
int  intvdebug_breakpoint_state(intvdebug *d, uint16_t addr);
/* Removes every entry, watchpoints included. */
void intvdebug_breakpoint_clear_all(intvdebug *d);
/* Copies the set EXEC addresses into out (ascending); returns how many. */
int  intvdebug_breakpoint_list(intvdebug *d, uint16_t *out, int max);

/* Why the machine last stopped. */
enum {
    INTVDEBUG_STOP_NONE = 0,
    INTVDEBUG_STOP_PAUSE,
    INTVDEBUG_STOP_STEP,
    INTVDEBUG_STOP_BREAKPOINT,  /* an EXEC entry, or a one-shot (step over/out) */
    INTVDEBUG_STOP_WATCH_READ,
    INTVDEBUG_STOP_WATCH_WRITE
};

typedef struct {
    int      reason;
    int      bp_id;      /* the entry that fired, 0 if none */
    uint16_t addr;       /* the word touched (watch), or the PC (breakpoint) */
    uint16_t value;      /* the value written (WRITE) */
    uint16_t old_value;  /* the value before it (WRITE, when known) */
    uint16_t pc;         /* the instruction after the access / the one stopped at */
    uint64_t serial;     /* intvdebug_stop_serial at the stop */
} intvdebug_stop_info;

int  intvdebug_last_stop(intvdebug *d, intvdebug_stop_info *out);

/* ---- instruction history and trace -------------------------------------------
 * With history on, every instruction the CPU runs is recorded into a ring of
 * the last 65536 (pc, the opcode's words, R0-R7, flags, cycle). It costs a
 * hook call per instruction instead of one per timeslice, so it is off until
 * asked for. The history tab shows it while stopped; the trace tab reads the
 * same ring while the machine runs (a line being written at that instant may
 * be torn; harmless for display). */
#define INTVDEBUG_HISTORY_SIZE 65536

enum { INTVDEBUG_EV_NONE = 0, INTVDEBUG_EV_INTERRUPT = 1, INTVDEBUG_EV_BUS = 2 };

typedef struct {
    uint64_t cycle;
    uint16_t pc;        /* the instruction's address */
    uint16_t w[3];      /* its opcode words */
    uint16_t r[8];      /* registers AFTER it ran */
    uint16_t flags;     /* bit0 S, 1 C, 2 O, 3 Z, 4 I, 5 D (double-byte-data pending) */
    uint8_t  event;     /* INTVDEBUG_EV_*: an interrupt or bus grant, not an instruction */
} intvdebug_hist;

void intvdebug_history_enable(intvdebug *d, int on);
int  intvdebug_history_enabled(const intvdebug *d);
void intvdebug_history_clear(intvdebug *d);
/* Total instructions recorded since it was last cleared (the ring holds the
 * last INTVDEBUG_HISTORY_SIZE of them). A UI polls this to see new lines. */
uint64_t intvdebug_history_count(const intvdebug *d);
/* Copies up to max of the newest entries, oldest first; returns how many. */
int  intvdebug_history_copy(intvdebug *d, intvdebug_hist *out, int max);

#define INTVDEBUG_HIST_BRANCHES 1   /* only branches, jumps and calls */
#define INTVDEBUG_HIST_RANGE    2   /* only pc in lo..hi */
/* One display line for an entry: "50CD  JSR  R5,$50D5   R0 0001 ... S0 C0 O0 Z0 I1". */
int  intvdebug_history_format(const intvdebug_hist *h, char *dst, int dstsz);
/* True when the entry passes the filter. */
int  intvdebug_history_match(const intvdebug_hist *h, int filter, uint16_t lo, uint16_t hi);

/* ---- the cartridge and the memory map: valid only while paused ----------- */
typedef struct {
    uint16_t lo, hi;      /* inclusive */
    uint8_t  readable, writable;
    char     name[32];    /* what answers there: "System RAM", "EXEC ROM", "STIC" ... */
} intvdebug_region;

/* The address map as the bus decodes it, runs of 16-word bins with the same
 * devices merged. Returns how many regions (up to max). */
int  intvdebug_memory_map(intvdebug *d, intvdebug_region *out, int max);

typedef struct {
    char path[512];       /* the cartridge image, or "(FujiNet config)" */
    char name[128];       /* its title if it says (BIN+CFG metadata), else "" */
    int  year;            /* 0 if unknown */
    int  fujinet_mailbox; /* the FujiNet mailbox is mapped */
    int  ecs;             /* ECS present */
    int  jlp;             /* JLP accelerator / flash */
} intvdebug_cart;

void intvdebug_cart_info(intvdebug *d, intvdebug_cart *out);

/* ---- FujiNet: the mailbox's BoIP bytes, as FujiBus packets ---------------
 * A tap on the staged tree's fn_sock (fn_sock_set_trace) feeds this from
 * the moment the program starts, engaged or not, so it shows what happened
 * before anyone opened a debugger. Safe while running. */
typedef struct {
    uint64_t serial;        /* new whenever this entry changes */
    int      to_host;       /* 1: FujiNet -> Intellivision, 0: Intellivision -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint32_t repeat;        /* identical exchanges folded into this one, >= 1 */
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "Intellivision -> FujiNet 70 F2 READ DEVICE SLOTS (6 bytes)" */
} intvdebug_fuji_packet;
#define INTVDEBUG_FUJI_TRACE_MAX 512
int  intvdebug_fujinet_trace(intvdebug *d, intvdebug_fuji_packet *out, int max);  /* oldest first */
void intvdebug_fujinet_trace_clear(intvdebug *d);
/* The mailbox and its link to FujiNet, as text lines. */
int  intvdebug_fujinet_state(intvdebug *d, char *dst, int dstsz);

/* The debugger is a process-wide singleton, like cfg_t intv itself (see
 * core/jzintv/intv_host.h) -- one machine, one debugger. Returns the same
 * object every time; never NULL. */
intvdebug *intvdebug_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTVDEBUG_H */
