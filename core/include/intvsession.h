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

/* ---- machine options --------------------------------------------------
 * Tri-state hardware toggles, matching jzIntv's own cfg_t fields
 * (ecs_enable/ivc_enable) one-for-one: AUTO lets jzIntv decide from
 * cartridge metadata (its own default -- most carts want this), OFF/ON
 * force the peripheral regardless. See core/jzintv/intv_host.h. */
enum {
    INTVSESSION_HW_AUTO = 0,
    INTVSESSION_HW_OFF,
    INTVSESSION_HW_ON,
};

enum {
    INTVSESSION_VIDEO_NTSC = 0,
    INTVSESSION_VIDEO_PAL,
};

/* Read by intvsession_start; filled from the settings store (keys "ecs",
 * "ivoice", "video_standard") by intvsession_default_opts so every frontend
 * builds the same options the same way -- see that function's own comment
 * for the exact keys and defaults. */
typedef struct {
    int ecs;    /* INTVSESSION_HW_* */
    int ivoice; /* INTVSESSION_HW_* */
    int video;  /* INTVSESSION_VIDEO_* */
    const char *cart_path; /* NULL/"" -> boot the embedded FujiNet config
                            * ROM (unchanged default). Otherwise a path to a
                            * cartridge image, passed through to
                            * intv_host_opts.cart_path. Not read from the
                            * settings store by intvsession_default_opts
                            * (see that function) -- callers that persist a
                            * "last loaded cartridge" do so themselves and
                            * set this field explicitly. */
} intvsession_start_opts;

/* Fills *opts from the settings store: ecs/ivoice default to
 * INTVSESSION_HW_AUTO (jzIntv's own cart-metadata-driven default), video
 * defaults to INTVSESSION_VIDEO_NTSC, cart_path defaults to the "cart"
 * settings key (NULL if unset, i.e. boot the embedded FujiNet config ROM).
 * A frontend applying a machine-option change should re-call this (after
 * intvsession_set_int/intvsession_set_str on the relevant key) rather than
 * hand-assemble the struct, so a new option added here only has to be read
 * in one place. The returned cart_path points into settings storage owned
 * by s and is valid until the next settings mutation. */
void intvsession_default_opts(intvsession *s, intvsession_start_opts *opts);

/* Convenience: stop the running session (if any), set the "cart" settings
 * key to path (or clear it if path is NULL/"", reverting to the embedded
 * FujiNet config ROM), flush settings, and start again with the current
 * defaults. jzIntv is a process-wide singleton (see intv_host.h) so loading
 * a different cartridge is always stop-then-start, never a live swap.
 * Returns 0 on success, -1 with intvsession_last_error() set. */
int intvsession_load_cart(intvsession *s, const char *path);

/* The currently persisted cartridge path ("" if none -- i.e. the embedded
 * FujiNet config ROM boots), read from the same "cart" settings key
 * intvsession_default_opts fills cart_path from. */
const char *intvsession_cart_path(intvsession *s);

/* Inverse of intvsession_load_cart: clears the persisted cartridge and
 * restarts, which boots the embedded FujiNet config ROM (see
 * intv_host_start's own comment on cart_path == NULL). A frontend's "Reset
 * to CONFIG" action -- also the only way back to CONFIG once a cartridge's
 * .cfg memory map has disabled the FujiNet mailbox for the session (see the
 * staged jzIntv fujinet peripheral), since that only clears on restart.
 * Returns 0 on success, -1 with intvsession_last_error() set. */
int intvsession_reset_to_config(intvsession *s);

/* Soft-resets the currently running cartridge in place -- the console-level
 * RESET a real Intellivision's front switch performs, not a process
 * restart: the cart stays mapped (see intv_host.h's intv_host_reset), so a
 * cartridge pushed over FujiNet (which never touches the persisted "cart"
 * key -- see intv_host_start's own comment) restarts as itself rather than
 * dropping back to CONFIG. Every held key/disc/ECS input is released first,
 * so a button held at the moment of reset doesn't stay stuck down in the
 * machine. Unlike intvsession_reset_to_config, this does not touch the
 * "cart" setting, does not stop/restart the FujiNet runtime, and does not
 * block -- it only arms jzIntv's own one-shot reset for its next tick.
 * Always succeeds (0) if a session is running; a no-op otherwise. */
int intvsession_reset_game(intvsession *s);

/* Menu/combo label tables, NULL past the end so a caller walks idx = 0..
 * until NULL rather than hardcoding a count. */
const char *intvsession_hw_mode_name(int idx);   /* "Auto" / "Off" / "On" */
const char *intvsession_video_name(int idx);     /* "NTSC (60 Hz)" / "PAL (50 Hz)" */

/* 1 when the ROM directory holds a 24576-byte ecs.bin -- a Settings dialog
 * should disable the ECS option (rather than let intvsession_start refuse
 * it) when this is 0. Deliberately separate from
 * intvsession_has_system_roms: a missing ecs.bin must not stop the machine
 * from booting at all for users who never turn ECS on. */
int intvsession_has_ecs_rom(const intvsession *s);

/* ---- lifecycle ------------------------------------------------------------
 * Starts the emulator thread with the given machine options (NULL ==
 * everything AUTO/NTSC, equivalent to intvsession_default_opts on a
 * from-scratch settings store); boots the embedded FujiNet config ROM until
 * a frontend loads a cartridge (cartridge loading is not implemented yet).
 * Returns 0 on success, -1 with intvsession_last_error() set -- including
 * when opts->ecs is forced ON without a usable ecs.bin (checked before the
 * emulator thread starts; see intv_host_start's own comment on why this
 * can't be left to jzIntv itself to discover). */
