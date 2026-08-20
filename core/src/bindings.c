/*
 * bindings -- the remappable layer above intv_keymap.c's fixed keyboard
 * table and gamepad_sdl.c's fixed face-button table. See intvsession.h's own
 * "remappable bindings" section for the contract; this file owns the
 * process-global table backing it (one intvsession_binding -- a keyboard
 * keysym slot and a gamepad button slot -- per (side, pad key)) plus the
 * settings-store persistence and the human-readable name tables a Map mode
 * needs for its status line.
 *
 * Two tables, not one: s_defaults is computed once per bindings_init call and
 * never mutated again, s_table is the live, user-editable copy. Diffing the
 * two is what makes persistence (only non-default entries are written) and
 * reset (copy defaults back over the live table) both trivial, and is why
 * intvsession_key_from_keysym_bound has to fall back to the pure
 * intvsession_key_from_keysym only for the disc case: every keyboard MAP_KEY
 * default is already sitting in s_table from the moment bindings_init ran,
 * so a plain table scan finds it wherever it currently lives, moved or not.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "bindings.h"

#define NUM_SIDES 4
#define PACKED_BUF_SIZE 4096

static intvsession_binding s_defaults[NUM_SIDES][INTVSESSION_KEY_COUNT];
static intvsession_binding s_table[NUM_SIDES][INTVSESSION_KEY_COUNT];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Compass names for the fixed disc, indexed by direction/2 -- direction is
 * always one of the 8 even clock positions here (0=E .. 14=SE), matching
 * intv_keymap.c's own DIR_* enum; the odd half-steps are unreachable from a
 * keyboard at all (see intvsession.h's own comment on that). */
static const char *const disc_dir_names[8] = {
    "East", "Northeast", "North", "Northwest",
    "West", "Southwest", "South", "Southeast",
};

/* ---- defaults --------------------------------------------------------- */

/* Every INTVSESSION_KEYSYM_* the pad map (as opposed to the ECS keyboard
 * map) has any use for -- see intv_keymap.c's own switch. Explicit rather
 * than assuming the enum's values are contiguous from
 * INTVSESSION_KEYSYM_UP: safer against that enum being reordered or
 * extended later, and it costs nothing since this list is only walked once
 * per bindings_init. */
static const uint32_t special_keysyms[] = {
    INTVSESSION_KEYSYM_UP, INTVSESSION_KEYSYM_DOWN,
    INTVSESSION_KEYSYM_LEFT, INTVSESSION_KEYSYM_RIGHT,
    INTVSESSION_KEYSYM_KP_0, INTVSESSION_KEYSYM_KP_1, INTVSESSION_KEYSYM_KP_2,
    INTVSESSION_KEYSYM_KP_3, INTVSESSION_KEYSYM_KP_4, INTVSESSION_KEYSYM_KP_5,
    INTVSESSION_KEYSYM_KP_6, INTVSESSION_KEYSYM_KP_7, INTVSESSION_KEYSYM_KP_8,
    INTVSESSION_KEYSYM_KP_9,
    INTVSESSION_KEYSYM_KP_PERIOD, INTVSESSION_KEYSYM_KP_ENTER,
    INTVSESSION_KEYSYM_LSHIFT, INTVSESSION_KEYSYM_RSHIFT,
    INTVSESSION_KEYSYM_LCTRL, INTVSESSION_KEYSYM_RCTRL,
    INTVSESSION_KEYSYM_LALT, INTVSESSION_KEYSYM_RALT,
    INTVSESSION_KEYSYM_ESCAPE, INTVSESSION_KEYSYM_RETURN,
    INTVSESSION_KEYSYM_BACKSPACE,
};

/* Walks every keysym intvsession_key_from_keysym could possibly resolve --
 * the printable ASCII range plus the private special-keysym range above --
 * and stores each MAP_KEY result's keysym into its (side,key) slot. There is
 * deliberately no second, hand-transcribed copy of intv_keymap.c's table
 * here: this derives the defaults from the one pure function that already
 * carries them, so the two cannot drift apart. MAP_DISC results are skipped
 * (the disc is not remappable -- see intvsession.h) and so is MAP_NONE. */
