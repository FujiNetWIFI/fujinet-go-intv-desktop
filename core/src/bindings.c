/*
 * bindings -- the remappable layer above intv_keymap.c's fixed keyboard
 * table and gamepad_sdl.c's fixed face-button table. See intvsession.h's own
 * "remappable bindings" section for the contract; this file owns the
 * process-global table backing it (one intvsession_binding -- a keyboard
 * keysym slot and a gamepad button slot -- per (side, target)) plus the
 * settings-store persistence and the human-readable name tables a Map mode
 * needs for its status line.
 *
 * A "target" is one of the 15 keypad/action buttons or one of the disc's 16
 * clock positions, per side, or one of the machine-global system actions
 * (Reset Game, Reset to CONFIG) -- see intvsession_key_mapping and
 * intvsession_target_key/_target_disc/_target_sysaction in intvsession.h.
 * Internally all three kinds live in the same per-side array: keypad/action
 * buttons at slots 0..INTVSESSION_KEY_COUNT-1, disc positions at
 * DISC_SLOT(0..15) right after them, and system actions at
 * SYSACT_SLOT(0..INTVSESSION_SYSACT_COUNT-1) after that, LEFT side only
 * (slot_for_target/target_for_slot below convert). Appended, not inserted:
 * the persisted "bindings" string names a slot by number, so an older
 * build's parser (whose range check tops out at the old NUM_SLOTS) simply
 * ignores whatever trailing slots it doesn't know about instead of
 * misreading them as some other keypad key.
 *
 * Two tables, not one: s_defaults is computed once per bindings_init call and
 * never mutated again, s_table is the live, user-editable copy. Diffing the
 * two is what makes persistence (only non-default entries are written) and
 * reset (copy defaults back over the live table) both trivial, and is why
 * resolve_keysym_locked has to fall back to the pure intvsession_key_from_
 * keysym at all -- see that function's own comment on the disc's secondary
 * defaults.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "bindings.h"

#define NUM_SIDES 4
#define PACKED_BUF_SIZE 8192

/* See this file's own header: slots 0..INTVSESSION_KEY_COUNT-1 are the
 * keypad/action buttons (index == intvsession_key), slots DISC_SLOT(0)
 * .. DISC_SLOT(15) are the disc's 16 clock positions, and slots
 * SYSACT_SLOT(0) .. SYSACT_SLOT(INTVSESSION_SYSACT_COUNT-1) -- LEFT side
 * only, see intvsession_target_sysaction's own comment -- are the
 * machine-global system actions (Reset Game, Reset to CONFIG). */
#define DISC_SLOT(dir) (INTVSESSION_KEY_COUNT + (dir))
#define SYSACT_SLOT(a) (INTVSESSION_KEY_COUNT + INTVSESSION_DISC_POSITIONS + (a))
#define NUM_SLOTS (INTVSESSION_KEY_COUNT + INTVSESSION_DISC_POSITIONS + \
                  INTVSESSION_SYSACT_COUNT)

static intvsession_binding s_defaults[NUM_SIDES][NUM_SLOTS];
static intvsession_binding s_table[NUM_SIDES][NUM_SLOTS];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* All 16 clock positions' names, 0 = East going counter-clockwise -- the
 * same numbering intv_host.c's disc_codes and intvsession_disc_from_point
 * use. Unlike the 8-name table this replaced (indexed by direction/2, which
 * mislabelled every odd half-step -- direction 1 printed as plain "East"),
 * every position gets its own name. */
static const char *const disc_dir_names[INTVSESSION_DISC_POSITIONS] = {
    "East", "ENE", "Northeast", "NNE",
    "North", "NNW", "Northwest", "WNW",
    "West", "WSW", "Southwest", "SSW",
    "South", "SSE", "Southeast", "ESE",
};

/* intvsession_key_from_keysym is case-insensitive by construction (letters
 * are uppercased before its switch, see intv_keymap.c); the table below is
 * an exact-match scan over stored keysym bytes, so it has to be fed the same
 * canonical case or 'd' and 'D' become two different bindings that can never
 * see each other. That was harmless while only digits/modifiers were ever
 * stored (compute_defaults_locked used to skip every MAP_DISC result), but
 * every one of this project's four frontends delivers a LOWERCASE keysym for
 * an unshifted letter key (see e.g. frontends/gnome/keysym_map.h's own
 * comment on GDK's ASCII-valued keyvals), while the ASCII default-seeding
 * loop below happens to store UPPERCASE (it walks 'A'..'Z' before 'a'..'z').
 * Without normalising here, an explicit remap onto a letter key would store
 * whatever case the frontend delivered, an old default it displaced (stored
 * uppercase) would stop byte-matching any live keystroke, and the "stolen"
 * slot would never actually clear -- see intvsession_target_set_key's own
 * comment on why the clear is conditional on an exact match. */
