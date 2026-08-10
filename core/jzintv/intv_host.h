/*
 * intv_host -- owns cfg_t intv (jzIntv's single global machine instance --
 * see src/cfg/cfg.h's own comment: "CFG owns the entire machine") and runs
 * it on its own thread by calling jzintv_entry_point(), the same
 * non-emscripten entry point src/jzintv.c's own main() would call.
 *
 * jzIntv's machine is a process-wide singleton (cfg_t intv is `extern`, not
 * something cfg_init allocates), so intv_host_start refuses a second
 * concurrent session rather than corrupt shared state -- exactly one
 * intv_host may be running per process, matching the one-machine-per-process
 * shape every frontend already has.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_HOST_H
#define INTV_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *rom_dir;     /* Directory holding exec.bin/grom.bin --
                              * provisioned from the embedded table first if
                              * either is missing there (see
                              * core/src/roms_embedded.h). Required. */
    const char *fujinet_host; /* NULL/"" -> 127.0.0.1 */
    int         fujinet_port; /* 0 -> jzIntv's own default (1985); the
                              * session always passes an explicit value (see
                              * intvsession.h's INTVSESSION_BOIP_PORT) so a
                              * standalone fujinet-pc on the jzIntv default
                              * is never fought over. */
} intv_host_opts;

/* Writes any of exec.bin/grom.bin missing from opts->rom_dir out of the
 * embedded table (core/src/roms_embedded.h). A no-op per file already
 * present -- an imported ROM always wins over the compiled-in one. Returns
 * 0 on success, -1 on a write failure. */
int intv_host_provision_roms(const char *rom_dir);

/* Starts jzintv_entry_point() on its own thread, with --fujinet always set
 * (see the file header on why: the FujiNet mailbox is inert, not fatal, with
 * nothing listening -- fn_sock.c connects out non-blockingly) and no ROM
 * argument, so the machine boots the embedded FujiNet config ROM until a
 * frontend loads a cartridge. Returns 0 on success, -1 if a session is
 * already running or the ROM directory is missing exec.bin/grom.bin. */
int intv_host_start(const intv_host_opts *opts);

/* Requests the emulator thread stop (intv.do_exit = 1) and joins it. Safe to
 * call even if not running. */
void intv_host_stop(void);

int intv_host_is_running(void);

/* ---- direct pad injection -------------------------------------------------
 * jzIntv's keyboard/joystick bindings (src/cfg/mapping.c) work by writing a
 * fixed scan-code-like value into pad_t.l[]/r[] (pad0.l[] = left/PD0L,
 * pad0.r[] = right/PD0R) on press and 0 on release -- e.g. "PD0L_KP1" is
 * `intv.pad0.l[1] = 0x81` down, `= 0` up. This is exactly the mechanism a
 * clickable keypad window needs, and skips jzIntv's own event/keysym layer
 * entirely (that layer exists to turn a *keyboard* event into one of these
 * writes; a mouse click can just make the write directly).
 *
 * INTV_PAD_KEY_* below is both the pad_t.l[]/r[] array index AND carries the
 * scan-code value intv_host_pad_key writes -- see the .c file's key_codes
 * table, copied verbatim from mapping.c's PD0L_KP and PD0L_A entries. */
typedef enum {
    INTV_PAD_LEFT = 0,
    INTV_PAD_RIGHT = 1,
} intv_pad_side;

typedef enum {
    INTV_PAD_KEY_0 = 0,
    INTV_PAD_KEY_1, INTV_PAD_KEY_2, INTV_PAD_KEY_3,
    INTV_PAD_KEY_4, INTV_PAD_KEY_5, INTV_PAD_KEY_6,
    INTV_PAD_KEY_7, INTV_PAD_KEY_8, INTV_PAD_KEY_9,
    INTV_PAD_KEY_CLEAR,       /* "C" */
    INTV_PAD_KEY_ENTER,       /* "E" */
    INTV_PAD_ACTION_TOP,
    INTV_PAD_ACTION_LOWER_LEFT,
    INTV_PAD_ACTION_LOWER_RIGHT,
    INTV_PAD_KEY_COUNT
} intv_pad_key;

/* Sets/clears one keypad digit or action button on the given controller.
 * Safe to call whether or not the emulator is running (writes directly to
 * the cfg_t intv global; a no-op has nowhere useful to go before
 * intv_host_start, but nothing crashes). */
void intv_host_pad_key(intv_pad_side side, intv_pad_key key, int pressed);

/* The 16-way disc, addressed by clock position (0 = East, going clockwise
 * through the 16 half-steps mapping.c's PD0L_D and PD0R_D table defines --
 * see the .c file's disc_codes table for the exact value each position
 * writes). direction = -1 centers the disc (writes 0, same as every
 * direction's release). Unlike intv_host_pad_key (which ORs then clears a
 * single bit pattern, mirroring how the keyboard binding treats each digit
 * as independent), this always OVERWRITES pad_t.l[15]/r[15] outright: a
 * clickable disc widget has exactly one position active at a time, never a
 * combination of separately-held keys. */
void intv_host_pad_disc(intv_pad_side side, int direction);

/* Writes PSG0 (the base unit's AY-3-8914) registers intended to produce an
 * audible tone on channel A: register 0 (tone A period, low byte), register
 * 11 (channel A fixed amplitude, max), register 8 (mixer, active-low --
 * clearing bit 0 enables tone A, setting bits 1-5 disables tone B/C and all
 * three noise channels). The '8914 remaps the standard AY-3-8910 register
 * layout this way -- register 8 is the mixer here, and 11/12/13 are the
 * per-channel amplitude registers, cross-checked against
 * ay8910_calc_sound's own register reads in the staged tree's
 * src/ay8910/ay8910.c (the actual synthesis code, not just the write
 * path's generic reg[] store).
 *
 * KNOWN ISSUE: confirmed via intv_host.c's own debug output that these
 * writes reach ay8910->reg[] with the intended values, and the bit_a =
 * (snd_a | chn_a) & (noi_a | chn_n) formula in ay8910_calc_sound reduces to
 * exactly chn_a (the tone generator's own toggle state) with these register
 * values -- but the resulting audio output is empirically still silent in
 * core/tests/audio_test.c. Not yet root-caused; audio_test.c does not fail
 * on this (see its own comment), only on the pipeline failing to move data
 * at all. Whatever is wrong is either in the tone generator's internal
 * counter warm-up (cnt0/max0) or specific to driving these registers from
 * a thread other than the emulator's own -- worth another look before
 * relying on this helper for anything beyond "does data flow." */
void intv_host_debug_test_tone(void);

#ifdef __cplusplus
}
#endif

#endif /* INTV_HOST_H */