static void compute_defaults_locked(void)
{
    uint32_t c;
    size_t i;
    int side, key;

    memset(s_defaults, 0, sizeof(s_defaults));
    for (side = 0; side < NUM_SIDES; side++)
        for (key = 0; key < INTVSESSION_KEY_COUNT; key++)
            s_defaults[side][key].button = INTVSESSION_PAD_BTN_NONE;

    for (c = 0x20; c <= 0x7E; c++) {
        intvsession_key_mapping m = intvsession_key_from_keysym(c);
        if (m.kind == INTVSESSION_MAP_KEY)
            s_defaults[m.side][m.key].keysym = c;
    }
    for (i = 0; i < sizeof(special_keysyms) / sizeof(special_keysyms[0]); i++) {
        intvsession_key_mapping m =
            intvsession_key_from_keysym(special_keysyms[i]);
        if (m.kind == INTVSESSION_MAP_KEY)
            s_defaults[m.side][m.key].keysym = special_keysyms[i];
    }

    /* Gamepad defaults: every side's own SOUTH/EAST/WEST face buttons drive
     * that side's three action buttons -- the historical hardcoded mapping
     * in gamepad_sdl.c's handle_button, now just this table's starting
     * point instead of the only answer. Uniform across all four sides
     * because a physical gamepad's face buttons mean the same thing
     * regardless of which side it ends up bound to. */
    for (side = 0; side < NUM_SIDES; side++) {
        s_defaults[side][INTVSESSION_ACTION_TOP].button =
            INTVSESSION_PAD_BTN_SOUTH;
        s_defaults[side][INTVSESSION_ACTION_LOWER_RIGHT].button =
            INTVSESSION_PAD_BTN_EAST;
        s_defaults[side][INTVSESSION_ACTION_LOWER_LEFT].button =
            INTVSESSION_PAD_BTN_WEST;
    }
}

static void reset_to_defaults_locked(void)
{
    memcpy(s_table, s_defaults, sizeof(s_table));
}

/* ---- persistence -------------------------------------------------------
 * One packed settings key ("bindings"), only the entries that differ from
 * s_defaults: "<side>.<key>.k:<keysym>" for a keyboard override,
 * "<side>.<key>.p:<button>" for a gamepad one, ';'-separated. An entry whose
 * value is 0 (keysym) or -1 (button, INTVSESSION_PAD_BTN_NONE) means
 * "explicitly unbound", distinct from "not listed" (still default) -- that
 * distinction matters when a slot's default input was stolen by some other
 * target and the slot itself was never given a replacement. */

static void pack_locked(char *buf, size_t bufsz)
{
    int side, key;
    size_t used = 0;

    if (bufsz > 0)
        buf[0] = '\0';
    for (side = 0; side < NUM_SIDES; side++) {
        for (key = 0; key < INTVSESSION_KEY_COUNT; key++) {
            const intvsession_binding *cur = &s_table[side][key];
            const intvsession_binding *def = &s_defaults[side][key];
            int n;

            if (cur->keysym != def->keysym) {
                n = snprintf(buf + used, used < bufsz ? bufsz - used : 0,
                            "%s%d.%d.k:%u", used ? ";" : "", side, key,
                            (unsigned)cur->keysym);
                if (n > 0)
                    used += (size_t)n;
            }
            if (cur->button != def->button) {
                n = snprintf(buf + used, used < bufsz ? bufsz - used : 0,
                            "%s%d.%d.p:%d", used ? ";" : "", side, key,
                            (int)cur->button);
                if (n > 0)
                    used += (size_t)n;
            }
        }
    }
}

static void apply_persisted_locked(struct intvsession *s)
{
    char buf[PACKED_BUF_SIZE];
    char *tok, *saveptr = NULL;
    const char *packed = intvsession_get_str(s, "bindings", "");

    if (!packed || !packed[0])
        return;
    snprintf(buf, sizeof(buf), "%s", packed);

    for (tok = strtok_r(buf, ";", &saveptr); tok;
        tok = strtok_r(NULL, ";", &saveptr)) {
        int side, key;
        char kind;
        long val;

        if (sscanf(tok, "%d.%d.%c:%ld", &side, &key, &kind, &val) != 4)
            continue;
        if (side < 0 || side >= NUM_SIDES || key < 0 ||
            key >= INTVSESSION_KEY_COUNT)
            continue;
        if (kind == 'k')
            s_table[side][key].keysym = (uint32_t)val;
        else if (kind == 'p')
            s_table[side][key].button = (intvsession_pad_button)val;
    }
}

static void persist_locked_then(struct intvsession *s)
{
    char buf[PACKED_BUF_SIZE];
    pack_locked(buf, sizeof(buf));
    pthread_mutex_unlock(&s_mtx);
    intvsession_set_str(s, "bindings", buf);
}

void bindings_init(struct intvsession *s)
{
    pthread_mutex_lock(&s_mtx);
    compute_defaults_locked();
    reset_to_defaults_locked();
    apply_persisted_locked(s);
    pthread_mutex_unlock(&s_mtx);
}