static uint32_t norm_keysym(uint32_t k)
{
    return (k >= 'a' && k <= 'z') ? k - ('a' - 'A') : k;
}

/* ---- target <-> slot conversion ---------------------------------------- */

static int slot_for_target(intvsession_key_mapping t)
{
    if (t.kind == INTVSESSION_MAP_KEY) {
        if (t.key < 0 || t.key >= INTVSESSION_KEY_COUNT)
            return -1;
        return (int)t.key;
    }
    if (t.kind == INTVSESSION_MAP_DISC) {
        if (t.direction < 0 || t.direction >= INTVSESSION_DISC_POSITIONS)
            return -1;
        return DISC_SLOT(t.direction);
    }
    if (t.kind == INTVSESSION_MAP_SYSACT) {
        if (t.sysact < 0 || t.sysact >= INTVSESSION_SYSACT_COUNT)
            return -1;
        return SYSACT_SLOT(t.sysact);
    }
    return -1;
}

static intvsession_key_mapping target_for_slot(int side, int slot)
{
    intvsession_key_mapping m = { 0 };

    m.side = (intvsession_pad_side)side;
    if (slot < INTVSESSION_KEY_COUNT) {
        m.kind = INTVSESSION_MAP_KEY;
        m.key = (intvsession_key)slot;
    } else if (slot < INTVSESSION_KEY_COUNT + INTVSESSION_DISC_POSITIONS) {
        m.kind = INTVSESSION_MAP_DISC;
        m.direction = slot - INTVSESSION_KEY_COUNT;
    } else {
        m.kind = INTVSESSION_MAP_SYSACT;
        m.sysact = (intvsession_sysaction)
                (slot - INTVSESSION_KEY_COUNT - INTVSESSION_DISC_POSITIONS);
    }
    return m;
}

intvsession_key_mapping intvsession_target_key(intvsession_pad_side side,
                                                intvsession_key key)
{
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_KEY, .side = side,
                                  .key = key };
    return m;
}

intvsession_key_mapping intvsession_target_disc(intvsession_pad_side side,
                                                 int direction)
{
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_DISC, .side = side,
                                  .direction = direction };
    return m;
}

/* .side is always LEFT -- a sysaction is machine-global, not per-side, but
 * still needs some side to key its slot in s_table/s_defaults (see
 * SYSACT_SLOT below). Left was picked arbitrarily; every lookup that can
 * reach a sysaction target (bindings_target_from_button's fallback,
 * intvsession_target_set_button's steal scan) knows to treat it specially,
 * so nothing outside this file needs to know slots exist there at all. */
intvsession_key_mapping intvsession_target_sysaction(intvsession_sysaction a)
{
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_SYSACT,
                                  .side = INTVSESSION_PAD_LEFT, .sysact = a };
    return m;
}

/* ---- defaults --------------------------------------------------------- */

/* Every INTVSESSION_KEYSYM_* the pad map (as opposed to the ECS keyboard
 * map) has any use for -- see intv_keymap.c's own switch. Explicit rather
 * than assuming the enum's values are contiguous from
 * INTVSESSION_KEYSYM_UP: safer against that enum being reordered or
 * extended later, and it costs nothing since this list is only walked once
 * per bindings_init.
 *
 * Walked BEFORE the printable-ASCII sweep below (see seed_default_locked's
 * first-wins rule): the arrow keys need first claim on the left disc's four
 * cardinal defaults, ahead of the letters ('K'/'I'/'J'/'M') that mapping.c
 * binds to the very same four positions as a second, keyboard-only way to
 * reach them (see intv_keymap.c's own comment on that). Losing the
 * first-wins race doesn't unbind a letter -- resolve_keysym_locked's own
 * fallback keeps it working as a secondary default for as long as the
 * segment it lands on still carries its own default. */
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