int  intvsession_start(intvsession *s, const intvsession_start_opts *opts);
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
 * keysym/event layer. side: 0 = left controller, 1 = right, 2/3 = the ECS's
 * own second controller pair (meaningful only when ECS is enabled -- see
 * intvsession_start_opts). */
typedef enum {
    INTVSESSION_PAD_LEFT = 0,
    INTVSESSION_PAD_RIGHT = 1,
    INTVSESSION_PAD_ECS_LEFT = 2,
    INTVSESSION_PAD_ECS_RIGHT = 3,
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
    INTVSESSION_KEY_COUNT /* not a real key -- trailing count, for bindings.c's
                          * per-side table and any frontend that wants to walk
                          * every mappable button. */
} intvsession_key;

void intvsession_pad_key(intvsession *s, intvsession_pad_side side,
                         intvsession_key key, int pressed);
/* direction: 0-15 clock position (0 = East, clockwise), or -1 to center. */
void intvsession_pad_disc(intvsession *s, intvsession_pad_side side,
                          int direction);

/* ---- disc geometry, shared by every frontend's clickable disc widget ----
 * Pure, no session needed (same shape as intvsession_key_from_keysym below).
 * Every frontend used to carry its own copy of this arithmetic AND its own
 * copy of the numbers the widget is drawn from, which is exactly how the
 * four discs drifted apart -- one lighting a 67.5-degree wedge for a
 * 22.5-degree sector, another the mirror image of the sector actually
 * pressed. One definition here, one set of constants, four widgets that
 * only differ in which toolkit draws the lines. */
#define INTVSESSION_DISC_POSITIONS 16
#define INTVSESSION_DISC_SECTOR_DEG (360.0 / INTVSESSION_DISC_POSITIONS)
#define INTVSESSION_DISC_DEADZONE_FRAC 0.22

/* dx/dy: offsets from the disc's centre in SCREEN convention -- +dx right,
 * +dy DOWN -- in the same units as radius. Returns the clock position whose
 * sector contains that point (0-15: 0 = E, 1 = ENE, 2 = NE, ... numerically
 * counter-clockwise, matching intv_host.c's own disc_codes table), or -1
 * when the point is inside the dead centre (or radius is non-positive).
 *
 * Resolves all 16 positions, including the 8 odd half-steps that OR two
 * adjacent cardinal bits together. That is deliberately UNLIKE
 * intv_disc_from_stick (core/src/gamepad_sdl.c), which rounds an analog
 * stick to the 8 even positions only: a stick sits wherever a spring and a
 * thumb leave it and wobbles a few degrees off-axis, so a half-step sector
 * only +-11.25 degrees wide catches wobble that the player did not intend,
 * whereas a mouse pointer or fingertip is exactly where its owner put it. */
int intvsession_disc_from_point(double dx, double dy, double radius);

/* ---- ECS keyboard -----------------------------------------------------
 * The ECS's own 7x8 scan-matrix keyboard, distinct from the hand
 * controllers above -- see core/jzintv/intv_host.h's intv_ecs_key for the
 * full key list and why chording works here (unlike the disc). Meaningful
 * only when ECS is enabled. A frontend switching its keyboard focus away
 * from "ECS keyboard" mode (or losing window focus) should call
 * intvsession_ecs_keys_clear() so a key held at that moment doesn't stay
 * stuck down in the emulated matrix. */