/* ---- lookups ------------------------------------------------------------- */

static intvsession_key_mapping none_mapping(void)
{
    intvsession_key_mapping m = { INTVSESSION_MAP_NONE, 0, 0, 0 };
    return m;
}

/* keysym's current target: a table scan (finds a MAP_KEY default wherever it
 * has been moved to, or a non-default override), falling back to the pure
 * default table only to recognise a still-fixed disc keysym -- see the file
 * header on why a MAP_KEY result from that fallback means "moved away", not
 * "still here". Caller holds s_mtx. */
static intvsession_key_mapping resolve_keysym_locked(uint32_t keysym)
{
    int side, key;
    intvsession_key_mapping def;

    if (keysym != 0) {
        for (side = 0; side < NUM_SIDES; side++) {
            for (key = 0; key < INTVSESSION_KEY_COUNT; key++) {
                if (s_table[side][key].keysym == keysym) {
                    intvsession_key_mapping m;
                    m.kind = INTVSESSION_MAP_KEY;
                    m.side = (intvsession_pad_side)side;
                    m.key = (intvsession_key)key;
                    m.direction = 0;
                    return m;
                }
            }
        }
    }
    def = intvsession_key_from_keysym(keysym);
    return def.kind == INTVSESSION_MAP_DISC ? def : none_mapping();
}

intvsession_key_mapping intvsession_key_from_keysym_bound(intvsession *s,
                                                          uint32_t keysym)
{
    intvsession_key_mapping m;
    (void)s;
    pthread_mutex_lock(&s_mtx);
    m = resolve_keysym_locked(keysym);
    pthread_mutex_unlock(&s_mtx);
    return m;
}

intvsession_binding intvsession_binding_get(intvsession *s,
        intvsession_pad_side side, intvsession_key key)
{
    intvsession_binding b = { 0, INTVSESSION_PAD_BTN_NONE };
    (void)s;
    if (side < 0 || side >= NUM_SIDES || key < 0 || key >= INTVSESSION_KEY_COUNT)
        return b;
    pthread_mutex_lock(&s_mtx);
    b = s_table[side][key];
    pthread_mutex_unlock(&s_mtx);
    return b;
}