/* Stores keysym's default target, iff it has one and that target's slot
 * isn't already claimed -- first wins, see special_keysyms's own comment on
 * why special_keysyms is probed first. Every keypad/action button target has
 * exactly one default keysym in intv_keymap.c (no collisions there, so
 * first-wins changes nothing for those 15 slots); only the disc's four
 * cardinal positions on the left side have two -- an arrow key and a
 * letter -- which is the case this ordering exists for. */
static void seed_default_locked(uint32_t keysym)
{
    intvsession_key_mapping m = intvsession_key_from_keysym(keysym);
    int slot = slot_for_target(m);

    if (slot >= 0 && s_defaults[m.side][slot].keysym == 0)
        s_defaults[m.side][slot].keysym = norm_keysym(keysym);
}

/* Walks every keysym intvsession_key_from_keysym could possibly resolve --
 * the special range first, then printable ASCII -- and stores each result's
 * keysym into its (side,slot) default, first claim wins (seed_default_locked).
 * There is deliberately no second, hand-transcribed copy of intv_keymap.c's
 * table here: this derives the defaults from the one pure function that
 * already carries them, so the two cannot drift apart. */
static void compute_defaults_locked(void)
{
    uint32_t c;
    size_t i;
    int side, slot;

    memset(s_defaults, 0, sizeof(s_defaults));
    for (side = 0; side < NUM_SIDES; side++)
        for (slot = 0; slot < NUM_SLOTS; slot++)
            s_defaults[side][slot].button = INTVSESSION_PAD_BTN_NONE;

    for (i = 0; i < sizeof(special_keysyms) / sizeof(special_keysyms[0]); i++)
        seed_default_locked(special_keysyms[i]);
    for (c = 0x20; c <= 0x7E; c++)
        seed_default_locked(c);

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

    /* System action defaults: Backspace/Escape, LEFT side's sysact slots
     * only (see intvsession_target_sysaction's own comment on why a
     * sysaction lives on one arbitrary side). Both keysyms are already
     * walked by the special_keysyms sweep above, but intvsession_key_from_
     * keysym has no case for either (falls to its default: none()), so
     * neither claimed a slot there -- these are genuinely free. No gamepad
     * default for either; a player opts in via Map mode. */
    s_defaults[INTVSESSION_PAD_LEFT][SYSACT_SLOT(INTVSESSION_SYSACT_RESET_GAME)]
        .keysym = INTVSESSION_KEYSYM_BACKSPACE;
    s_defaults[INTVSESSION_PAD_LEFT][SYSACT_SLOT(INTVSESSION_SYSACT_RESET_CONFIG)]
        .keysym = INTVSESSION_KEYSYM_ESCAPE;
}

static void reset_to_defaults_locked(void)
{
    memcpy(s_table, s_defaults, sizeof(s_table));
}

/* ---- persistence -------------------------------------------------------
 * One packed settings key ("bindings"), only the entries that differ from
 * s_defaults: "<side>.<slot>.k:<keysym>" for a keyboard override,
 * "<side>.<slot>.p:<button>" for a gamepad one, ';'-separated. An entry whose
 * value is 0 (keysym) or -1 (button, INTVSESSION_PAD_BTN_NONE) means
 * "explicitly unbound", distinct from "not listed" (still default) -- that
 * distinction matters when a slot's default input was stolen by some other
 * target and the slot itself was never given a replacement. */