typedef enum {
    INTVSESSION_ECS_KEY_LEFT = 0, INTVSESSION_ECS_KEY_PERIOD,
    INTVSESSION_ECS_KEY_SEMI, INTVSESSION_ECS_KEY_P, INTVSESSION_ECS_KEY_ESC,
    INTVSESSION_ECS_KEY_0, INTVSESSION_ECS_KEY_ENTER,
    INTVSESSION_ECS_KEY_COMMA, INTVSESSION_ECS_KEY_M, INTVSESSION_ECS_KEY_K,
    INTVSESSION_ECS_KEY_I, INTVSESSION_ECS_KEY_9, INTVSESSION_ECS_KEY_8,
    INTVSESSION_ECS_KEY_O, INTVSESSION_ECS_KEY_L,
    INTVSESSION_ECS_KEY_N, INTVSESSION_ECS_KEY_B, INTVSESSION_ECS_KEY_H,
    INTVSESSION_ECS_KEY_Y, INTVSESSION_ECS_KEY_7, INTVSESSION_ECS_KEY_6,
    INTVSESSION_ECS_KEY_U, INTVSESSION_ECS_KEY_J,
    INTVSESSION_ECS_KEY_V, INTVSESSION_ECS_KEY_C, INTVSESSION_ECS_KEY_F,
    INTVSESSION_ECS_KEY_R, INTVSESSION_ECS_KEY_5, INTVSESSION_ECS_KEY_4,
    INTVSESSION_ECS_KEY_T, INTVSESSION_ECS_KEY_G,
    INTVSESSION_ECS_KEY_X, INTVSESSION_ECS_KEY_Z, INTVSESSION_ECS_KEY_S,
    INTVSESSION_ECS_KEY_W, INTVSESSION_ECS_KEY_3, INTVSESSION_ECS_KEY_2,
    INTVSESSION_ECS_KEY_E, INTVSESSION_ECS_KEY_D,
    INTVSESSION_ECS_KEY_SPACE, INTVSESSION_ECS_KEY_DOWN,
    INTVSESSION_ECS_KEY_UP, INTVSESSION_ECS_KEY_Q, INTVSESSION_ECS_KEY_1,
    INTVSESSION_ECS_KEY_RIGHT, INTVSESSION_ECS_KEY_CTRL,
    INTVSESSION_ECS_KEY_A,
    INTVSESSION_ECS_KEY_SHIFT,
    INTVSESSION_ECS_KEY_PHYSICAL_COUNT, /* the 48 real key caps end here --
                                         * what an on-screen ECS keyboard
                                         * draws a button for. */

    /* ---- shifted symbols -------------------------------------------------
     * The second character printed on an ECS key cap. On real hardware the
     * only way to reach one is to hold SHIFT with the base key, and these
     * do exactly that: each is the base key's own matrix bit written into
     * the high byte of intv.pad1.k[row], which is jzIntv's "fake shift"
     * convention -- pads.c asserts SHIFT for the scan on their behalf. See
     * core/jzintv/intv_host.h's intv_ecs_key for the full mechanism.
     *
     * Which base key each rides on is the ECS's own layout, NOT a PC's:
     * '%' is SHIFT+LEFT-ARROW, "'" is SHIFT+RIGHT-ARROW, '^' is SHIFT+UP,
     * '?' is SHIFT+DOWN, '/' is SHIFT+7, '+' is SHIFT+5, '-' is SHIFT+6,
     * '=' is SHIFT+1. That mismatch is why the host keyboard is resolved by
     * CHARACTER (intvsession_ecs_key_from_char) rather than by key
     * position: typing '%' should not require knowing it lives on an arrow
     * key over here. */
    INTVSESSION_ECS_KEY_EQUAL = INTVSESSION_ECS_KEY_PHYSICAL_COUNT,
    INTVSESSION_ECS_KEY_QUOTE, INTVSESSION_ECS_KEY_HASH,
    INTVSESSION_ECS_KEY_DOLLAR, INTVSESSION_ECS_KEY_PLUS,
    INTVSESSION_ECS_KEY_MINUS, INTVSESSION_ECS_KEY_SLASH,
    INTVSESSION_ECS_KEY_STAR, INTVSESSION_ECS_KEY_LPAREN,
    INTVSESSION_ECS_KEY_RPAREN, INTVSESSION_ECS_KEY_CARET,
    INTVSESSION_ECS_KEY_QUEST, INTVSESSION_ECS_KEY_PCT,
    INTVSESSION_ECS_KEY_SQUOTE, INTVSESSION_ECS_KEY_COLON,
    INTVSESSION_ECS_KEY_GREATER, INTVSESSION_ECS_KEY_LESS,

    INTVSESSION_ECS_KEY_NONE, /* returned by intvsession_ecs_key_from_keysym
                              * for a keysym with no ECS keyboard mapping;
                              * not a real key -- never pass this to
                              * intvsession_ecs_key(). */
} intvsession_ecs_key;

void intvsession_ecs_key_set(intvsession *s, intvsession_ecs_key key,
                             int pressed);
void intvsession_ecs_keys_clear(intvsession *s);

/* One host key event in "ECS keyboard" mode -- what a frontend's key handler
 * should call instead of resolving and injecting itself.
 *
 * `keysym` and `ch` are intvsession_ecs_key_from_char's two arguments; see
 * that function for how a press resolves.
 *
 * `host_key` is the frontend's own raw identifier for the PHYSICAL key --
 * GDK's hardware keycode, Qt's nativeScanCode(), AppKit's keyCode, Win32's
 * VK -- and exists purely so a release can find what its own press
 * asserted. It cannot be `keysym`: a press and its release do not always
 * produce the same keysym. Two of the four frontends resolve a key with no
 * curated symbol of its own through the toolkit's text, which Shift moves
 * (Qt and AppKit both report "?" for the "/" key while Shift is held --
 * frontends/kde/KeyForward.cpp's scancode table and
 * frontends/macos/IntvKeyForward.m's keyCode table cover neither "/" nor
 * "'"), and those tables cannot simply be extended, because a curated
 * symbol is also a key's PERSISTED BINDING IDENTITY -- see the fallback
 * keysym bands' own comment on why a key must be reachable through exactly
 * one of them. Pass 0 if the frontend genuinely has no stable id (a
 * synthesized event); `keysym` is then used instead, which is right for
 * every key that has a curated symbol and merely no better than today's
 * behaviour for the rest.
 *
 * Why the bookkeeping exists at all: press Shift, press "5" and `ch` is
 * '%', which is ECS SHIFT+LEFT-ARROW; release Shift first and the "5"
 * key-up arrives as plain '5'. A release resolved afresh would clear ECS
 * "5" and leave the '%' key's fake-shift bit down -- and pads.c re-reads
 * every row's fake-shift bits on every scan, so one stranded bit leaves the
 * machine reading SHIFT on every later keystroke. intvsession_ecs_keys_clear
 * forgets every held key too, so a focus-loss clear cannot leave stale
 * entries behind. */
void intvsession_ecs_key_event(intvsession *s, uint32_t host_key,
                               uint32_t keysym, uint32_t ch, int down);

/* The hand-controller equivalent: releases every keypad/action key and both
 * discs on all four sides. A frontend should call BOTH on losing keyboard
 * focus -- the WM_KEYUP/key-release for whatever was held goes to whoever
 * took focus, so without this a held key stays down forever. (A gamepad
 * holding a direction is re-asserted on its next poll; a gamepad button held
 * across the focus change is released until its next press.) */
void intvsession_pads_clear(intvsession *s);