intvsession_key_mapping bindings_key_from_button(intvsession_pad_side side,
                                                  intvsession_pad_button button)
{
    int key;

    if (side < 0 || side >= NUM_SIDES || button == INTVSESSION_PAD_BTN_NONE)
        return none_mapping();
    pthread_mutex_lock(&s_mtx);
    for (key = 0; key < INTVSESSION_KEY_COUNT; key++) {
        if (s_table[side][key].button == button) {
            intvsession_key_mapping m;
            m.kind = INTVSESSION_MAP_KEY;
            m.side = side;
            m.key = (intvsession_key)key;
            m.direction = 0;
            pthread_mutex_unlock(&s_mtx);
            return m;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return none_mapping();
}

int bindings_button_is_free(intvsession_pad_side side,
                            intvsession_pad_button button)
{
    return bindings_key_from_button(side, button).kind == INTVSESSION_MAP_NONE;
}

/* ---- names --------------------------------------------------------------- */

const char *intvsession_pad_side_name(intvsession_pad_side side)
{
    switch (side) {
    case INTVSESSION_PAD_LEFT:      return "Left";
    case INTVSESSION_PAD_RIGHT:     return "Right";
    case INTVSESSION_PAD_ECS_LEFT:  return "ECS Left";
    case INTVSESSION_PAD_ECS_RIGHT: return "ECS Right";
    default:                       return "?";
    }
}

/* Matches the button captions the keypad windows already draw (GNOME/KDE/
 * macOS/Windows all use "Top"/"L-Lower"/"R-Lower" -- see e.g.
 * frontends/gnome/keypad/keypad_window.c's own digits[]/action row). */
const char *intvsession_pad_key_name(intvsession_key key)
{
    switch (key) {
    case INTVSESSION_KEY_0:            return "0";
    case INTVSESSION_KEY_1:            return "1";
    case INTVSESSION_KEY_2:            return "2";
    case INTVSESSION_KEY_3:            return "3";
    case INTVSESSION_KEY_4:            return "4";
    case INTVSESSION_KEY_5:            return "5";
    case INTVSESSION_KEY_6:            return "6";
    case INTVSESSION_KEY_7:            return "7";
    case INTVSESSION_KEY_8:            return "8";
    case INTVSESSION_KEY_9:            return "9";
    case INTVSESSION_KEY_CLEAR:        return "Clear";
    case INTVSESSION_KEY_ENTER:        return "Enter";
    case INTVSESSION_ACTION_TOP:       return "Top";
    case INTVSESSION_ACTION_LOWER_LEFT:  return "L-Lower";
    case INTVSESSION_ACTION_LOWER_RIGHT: return "R-Lower";
    default:                          return "?";
    }
}

/* Xbox-style face-button letters -- SDL3's own SOUTH/EAST/WEST/NORTH names
 * are physical-position, not brand, but "A"/"B"/"X"/"Y" is the label most
 * players will recognise regardless of which pad they actually own. */
const char *intvsession_pad_button_name(intvsession_pad_button button)
{
    switch (button) {
    case INTVSESSION_PAD_BTN_SOUTH:          return "A";
    case INTVSESSION_PAD_BTN_EAST:           return "B";
    case INTVSESSION_PAD_BTN_WEST:           return "X";
    case INTVSESSION_PAD_BTN_NORTH:          return "Y";
    case INTVSESSION_PAD_BTN_BACK:           return "Back";
    case INTVSESSION_PAD_BTN_GUIDE:          return "Guide";
    case INTVSESSION_PAD_BTN_START:          return "Start";
    case INTVSESSION_PAD_BTN_LEFT_STICK:     return "Left Stick";
    case INTVSESSION_PAD_BTN_RIGHT_STICK:    return "Right Stick";
    case INTVSESSION_PAD_BTN_LEFT_SHOULDER:  return "Left Shoulder";
    case INTVSESSION_PAD_BTN_RIGHT_SHOULDER: return "Right Shoulder";
    case INTVSESSION_PAD_BTN_DPAD_UP:        return "D-Pad Up";
    case INTVSESSION_PAD_BTN_DPAD_DOWN:      return "D-Pad Down";
    case INTVSESSION_PAD_BTN_DPAD_LEFT:      return "D-Pad Left";
    case INTVSESSION_PAD_BTN_DPAD_RIGHT:     return "D-Pad Right";
    default:                                return "?";
    }
}

int intvsession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    char letter[2];
    const char *name = NULL;

    if (keysym >= 0x20 && keysym <= 0x7E) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if (c == ' ') {
            name = "Space";
        } else {
            letter[0] = c;
            letter[1] = '\0';
            name = letter;
        }
    } else {
        switch (keysym) {
        case INTVSESSION_KEYSYM_UP:        name = "Up Arrow"; break;
        case INTVSESSION_KEYSYM_DOWN:      name = "Down Arrow"; break;
        case INTVSESSION_KEYSYM_LEFT:      name = "Left Arrow"; break;
        case INTVSESSION_KEYSYM_RIGHT:     name = "Right Arrow"; break;
        case INTVSESSION_KEYSYM_KP_0:      name = "Numpad 0"; break;
        case INTVSESSION_KEYSYM_KP_1:      name = "Numpad 1"; break;
        case INTVSESSION_KEYSYM_KP_2:      name = "Numpad 2"; break;
        case INTVSESSION_KEYSYM_KP_3:      name = "Numpad 3"; break;
        case INTVSESSION_KEYSYM_KP_4:      name = "Numpad 4"; break;
        case INTVSESSION_KEYSYM_KP_5:      name = "Numpad 5"; break;
        case INTVSESSION_KEYSYM_KP_6:      name = "Numpad 6"; break;
        case INTVSESSION_KEYSYM_KP_7:      name = "Numpad 7"; break;
        case INTVSESSION_KEYSYM_KP_8:      name = "Numpad 8"; break;
        case INTVSESSION_KEYSYM_KP_9:      name = "Numpad 9"; break;
        case INTVSESSION_KEYSYM_KP_PERIOD: name = "Numpad ."; break;
        case INTVSESSION_KEYSYM_KP_ENTER:  name = "Numpad Enter"; break;
        case INTVSESSION_KEYSYM_LSHIFT:    name = "Left Shift"; break;
        case INTVSESSION_KEYSYM_RSHIFT:    name = "Right Shift"; break;
        case INTVSESSION_KEYSYM_LCTRL:     name = "Left Ctrl"; break;
        case INTVSESSION_KEYSYM_RCTRL:     name = "Right Ctrl"; break;
        case INTVSESSION_KEYSYM_LALT:      name = "Left Alt"; break;
        case INTVSESSION_KEYSYM_RALT:      name = "Right Alt"; break;
        case INTVSESSION_KEYSYM_ESCAPE:    name = "Escape"; break;
        case INTVSESSION_KEYSYM_RETURN:    name = "Return"; break;
        case INTVSESSION_KEYSYM_BACKSPACE: name = "Backspace"; break;
        default:                          name = NULL; break;
        }
    }
    if (!name)
        return 0;
    if (dst && dstsz > 0)
        snprintf(dst, (size_t)dstsz, "%s", name);
    return (int)strlen(name);
}