static void pack_locked(char *buf, size_t bufsz)
{
    int side, slot;
    size_t used = 0;

    if (bufsz > 0)
        buf[0] = '\0';
    for (side = 0; side < NUM_SIDES; side++) {
        for (slot = 0; slot < NUM_SLOTS; slot++) {
            const intvsession_binding *cur = &s_table[side][slot];
            const intvsession_binding *def = &s_defaults[side][slot];
            /* Clamped separately from `used`: forming buf+used once used
             * has run past bufsz -- which NUM_SLOTS growing over time makes
             * less and less theoretical -- would be a pointer more than one
             * past the end, UB even though snprintf's size-0 call never
             * dereferences it. */
            size_t off = used < bufsz ? used : bufsz;
            int n;

            if (cur->keysym != def->keysym) {
                n = snprintf(buf + off, bufsz - off, "%s%d.%d.k:%u",
                            used ? ";" : "", side, slot,
                            (unsigned)cur->keysym);
                if (n > 0)
                    used += (size_t)n;
            }
            off = used < bufsz ? used : bufsz;
            if (cur->button != def->button) {
                n = snprintf(buf + off, bufsz - off, "%s%d.%d.p:%d",
                            used ? ";" : "", side, slot, (int)cur->button);
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
        int side, slot;
        char kind;
        long val;

        if (sscanf(tok, "%d.%d.%c:%ld", &side, &slot, &kind, &val) != 4)
            continue;
        if (side < 0 || side >= NUM_SIDES || slot < 0 || slot >= NUM_SLOTS)
            continue;
        if (kind == 'k')
            s_table[side][slot].keysym = norm_keysym((uint32_t)val);
        else if (kind == 'p')
            s_table[side][slot].button = (intvsession_pad_button)val;
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
    intvsession_key_mapping m = { .kind = INTVSESSION_MAP_NONE };
    return m;
}

/* keysym's current target: a table scan (finds a MAP_KEY/MAP_DISC default
 * wherever it has been moved to, or a non-default override), falling back to
 * the pure default table only to recognise a still-unclaimed disc secondary
 * default -- see the file header on why. Caller holds s_mtx. */
static intvsession_key_mapping resolve_keysym_locked(uint32_t keysym)
{
    int side, slot;
    intvsession_key_mapping def;

    /* norm_keysym: the table stores canonical (uppercase) letters, so the
     * scan below needs the same case a live keystroke won't otherwise
     * match -- see that function's own comment. */
    keysym = norm_keysym(keysym);
    if (keysym != 0) {
        for (side = 0; side < NUM_SIDES; side++) {
            for (slot = 0; slot < NUM_SLOTS; slot++) {
                if (s_table[side][slot].keysym == keysym)
                    return target_for_slot(side, slot);
            }
        }
    }

    /* Not in the table under this keysym. The only way it can still resolve
     * to anything is as one of the disc's secondary defaults (see
     * intv_keymap.c: the left disc's four cardinals each have a second,
     * letter keysym alongside the arrow key that won compute_defaults_locked's
     * first-wins race -- 'K' for East, 'I' for North, and so on).
     *
     * A secondary default survives only while its segment's own table slot
     * still holds ITS default keysym (untouched) or nothing at all (emptied
     * by some other target stealing the winning keysym away, which is not a
     * deliberate remap of this segment). The moment a user puts some other
     * keysym on the segment, the secondary default goes dark along with the
     * one it rode in on -- there is no world where two different keys both
     * claim to drive the same disc position at once. */
    def = intvsession_key_from_keysym(keysym);
    if (def.kind == INTVSESSION_MAP_DISC) {
        int slot2 = DISC_SLOT(def.direction);
        uint32_t held = s_table[def.side][slot2].keysym;
        if (held == 0 || held == s_defaults[def.side][slot2].keysym)
            return def;
    }
    return none_mapping();
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

intvsession_binding intvsession_target_binding_get(intvsession *s,
        intvsession_key_mapping target)
{
    intvsession_binding b = { 0, INTVSESSION_PAD_BTN_NONE };
    int slot;
    (void)s;
    if (target.side < 0 || target.side >= NUM_SIDES)
        return b;
    slot = slot_for_target(target);
    if (slot < 0)
        return b;
    pthread_mutex_lock(&s_mtx);
    b = s_table[target.side][slot];
    pthread_mutex_unlock(&s_mtx);
    return b;
}

intvsession_binding intvsession_binding_get(intvsession *s,
        intvsession_pad_side side, intvsession_key key)
{
    return intvsession_target_binding_get(s, intvsession_target_key(side, key));
}

/* Gamepad-button lookup, scoped to one side only -- see bindings.h. Now
 * returns MAP_DISC too, since a button can drive a disc segment as well as a
 * keypad key. Falls back to LEFT's system-action slots when `side` isn't
 * LEFT and the per-side scan misses: a sysaction target is always stored on
 * LEFT (see intvsession_target_sysaction's own comment) regardless of which
 * side's physical pad the player used to bind it, so a pad bound to RIGHT
 * (or ECS_LEFT/ECS_RIGHT) can still fire one. */
intvsession_key_mapping bindings_target_from_button(intvsession_pad_side side,
                                                     intvsession_pad_button button)
{
    int slot;

    if (side < 0 || side >= NUM_SIDES || button == INTVSESSION_PAD_BTN_NONE)
        return none_mapping();
    pthread_mutex_lock(&s_mtx);
    for (slot = 0; slot < NUM_SLOTS; slot++) {
        if (s_table[side][slot].button == button) {
            intvsession_key_mapping m = target_for_slot(side, slot);
            pthread_mutex_unlock(&s_mtx);
            return m;
        }
    }
    if (side != INTVSESSION_PAD_LEFT) {
        int a;
        for (a = 0; a < INTVSESSION_SYSACT_COUNT; a++) {
            slot = SYSACT_SLOT(a);
            if (s_table[INTVSESSION_PAD_LEFT][slot].button == button) {
                intvsession_key_mapping m =
                    target_for_slot(INTVSESSION_PAD_LEFT, slot);
                pthread_mutex_unlock(&s_mtx);
                return m;
            }
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return none_mapping();
}

int bindings_button_is_free(intvsession_pad_side side,
                            intvsession_pad_button button)
{
    return bindings_target_from_button(side, button).kind == INTVSESSION_MAP_NONE;
}

int bindings_disc_buttons(intvsession_pad_side side,
                          intvsession_pad_button out[INTVSESSION_DISC_POSITIONS])
{
    int dir, n = 0;

    if (side < 0 || side >= NUM_SIDES)
        return 0;
    pthread_mutex_lock(&s_mtx);
    for (dir = 0; dir < INTVSESSION_DISC_POSITIONS; dir++) {
        intvsession_pad_button b = s_table[side][DISC_SLOT(dir)].button;
        out[dir] = b;
        if (b != INTVSESSION_PAD_BTN_NONE)
            n++;
    }
    pthread_mutex_unlock(&s_mtx);
    return n;
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

const char *intvsession_disc_dir_name(int direction)
{
    if (direction < 0 || direction >= INTVSESSION_DISC_POSITIONS)
        return "?";
    return disc_dir_names[direction];
}

const char *intvsession_sysaction_name(intvsession_sysaction a)
{
    switch (a) {
    case INTVSESSION_SYSACT_RESET_GAME:   return "Reset Game";
    case INTVSESSION_SYSACT_RESET_CONFIG: return "Reset to CONFIG";
    default:                             return "?";
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

int intvsession_target_name(intvsession_key_mapping target, char *dst,
                            int dstsz)
{
    char buf[128];

    if (target.kind == INTVSESSION_MAP_KEY)
        snprintf(buf, sizeof(buf), "%s %s",
                intvsession_pad_side_name(target.side),
                intvsession_pad_key_name(target.key));
    else if (target.kind == INTVSESSION_MAP_DISC)
        snprintf(buf, sizeof(buf), "%s disc %s",
                intvsession_pad_side_name(target.side),
                intvsession_disc_dir_name(target.direction));
    else if (target.kind == INTVSESSION_MAP_SYSACT)
        /* No side prefix -- a sysaction isn't per-side (see
         * intvsession_target_sysaction's own comment), so ".side" here is
         * an implementation detail this name deliberately doesn't leak. */
        snprintf(buf, sizeof(buf), "%s",
                intvsession_sysaction_name(target.sysact));
    else
        buf[0] = '\0';

    if (dst && dstsz > 0)
        snprintf(dst, (size_t)dstsz, "%s", buf);
    return (int)strlen(buf);
}

static void describe_mapping_locked(intvsession_key_mapping m, char *dst,
                                    int dstsz)
{
    if (!dst || dstsz <= 0)
        return;
    intvsession_target_name(m, dst, dstsz);
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
 * there if it was actually holding that input, write the new binding, then
 * persist outside the lock (persist_locked_then unlocks s_mtx itself -- see
 * its own comment). Re-binding an input to the target it already occupies is
 * treated as "nothing stolen" rather than reporting a target stealing from
 * itself. */

void intvsession_target_set_key(intvsession *s, intvsession_key_mapping target,
        uint32_t keysym, char *stolen, int stolensz)
{
    intvsession_key_mapping cur;
    int slot, cur_slot;

    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (target.side < 0 || target.side >= NUM_SIDES)
        return;
    slot = slot_for_target(target);
    if (slot < 0)
        return;
    /* norm_keysym: stored, scanned and compared canonically -- see that
     * function's own comment. */
    keysym = norm_keysym(keysym);

    pthread_mutex_lock(&s_mtx);
    cur = resolve_keysym_locked(keysym);
    if (cur.kind != INTVSESSION_MAP_NONE && cur.side == target.side &&
        slot_for_target(cur) == slot)
        cur = none_mapping();
    describe_mapping_locked(cur, stolen, stolensz);
    /* Only clear the old slot if it is actually the one holding this keysym
     * -- resolve_keysym_locked can also report a secondary default (e.g.
     * 'K' resolving to Left disc East while the table slot itself still
     * holds the Right Arrow that won the default race), and that slot must
     * not be wiped out from under the input that legitimately owns it. */
    cur_slot = slot_for_target(cur);
    if (cur_slot >= 0 && s_table[cur.side][cur_slot].keysym == keysym)
        s_table[cur.side][cur_slot].keysym = 0;
    s_table[target.side][slot].keysym = keysym;
    persist_locked_then(s); /* unlocks s_mtx */
}

void intvsession_target_set_button(intvsession *s, intvsession_key_mapping target,
        intvsession_pad_button button, char *stolen, int stolensz)
{
    int slot, cur_slot = -1, cur_side = target.side;
    int k;

    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (target.side < 0 || target.side >= NUM_SIDES)
        return;
    slot = slot_for_target(target);
    if (slot < 0)
        return;

    pthread_mutex_lock(&s_mtx);
    if (button != INTVSESSION_PAD_BTN_NONE) {
        if (target.kind == INTVSESSION_MAP_SYSACT) {
            /* A sysaction target is machine-global (always stored on LEFT,
             * see intvsession_target_sysaction's own comment), but the
             * button being bound to it can currently belong to ANY side --
             * a physical gamepad's face buttons get the same per-side
             * defaults on all four (compute_defaults_locked). Scanning and
             * clearing only target.side (always LEFT) would miss a claim on
             * RIGHT/ECS_LEFT/ECS_RIGHT, leaving that side able to fire the
             * same button's old action right alongside the new sysaction --
             * so every side holding this button is cleared, not just the
             * first, keeping this consistent with bindings_target_from_
             * button's own cross-side fallback. `stolen` still names only
             * the first one found; a button hitting more than one side at
             * once shouldn't happen given every other setter's own
             * per-side-unique invariant, but clearing all of them is
             * correct even if it somehow did. */
            int side;
            for (side = 0; side < NUM_SIDES; side++) {
                for (k = 0; k < NUM_SLOTS; k++) {
                    if (s_table[side][k].button == button) {
                        if (cur_slot < 0) {
                            cur_side = side;
                            cur_slot = k;
                        }
                        s_table[side][k].button = INTVSESSION_PAD_BTN_NONE;
                    }
                }
            }
        } else {
            for (k = 0; k < NUM_SLOTS; k++) {
                if (s_table[target.side][k].button == button) {
                    cur_slot = k;
                    s_table[target.side][k].button = INTVSESSION_PAD_BTN_NONE;
                    break;
                }
            }
        }
    }
    if (cur_slot == slot && cur_side == (int)target.side)
        cur_slot = -1; /* re-bound to the slot it already occupies -- not a
                        * steal, see intvsession_target_set_key's own
                        * comment on the same rule. The slot itself was
                        * already cleared above; the unconditional write
                        * below puts `button` straight back. */
    if (cur_slot >= 0 && stolen && stolensz > 0)
        intvsession_target_name(target_for_slot(cur_side, cur_slot),
                                stolen, stolensz);
    s_table[target.side][slot].button = button;
    persist_locked_then(s); /* unlocks s_mtx */
}

void intvsession_binding_set_key(intvsession *s, intvsession_pad_side side,
        intvsession_key key, uint32_t keysym, char *stolen, int stolensz)
{
    intvsession_target_set_key(s, intvsession_target_key(side, key), keysym,
                               stolen, stolensz);
}

void intvsession_binding_set_button(intvsession *s, intvsession_pad_side side,
        intvsession_key key, intvsession_pad_button button,
        char *stolen, int stolensz)
{
    intvsession_target_set_button(s, intvsession_target_key(side, key), button,
                                  stolen, stolensz);
}

void intvsession_bindings_reset(intvsession *s)
{
    pthread_mutex_lock(&s_mtx);
    reset_to_defaults_locked();
    pthread_mutex_unlock(&s_mtx);
    intvsession_set_str(s, "bindings", "");
}