/* ---- audio ------------------------------------------------------------
 * jzIntv's PSG mixer produces mono int16 samples (see
 * core/jzintv/intv_audio.h); this is that stream, sample rate
 * INTVSESSION_AUDIO_RATE, drained on demand rather than pushed -- a
 * frontend pulls whatever has accumulated since the last call on its own
 * audio callback/thread. Returns the number of samples actually copied,
 * which may be less than max_samples (0 if nothing new has played). */
#define INTVSESSION_AUDIO_RATE 48000
int intvsession_render_audio(intvsession *s, int16_t *dst, int max_samples);

/* ---- keyboard -> pad mapping ----------------------------------------------
 * A pure function (no session state, no threading) shared by every frontend,
 * the same way cocosession's coco_key_from_event is: a frontend translates
 * its toolkit's key event to one of the symbols below, looks up the mapping,
 * and calls intvsession_pad_key/intvsession_pad_disc with the result. F10,
 * F11 and F12 are reserved for the frontends (movie/screenshot/debugger in
 * upstream jzIntv's own default bindings -- see src/cfg/mapping.c's
 * cfg_key_bind table) and are deliberately NOT in this table.
 *
 * Default bindings are copied from that same upstream table (its column 0,
 * the normal-play action map) wherever it assigns a pad-relevant action --
 * see intv_keymap.c for the exact source line each entry mirrors. Printable
 * keys use their ASCII value directly (matching upstream's own key naming,
 * which is one character for those keys); everything else is one of the
 * symbols below, a private range starting past any ASCII value.
 *
 * KNOWN LIMITATION: unlike upstream's event subsystem (which OR-combines
 * simultaneously-held direction keys into 16-way half-steps), each disc
 * mapping here simply overwrites the disc position -- there is no keyboard
 * equivalent of holding "UP" and "RIGHT" together to get NE. The 8 primary
 * compass directions are each reachable by a single key (arrows, or the
 * WASD/IJKM-style diagonal keys upstream also binds), so this only costs
 * true simultaneous-key diagonals, not any direction outright -- and a
 * keypad window's Map mode (core/src/bindings.c) can put a single key or
 * gamepad button directly on any of the 8 odd half-steps too, this table
 * just carries no *default* for them. */
typedef enum {
    INTVSESSION_KEYSYM_NONE = 0,
    INTVSESSION_KEYSYM_UP = 0x1000,
    INTVSESSION_KEYSYM_DOWN,
    INTVSESSION_KEYSYM_LEFT,
    INTVSESSION_KEYSYM_RIGHT,
    INTVSESSION_KEYSYM_KP_0, INTVSESSION_KEYSYM_KP_1, INTVSESSION_KEYSYM_KP_2,
    INTVSESSION_KEYSYM_KP_3, INTVSESSION_KEYSYM_KP_4, INTVSESSION_KEYSYM_KP_5,
    INTVSESSION_KEYSYM_KP_6, INTVSESSION_KEYSYM_KP_7, INTVSESSION_KEYSYM_KP_8,
    INTVSESSION_KEYSYM_KP_9,
    INTVSESSION_KEYSYM_KP_PERIOD,
    INTVSESSION_KEYSYM_KP_ENTER,
    INTVSESSION_KEYSYM_LSHIFT,
    INTVSESSION_KEYSYM_RSHIFT,
    INTVSESSION_KEYSYM_LCTRL,
    INTVSESSION_KEYSYM_RCTRL,
    INTVSESSION_KEYSYM_LALT,
    INTVSESSION_KEYSYM_RALT,
    /* Non-printable keys the base controller map has no use for, but the
     * ECS keyboard map (intvsession_ecs_key_from_keysym) does -- mapping.c's
     * "ESCAPE"/"RETURN"/"BACKSPACE" rows. Space, being printable ASCII
     * (0x20), needs no symbol of its own -- see that function. */
    INTVSESSION_KEYSYM_ESCAPE,
    INTVSESSION_KEYSYM_RETURN,
    INTVSESSION_KEYSYM_BACKSPACE,
} intvsession_keysym;

/* ---- fallback keysym bands -------------------------------------------------
 * The symbols above name only the keys the *default* controller and ECS
 * keyboard maps care about. A Map mode has to be able to capture ANY key the
 * host can deliver, including keys a normal PC keyboard has no cap for at
 * all: an Intellivision-to-USB adapter in keyboard mode (the Ultimate PC
 * Interface's ECS-keyboard and Music-Synthesizer personalities, say) emits
 * HID usages that Windows reports in its OEM-specific/unassigned VK bands
 * and that X11/AppKit have no named keysym for either. Before these bands
 * existed every such key translated to 0 and was silently swallowed by the
 * capture path -- the key simply could not be mapped.
 *
 * HID_BASE is the primary band: the USB HID Keyboard usage ID the key
 * actually came from. Every platform's own key numbering derives from that
 * table, so it is the one identifier all four frontends can agree on, it
 * names cleanly (see intvsession_keysym_name), and a persisted binding stays
 * meaningful across them. NATIVE_BASE is the last resort for a key that
 * arrives with no usable scancode at all (Windows hands some HID usages a VK
 * with MakeCode 0); it is platform-local by construction and named as raw
 * hex.
 *
 * Translation MUST try the symbols above first and fall into a band only on
 * a miss. A key that has a curated symbol must never ALSO be reachable
 * through a band, or the same physical key would bind under two different
 * values and a binding made through one path would not match the other.
 * These bands carry no default mapping (intvsession_key_from_keysym returns
 * MAP_NONE for them, by construction -- see bindings.c's
 * compute_defaults_locked, which only ever sweeps printable ASCII plus the
 * symbols above); they are reachable only through the bindings table. */
