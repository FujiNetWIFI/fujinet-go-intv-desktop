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
    INTVSESSION_ECS_KEY_NONE, /* returned by intvsession_ecs_key_from_keysym
                              * for a keysym with no ECS keyboard mapping;
                              * not a real key -- never pass this to
                              * intvsession_ecs_key(). */
} intvsession_ecs_key;

void intvsession_ecs_key_set(intvsession *s, intvsession_ecs_key key,
                             int pressed);
void intvsession_ecs_keys_clear(intvsession *s);

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

typedef enum {
    INTVSESSION_MAP_NONE = 0, /* keysym has no pad mapping */
    INTVSESSION_MAP_KEY,      /* side/key valid -- call intvsession_pad_key */
    INTVSESSION_MAP_DISC,     /* side/direction valid -- call
                              * intvsession_pad_disc (direction is a clock
                              * position 0-15, never -1: -1 is only ever
                              * what a frontend sends on release, ANDed with
                              * its own "is anything else on this disc still
                              * held" bookkeeping if it wants combos). */
} intvsession_map_kind;

typedef struct {
    intvsession_map_kind kind;
    intvsession_pad_side side;
    intvsession_key      key;       /* valid iff kind == INTVSESSION_MAP_KEY */
    int                  direction; /* valid iff kind == INTVSESSION_MAP_DISC */
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

/* ---- remappable bindings (core/src/bindings.c) -----------------------------
 * intvsession_key_from_keysym above and the gamepad face buttons (core/src/
 * gamepad_sdl.c) are both fixed defaults transcribed from upstream jzIntv.
 * This layer sits above them: a table of "targets" -- the 15 keypad/action
 * buttons per side AND the disc's 16 clock positions per side, each with one
 * keyboard slot and one gamepad slot -- seeded from those same defaults and
 * overridable at runtime, e.g. by a keypad window's "Map" mode. Shared
 * process-wide (jzIntv's own machine is a process singleton, see
 * core/jzintv/intv_host.h) and persisted in the settings store, so a mapping
 * chosen in one frontend is what every other frontend sees.
 *
 * A target is exactly an intvsession_key_mapping (below): kind MAP_KEY names
 * a (side,key), kind MAP_DISC names a (side,direction). intvsession_target_key
 * and intvsession_target_disc build one of each; every other function in this
 * section takes that struct rather than a bare (side,key) pair so the same
 * calls work for both kinds. */

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
    INTVSESSION_PAD_BTN_COUNT
} intvsession_pad_button;

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

/* Human-readable names, for a Map mode's status line. keysym_name returns the
 * string length (0 for an unnamed keysym, dst untouched); the rest are
 * NUL-terminated statics, never NULL. */
int         intvsession_keysym_name(uint32_t keysym, char *dst, int dstsz);
const char *intvsession_pad_button_name(intvsession_pad_button button);
const char *intvsession_pad_key_name(intvsession_key key);        /* "5", "Clear", "Top" */
const char *intvsession_pad_side_name(intvsession_pad_side side); /* "Left" */
/* "East", "ENE", "Northeast", ... one of the 16 compass/half-step names for
 * a disc clock position (0-15); "?" out of range. */
const char *intvsession_disc_dir_name(int direction);

/* Names a target itself, e.g. "Left 5", "Left disc East", "Right disc ENE";
 * "" for a MAP_NONE target. Returns the string length. */
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
