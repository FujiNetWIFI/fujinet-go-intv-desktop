/*
 * intvsession -- toolkit-agnostic desktop session for FujiNet Go Intv.
 *
 * Owns the headless jzIntv core (driven on its own paced thread through
 * core/jzintv/intv_host.h), the frame store (core/jzintv/intv_frame.h), the
 * shared settings store, and the ROM directory layout. Frontends (GTK, Qt,
 * AppKit, Win32) drive this API and only do windowing, painting, and event
 * translation.
 *
 * Like the CoCo port (and unlike ADAM/Apple II): FujiNet listens and the
 * emulator connects out to it (src/fujinet/fn_sock.c in the staged jzIntv
 * tree is a BoIP TCP client). Starting the FujiNet runtime is not yet wired
 * up here (see cmake/FujiNetRuntime.cmake) -- intvsession_start always passes
 * --fujinet regardless, since jzIntv's own mailbox peripheral is inert, not
 * fatal, with nothing listening (fn_sock's connect is non-blocking).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTVSESSION_H
#define INTVSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed geometry -- unlike the CoCo's, the Intellivision's STIC output does
 * not vary at runtime. See core/jzintv/intv_frame.h. */
#define INTVSESSION_FB_WIDTH  160
#define INTVSESSION_FB_HEIGHT 200

/* BoIP (Bus Over IP) loopback port -- FujiNet listens here and the emulator
 * connects out -- deliberately not jzIntv's own default (1985), so a
 * standalone fujinet-pc-intv and this app can both run at once. High ports
 * for the same reason the other desktop targets use them (ADAM 65214, Apple
 * II 64001, CoCo's Becker port 65504): each target claims a high port of its
 * own so all of them can run side by side. */
#define INTVSESSION_BOIP_PORT  65503
#define INTVSESSION_WEBUI_PORT 64003

typedef struct intvsession intvsession;

/* All members optional (NULL = default).
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-intv
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-intv
 */
typedef struct {
    const char *config_dir;
    const char *data_dir;
} intvsession_paths;

/* Creates the session, provisions the config/data directories, materialises
 * the embedded ROMs (if any -- see WITH_INTV_ROMS) into the ROM directory,
 * and loads the shared settings store. Does not start emulation. Returns
 * NULL only on out-of-memory / unusable directories. */
intvsession *intvsession_new(const intvsession_paths *paths);
void         intvsession_free(intvsession *s);

/* ---- settings (shared INI; one store for all frontends/platforms) ------- */
int         intvsession_get_int(intvsession *s, const char *key, int def);
void        intvsession_set_int(intvsession *s, const char *key, int value);
const char *intvsession_get_str(intvsession *s, const char *key,
                                const char *def);
void        intvsession_set_str(intvsession *s, const char *key,
                                const char *value);
void        intvsession_settings_flush(intvsession *s);

/* ---- lifecycle ------------------------------------------------------------
 * Starts the emulator thread; boots the embedded FujiNet config ROM until a
 * frontend loads a cartridge (cartridge loading is not implemented yet).
 * Returns 0 on success, -1 with intvsession_last_error() set. */
int  intvsession_start(intvsession *s);
void intvsession_stop(intvsession *s);
int  intvsession_is_running(const intvsession *s);
const char *intvsession_last_error(const intvsession *s);

/* ---- video -----------------------------------------------------------------
 * The emulator thread stores the latest changed frame; the UI thread pulls it
 * on its own vsync/frame-clock tick. copy_frame copies the latest frame into
 * dst (XRGB8888, tightly packed, INTVSESSION_FB_WIDTH*INTVSESSION_FB_HEIGHT
 * uint32) iff its serial differs from *serial_inout, updates *serial_inout,
 * and returns 1; returns 0 when the frame is unchanged (dst untouched). Pass
 * *serial_inout = 0 to force a copy (e.g. first paint after a window map). */
int intvsession_copy_frame(intvsession *s, uint32_t *dst,
                           uint64_t *serial_inout);

/* ---- input -----------------------------------------------------------------
 * See core/jzintv/intv_host.h for exactly what each id writes and why this
 * is a direct pad_t.l[]/r[] injection rather than going through jzIntv's own
 * keysym/event layer. side: 0 = left controller, 1 = right. */
typedef enum {
    INTVSESSION_PAD_LEFT = 0,
    INTVSESSION_PAD_RIGHT = 1,
} intvsession_pad_side;

typedef enum {
    INTVSESSION_KEY_0 = 0,
    INTVSESSION_KEY_1, INTVSESSION_KEY_2, INTVSESSION_KEY_3,
    INTVSESSION_KEY_4, INTVSESSION_KEY_5, INTVSESSION_KEY_6,
    INTVSESSION_KEY_7, INTVSESSION_KEY_8, INTVSESSION_KEY_9,
    INTVSESSION_KEY_CLEAR,
    INTVSESSION_KEY_ENTER,
    INTVSESSION_ACTION_TOP,
    INTVSESSION_ACTION_LOWER_LEFT,
    INTVSESSION_ACTION_LOWER_RIGHT,
} intvsession_key;

void intvsession_pad_key(intvsession *s, intvsession_pad_side side,
                         intvsession_key key, int pressed);
/* direction: 0-15 clock position (0 = East, clockwise), or -1 to center. */
void intvsession_pad_disc(intvsession *s, intvsession_pad_side side,
                          int direction);

/* ---- paths ----------------------------------------------------------------
 * Directory paths (valid for the session's lifetime). */
const char *intvsession_roms_path(const intvsession *s);
const char *intvsession_config_path(const intvsession *s);
const char *intvsession_data_path(const intvsession *s);

/* 1 when the ROM directory holds exec.bin and grom.bin -- a frontend that
 * gets 0 here should prompt for "Import System ROMs..." rather than start a
 * session that would fail to boot. See COMPLIANCE.md. */
int intvsession_has_system_roms(const intvsession *s);

#ifdef __cplusplus
}
#endif

#endif /* INTVSESSION_H */