#define INTVSESSION_KEYSYM_HID_BASE    0x10000u /* + USB HID keyboard usage */
#define INTVSESSION_KEYSYM_NATIVE_BASE 0x20000u /* + the platform's own code */

/* Highest HID keyboard usage the band accepts (the usage page's own last
 * defined key, Keyboard Right GUI). Anything past it is not a keyboard key. */
#define INTVSESSION_HID_USAGE_MAX      0xE7u

/* Native key code -> USB HID Keyboard usage ID, one per platform's own
 * numbering. 0 when the code maps to no keyboard usage (which is the
 * caller's cue to fall back to INTVSESSION_KEYSYM_NATIVE_BASE).
 *
 * All three are compiled and tested on every platform, deliberately: the
 * maintainer has no Windows machine (see cmake/toolchains/mingw-w64.cmake's
 * own header), so the Windows table has to be verifiable from Linux CI or a
 * mistake in it is invisible until a user reports it -- which is exactly how
 * the bug these bands fix was found.
 *
 *   _win_scancode: the PS/2 set-1 code in WM_KEYDOWN's lParam bits 16-23,
 *                  with `extended` set from lParam bit 24 (0x01000000).
 *   _evdev:        the Linux evdev code -- GTK's gdk_key_event_get_keycode()
 *                  and Qt's QKeyEvent::nativeScanCode() both report it
 *                  biased by 8, so subtract 8 before calling.
 *   _macos:        AppKit's NSEvent.keyCode (Apple's own virtual key set). */
uint32_t intvsession_hid_from_win_scancode(unsigned scancode, int extended);
uint32_t intvsession_hid_from_evdev(unsigned code);
uint32_t intvsession_hid_from_macos_keycode(unsigned keycode);

/* Wraps a HID usage into the band, or 0 for usage 0 / out of range -- so a
 * frontend can write `keysym = intvsession_keysym_from_hid(usage)` and test
 * the result rather than open-coding the arithmetic and the range check. */
uint32_t intvsession_keysym_from_hid(uint32_t usage);

/* A machine-global action -- distinct from intvsession_key, which is always
 * per-side hardware the CP1610 reads through pad_t. A sysaction target's
 * .side is forced to INTVSESSION_PAD_LEFT by intvsession_target_sysaction
 * (bindings.c stores it in a slot band of its own, past the disc's 16
 * positions) but carries no meaning -- there is only one machine, not one
 * per side. See intvsession_sysaction_fire/_post/_take below for how a
 * bound input actually triggers one. */
typedef enum {
    INTVSESSION_SYSACT_RESET_GAME = 0,   /* intvsession_reset_game */
    INTVSESSION_SYSACT_RESET_CONFIG,     /* intvsession_reset_to_config */
    INTVSESSION_SYSACT_COUNT
} intvsession_sysaction;

typedef enum {
    INTVSESSION_MAP_NONE = 0, /* keysym has no pad mapping */
    INTVSESSION_MAP_KEY,      /* side/key valid -- call intvsession_pad_key */
    INTVSESSION_MAP_DISC,     /* side/direction valid -- call
                              * intvsession_pad_disc (direction is a clock
                              * position 0-15, never -1: -1 is only ever
                              * what a frontend sends on release, ANDed with
                              * its own "is anything else on this disc still
                              * held" bookkeeping if it wants combos). */
    INTVSESSION_MAP_SYSACT,   /* sysact valid -- call
                              * intvsession_sysaction_fire (keyboard/on-screen
                              * button) or intvsession_sysaction_post (SDL
                              * gamepad thread, see that function's own
                              * comment on why it can't fire inline). A
                              * frontend that doesn't know this kind yet is
                              * still safe: every existing if/else-if
                              * dispatch chain simply falls through and does
                              * nothing, matching an unmapped keysym. */
} intvsession_map_kind;

typedef struct {
    intvsession_map_kind kind;
    intvsession_pad_side side;
    intvsession_key      key;       /* valid iff kind == INTVSESSION_MAP_KEY */
    int                  direction; /* valid iff kind == INTVSESSION_MAP_DISC */
    intvsession_sysaction sysact;   /* valid iff kind == INTVSESSION_MAP_SYSACT
                                     * -- appended last so every existing
                                     * positional initializer of this struct
                                     * stays valid. */
} intvsession_key_mapping;

/* keysym: an ASCII value for a printable key, or one of the
 * INTVSESSION_KEYSYM_* symbols above. Case-insensitive for letters. Pure
 * function; unit-tested on its own (core/tests/keymap_test.c). */
intvsession_key_mapping intvsession_key_from_keysym(uint32_t keysym);

/* ECS keyboard equivalent of intvsession_key_from_keysym: same keysym
 * space, but mapped from cfg_key_bind[]'s column 3 ("ECS Keyboard setup")
 * instead of column 0, since the two modes can't both claim the host
 * keyboard at once -- a frontend switches which of these two functions it
 * calls based on its own "ECS keyboard focus" toggle. Returns
 * INTVSESSION_ECS_KEY_NONE when keysym has no ECS keyboard mapping;
 * F10/F11/F12 stay reserved for the frontends here too. Pure function;
 * unit-tested in core/tests/keymap_test.c. */
intvsession_ecs_key intvsession_ecs_key_from_keysym(uint32_t keysym);