static void describe_mapping_locked(intvsession_key_mapping m, char *dst,
                                    int dstsz)
{
    if (!dst || dstsz <= 0)
        return;
    if (m.kind == INTVSESSION_MAP_KEY)
        snprintf(dst, (size_t)dstsz, "%s %s", intvsession_pad_side_name(m.side),
                intvsession_pad_key_name(m.key));
    else if (m.kind == INTVSESSION_MAP_DISC)
        snprintf(dst, (size_t)dstsz, "%s disc %s",
                intvsession_pad_side_name(m.side),
                disc_dir_names[m.direction / 2]);
    else
        dst[0] = '\0';
}

int intvsession_binding_describe(intvsession_binding b, char *dst, int dstsz)
{
    char keybuf[64];
    char combined[160];
    int have_key = b.keysym != 0 &&
                   intvsession_keysym_name(b.keysym, keybuf, sizeof(keybuf)) > 0;
    int have_btn = b.button != INTVSESSION_PAD_BTN_NONE;

    if (have_key && have_btn)
        snprintf(combined, sizeof(combined), "%s / Gamepad %s", keybuf,
                intvsession_pad_button_name(b.button));
    else if (have_key)
        snprintf(combined, sizeof(combined), "%s", keybuf);
    else if (have_btn)
        snprintf(combined, sizeof(combined), "Gamepad %s",
                intvsession_pad_button_name(b.button));
    else
        snprintf(combined, sizeof(combined), "nothing");

    if (dst && dstsz > 0)
        snprintf(dst, (size_t)dstsz, "%s", combined);
    return (int)strlen(combined);
}

/* ---- mutation -------------------------------------------------------------
 * Both setters follow the same shape: resolve what (if anything) currently
 * owns the input being bound, describe that for `stolen`, clear it from
 * there if it was a live pad-key slot, write the new binding, then persist
 * outside the lock (persist_locked_then unlocks s_mtx itself -- see its own
 * comment). Re-binding an input to the slot it already occupies is treated
 * as "nothing stolen" rather than reporting a button stealing from itself. */

void intvsession_binding_set_key(intvsession *s, intvsession_pad_side side,
        intvsession_key key, uint32_t keysym, char *stolen, int stolensz)
{
    intvsession_key_mapping cur;

    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (side < 0 || side >= NUM_SIDES || key < 0 || key >= INTVSESSION_KEY_COUNT)
        return;

    pthread_mutex_lock(&s_mtx);
    cur = resolve_keysym_locked(keysym);
    if (cur.kind == INTVSESSION_MAP_KEY && cur.side == side && cur.key == key)
        cur = none_mapping();
    describe_mapping_locked(cur, stolen, stolensz);
    if (cur.kind == INTVSESSION_MAP_KEY)
        s_table[cur.side][cur.key].keysym = 0;
    s_table[side][key].keysym = keysym;
    persist_locked_then(s); /* unlocks s_mtx */
}

void intvsession_binding_set_button(intvsession *s, intvsession_pad_side side,
        intvsession_key key, intvsession_pad_button button,
        char *stolen, int stolensz)
{
    int cur_key = -1;
    int k;

    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (side < 0 || side >= NUM_SIDES || key < 0 || key >= INTVSESSION_KEY_COUNT)
        return;

    pthread_mutex_lock(&s_mtx);
    if (button != INTVSESSION_PAD_BTN_NONE) {
        for (k = 0; k < INTVSESSION_KEY_COUNT; k++) {
            if (s_table[side][k].button == button) {
                cur_key = k;
                break;
            }
        }
    }
    if (cur_key == (int)key)
        cur_key = -1;
    if (cur_key >= 0 && stolen && stolensz > 0)
        snprintf(stolen, (size_t)stolensz, "%s %s",
                intvsession_pad_side_name(side),
                intvsession_pad_key_name((intvsession_key)cur_key));
    if (cur_key >= 0)
        s_table[side][cur_key].button = INTVSESSION_PAD_BTN_NONE;
    s_table[side][key].button = button;
    persist_locked_then(s); /* unlocks s_mtx */
}

void intvsession_bindings_reset(intvsession *s)
{
    pthread_mutex_lock(&s_mtx);
    reset_to_defaults_locked();
    pthread_mutex_unlock(&s_mtx);
    intvsession_set_str(s, "bindings", "");
}