/* The same, but read the way a key cap reads: `ch` is the character the host
 * layout actually produced (0 if none), `keysym` the shift-independent
 * identity of the same physical key.
 *
 * A printable `ch` wins, because the ECS's shifted layer is nothing like a
 * PC's -- ECS SHIFT+5 is '+', not '%' -- so passing the physical key plus a
 * SHIFT chord through positionally types the wrong character, and leaves
 * '/', '-', '=' and '+' unreachable entirely (the ECS has no unshifted key
 * for any of them). Resolving by character puts every symbol on the host key
 * that prints it, which is also what upstream jzIntv does: its own
 * cfg_key_bind[] binds the SHIFTED SDL keysyms to the KEYB_SLASH/KEYB_PLUS/
 * ... fake-shift actions (mapping.c's "ECS Keyboard 'Shifted' Keys" block).
 *
 * `keysym` is the fallback for every key that produces no character at all
 * -- arrows, Enter, Esc, the numeric keypad, Shift/Ctrl themselves, and Ctrl
 * combos, for which no toolkit reports text. Pure function; unit-tested in
 * core/tests/keymap_test.c. */
intvsession_ecs_key intvsession_ecs_key_from_char(uint32_t keysym,
                                                  uint32_t ch);

/* ---- remappable bindings (core/src/bindings.c) -----------------------------
 * intvsession_key_from_keysym above and the gamepad face buttons (core/src/
 * gamepad_sdl.c) are both fixed defaults transcribed from upstream jzIntv.
 * This layer sits above them: a table of "targets" -- the 15 keypad/action
 * buttons per side, the disc's 16 clock positions per side, AND the two
 * machine-global system actions (Reset Game, Reset to CONFIG) -- each with
 * one keyboard slot and one gamepad slot, seeded from those same defaults
 * (or, for the system actions, from Backspace/Escape -- see bindings.c's
 * compute_defaults_locked) and overridable at runtime, e.g. by a keypad
 * window's "Map" mode. Shared process-wide (jzIntv's own machine is a
 * process singleton, see core/jzintv/intv_host.h) and persisted in the
 * settings store, so a mapping chosen in one frontend is what every other
 * frontend sees.
 *
 * A target is exactly an intvsession_key_mapping (below): kind MAP_KEY names
 * a (side,key), kind MAP_DISC names a (side,direction), kind MAP_SYSACT
 * names a machine-global intvsession_sysaction (see that enum's own
 * comment). intvsession_target_key, _target_disc and _target_sysaction build
 * one of each; every other function in this section takes that struct
 * rather than a bare (side,key) pair so the same calls work for all three
 * kinds. */

/* Mirrors SDL3's SDL_GamepadButton numbering (gamepad_sdl.c translates
 * explicitly rather than casting) but declared here so this header, and
 * anything persisted through it, stays SDL-free. */
typedef enum {
    INTVSESSION_PAD_BTN_NONE = -1,
    INTVSESSION_PAD_BTN_SOUTH = 0, INTVSESSION_PAD_BTN_EAST,
    INTVSESSION_PAD_BTN_WEST, INTVSESSION_PAD_BTN_NORTH,
    INTVSESSION_PAD_BTN_BACK, INTVSESSION_PAD_BTN_GUIDE,
    INTVSESSION_PAD_BTN_START,
    INTVSESSION_PAD_BTN_LEFT_STICK, INTVSESSION_PAD_BTN_RIGHT_STICK,
    INTVSESSION_PAD_BTN_LEFT_SHOULDER, INTVSESSION_PAD_BTN_RIGHT_SHOULDER,
    INTVSESSION_PAD_BTN_DPAD_UP, INTVSESSION_PAD_BTN_DPAD_DOWN,
    INTVSESSION_PAD_BTN_DPAD_LEFT, INTVSESSION_PAD_BTN_DPAD_RIGHT,
    INTVSESSION_PAD_BTN_COUNT,

    /* ---- raw joystick bands ----------------------------------------------
     * The named buttons above are SDL's *gamepad* abstraction, which only
     * exists for devices SDL has a mapping for and which tops out at those
     * 15. An Intellivision-to-USB adapter is a plain HID joystick: the
     * Ultimate PC Interface reports ~20 buttons and all 16 disc positions
     * (jzIntv's own HACKFILE.CFG for it names JS0_BTN_00..JS0_BTN_19), and
     * one Intellivision controller alone is 12 keypad keys plus 3 action
     * buttons. So gamepad_sdl.c also opens un-mapped devices through
     * SDL_OpenJoystick and reports their inputs in these bands.
     *
     * Values, not enumerators, because the index is the identity: there is
     * nothing to name. They start past PAD_BTN_COUNT with room to spare so
     * adding a named gamepad button later cannot collide, and the persisted
     * form is a plain integer either way (see bindings.c's pack_locked), so
     * the settings format does not change. */
    INTVSESSION_PAD_BTN_RAW_BASE  = 64,  /* + raw button index, 0..63 */
    INTVSESSION_PAD_BTN_RAW_LAST  = 127,
    INTVSESSION_PAD_HAT_BASE      = 192, /* + hat * 8 + direction, see below */
    INTVSESSION_PAD_HAT_LAST      = 255
} intvsession_pad_button;

/* Hat directions within INTVSESSION_PAD_HAT_BASE, clockwise from North --
 * the 8 positions a HID hat switch can report. A hat's button value is
 * INTVSESSION_PAD_HAT_BASE + hat_index * 8 + direction. */
#define INTVSESSION_PAD_HAT_DIRS 8

/* True for a value in the corresponding band. Written as macros rather than
 * range checks at each use site because gamepad_sdl.c, bindings.c and the
 * frontends all need the same three tests. */
#define INTVSESSION_PAD_BTN_IS_NAMED(b) \
    ((b) >= 0 && (b) < INTVSESSION_PAD_BTN_COUNT)
#define INTVSESSION_PAD_BTN_IS_RAW(b) \
    ((b) >= INTVSESSION_PAD_BTN_RAW_BASE && (b) <= INTVSESSION_PAD_BTN_RAW_LAST)
#define INTVSESSION_PAD_BTN_IS_HAT(b) \
    ((b) >= INTVSESSION_PAD_HAT_BASE && (b) <= INTVSESSION_PAD_HAT_LAST)

typedef struct {
    uint32_t               keysym; /* 0 = no keyboard binding */
    intvsession_pad_button button; /* INTVSESSION_PAD_BTN_NONE = no gamepad
                                    * binding */
} intvsession_binding;

/* Builds a Map target naming a keypad/action button, or one of the disc's 16
 * clock positions (0-15, see intvsession_disc_from_point above). Pure
 * constructors -- no session needed. */
intvsession_key_mapping intvsession_target_key(intvsession_pad_side side,
                                               intvsession_key key);
intvsession_key_mapping intvsession_target_disc(intvsession_pad_side side,
                                                int direction);
/* .side is always INTVSESSION_PAD_LEFT (see intvsession_sysaction's own
 * comment) -- callers should not read it back out of the result. */
intvsession_key_mapping intvsession_target_sysaction(intvsession_sysaction a);

intvsession_binding intvsession_target_binding_get(intvsession *s,
        intvsession_key_mapping target);

/* Binds keysym/button to `target`, first clearing it from wherever it was
 * previously bound (a keysym/button drives exactly one target at a time).
 * If `stolen` is non-NULL, writes a description of what it was taken from
 * ("" if it was unbound) -- e.g. "Left disc East" or "Right 5" -- null
 * terminated within stolensz. Persists immediately. */
void intvsession_target_set_key(intvsession *s, intvsession_key_mapping target,
        uint32_t keysym, char *stolen, int stolensz);
void intvsession_target_set_button(intvsession *s, intvsession_key_mapping target,
        intvsession_pad_button button, char *stolen, int stolensz);

/* Restores every binding (keypad, action buttons, and disc alike) to its
 * upstream default and persists that. */
void intvsession_bindings_reset(intvsession *s);

/* Same contract as intvsession_key_from_keysym, but honouring the bindings
 * table: returns keysym's current target, MAP_KEY or MAP_DISC, wherever it
 * has been remapped to (including when keysym's *default* target has been
 * remapped away to a different input, unlike the pure function above which
 * would still report it), or MAP_NONE if keysym drives nothing at all.
 * Frontends should call this one; intvsession_key_from_keysym remains the
 * pure default-table lookup bindings.c itself seeds from. */
intvsession_key_mapping intvsession_key_from_keysym_bound(intvsession *s,
                                                          uint32_t keysym);

/* Human-readable names, for a Map mode's status line. keysym_name ALWAYS
 * writes dst (given dstsz > 0) -- the empty string for keysym 0, a
 * synthesized "HID 0x.." / "Key 0x.." for a fallback-band keysym with no
 * name of its own -- and returns the string length, so 0 means "no key" and
 * never "dst left untouched". (It used to leave dst alone for an unnamed
 * keysym, which every caller then printed as uninitialised stack.)
 *
 * pad_button_name returns a NUL-terminated static for a named gamepad
 * button. The raw joystick bands have no fixed names, so those are formatted
 * ("Button 17", "Hat 1 NE") into a thread-local buffer that the NEXT
 * pad_button_name call on the same thread overwrites -- copy the string, do
 * not hold the pointer. Thread-local rather than plain static because
 * gamepad_sdl.c's polling thread names buttons too, concurrently with a
 * frontend's Map mode on the UI thread. Never NULL either way. */
int         intvsession_keysym_name(uint32_t keysym, char *dst, int dstsz);
const char *intvsession_pad_button_name(intvsession_pad_button button);
const char *intvsession_pad_key_name(intvsession_key key);        /* "5", "Clear", "Top" */
const char *intvsession_pad_side_name(intvsession_pad_side side); /* "Left" */
/* "East", "ENE", "Northeast", ... one of the 16 compass/half-step names for
 * a disc clock position (0-15); "?" out of range. */
const char *intvsession_disc_dir_name(int direction);
/* "Reset Game" / "Reset to CONFIG"; "?" out of range. */
const char *intvsession_sysaction_name(intvsession_sysaction a);

/* Names a target itself, e.g. "Left 5", "Left disc East", "Right disc ENE",
 * or a bare sysaction name ("Reset Game") with no side prefix, since a
 * sysaction isn't per-side; "" for a MAP_NONE target. Returns the string
 * length. */
int intvsession_target_name(intvsession_key_mapping target, char *dst,
                            int dstsz);

/* Describes a full binding pair, e.g. "Numpad 5 / Gamepad A", "Numpad 5",
 * "Gamepad A", or "nothing" if both slots are empty. Returns the string
 * length. */
int intvsession_binding_describe(intvsession_binding b, char *dst, int dstsz);

/* ---- back-compat wrappers, (side,key) only ---------------------------------
 * Thin shims over the target-based API above, scoped to MAP_KEY targets --
 * kept for callers that only ever dealt with keypad/action buttons, never the
 * disc. New code should prefer the intvsession_target_* functions, which work
 * for both. */
intvsession_binding intvsession_binding_get(intvsession *s,
        intvsession_pad_side side, intvsession_key key);
void intvsession_binding_set_key(intvsession *s, intvsession_pad_side side,
        intvsession_key key, uint32_t keysym, char *stolen, int stolensz);
void intvsession_binding_set_button(intvsession *s, intvsession_pad_side side,
        intvsession_key key, intvsession_pad_button button,
        char *stolen, int stolensz);

/* ---- gamepad capture (for a Map mode's "press a gamepad button" step) -----
 * SDL's gamepad events arrive on gamepad_sdl.c's own background thread, which
 * cannot safely call into a UI toolkit -- so capture is poll-based instead of
 * callback-based. While armed, button events are recorded here instead of
 * being injected into the emulated machine, so a mapping press/release can't
 * leak through to whatever pad key the button used to drive. */
void intvsession_gamepad_capture_begin(intvsession *s);
/* Disarms without consuming a result -- call when a Map sequence is aborted
 * or the window closes, so a stray later press doesn't resume mid-capture. */
void intvsession_gamepad_capture_cancel(intvsession *s);
/* 1 and writes *button once some gamepad button has been pressed since
 * capture began (also disarms); 0 while still waiting. Poll on a short
 * timer. */
int intvsession_gamepad_capture_poll(intvsession *s,
                                     intvsession_pad_button *button);

/* ---- system actions (core/src/session.c) -------------------------------
 * Dispatches a MAP_SYSACT target's effect. Safe to call from any thread that
 * is allowed to block briefly and is not itself the emulator or gamepad
 * thread (RESET_CONFIG's intvsession_stop joins both) -- i.e. a UI thread,
 * on a keypress or a keypad-window button click. Returns whatever the
 * underlying call returns (0 success, -1 + intvsession_last_error() on
 * failure); -1 for an out-of-range action. */
int intvsession_sysaction_fire(intvsession *s, intvsession_sysaction a);

/* Cross-thread queue for exactly the one caller that can't use
 * intvsession_sysaction_fire directly: gamepad_sdl.c's background SDL
 * thread, for which firing RESET_CONFIG inline would join itself (see
 * intv_gamepad_stop). _post queues a; a frontend's own UI-thread timer
 * drains the queue with _take (returns 1 and writes *out for one pending
 * action, 0 once empty -- call in a loop) and calls _fire from there
 * instead. Multiple distinct actions posted before a drain are each
 * delivered once, not coalesced. */
void intvsession_sysaction_post(intvsession *s, intvsession_sysaction a);
int  intvsession_sysaction_take(intvsession *s, intvsession_sysaction *out);

/* ---- FujiNet ---------------------------------------------------------
 * The runtime (libfujinet.{so,dylib}/fujinet.dll, built by
 * cmake/FujiNetRuntime.cmake -DWITH_FUJINET=ON and dlopen'd at run time --
 * see core/src/fujinet_runtime.c) is a BoIP TCP *server*: jzIntv's own
 * --fujinet flag (see core/jzintv/intv_host.c) connects out to it on
 * INTVSESSION_BOIP_PORT, same direction as the CoCo port's Becker link and
 * the opposite of ADAM/Apple II. intvsession_start() starts FujiNet first
 * (and waits for its listener to come up) for exactly the reason CoCo's own
 * fujinet_wait_for_becker documents: starting the emulator before the
 * listener is live means its first connection attempt finds nothing.
 * Not fatal if unavailable (-DWITH_FUJINET=OFF, or the runtime failed to
 * load) -- the machine still boots the embedded config ROM either way,
 * jzIntv's own --fujinet peripheral simply keeps retrying the connection
 * non-blockingly (see core/jzintv/intv_host.h's own comment on that). */
int         intvsession_fujinet_running(const intvsession *s);
const char *intvsession_fujinet_webui_url(const intvsession *s);
/* Copies the most recent FujiNet console output (NUL-terminated) into dst;
 * returns the number of bytes written (excluding the NUL). */
int         intvsession_fujinet_copy_log(intvsession *s, char *dst, int max);

/* ---- gamepads (SDL, hotplug; started/stopped with the session) ----------
 * Left stick drives the disc; SOUTH/EAST/WEST face buttons drive the three
 * action buttons. No keypad digit mapping (see core/src/gamepad_sdl.c for
 * why). Automatic side assignment: the first connected pad drives the left
 * controller, the second the right, until intvsession_gamepad_assign pins
 * one explicitly (side: 0/INTVSESSION_PAD_LEFT, 1/INTVSESSION_PAD_RIGHT, or
 * -1 to restore automatic assignment). */
int  intvsession_gamepad_count(intvsession *s);
/* Name of connected pad idx into dst; returns the name's length (0 if idx
 * is out of range). */
int  intvsession_gamepad_name(intvsession *s, int idx, char *dst, int dstsz);
void intvsession_gamepad_assign(intvsession *s, int idx, int side);

/* What intvsession_gamepad_assign was last told for pad idx: an
 * intvsession_pad_side, or -1 for automatic (the default) -- so a settings
 * UI can show the current choice rather than guess at it. */
int  intvsession_gamepad_assignment(intvsession *s, int idx);

/* Which side pad idx is ACTUALLY driving right now, explicit assignment and
 * automatic fallback resolved together, or -1 if it drives nothing. This is
 * the one a user needs to see: "Automatic" alone does not answer "is my
 * adapter the left controller or the right one?". */
int  intvsession_gamepad_effective_side(intvsession *s, int idx);

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
