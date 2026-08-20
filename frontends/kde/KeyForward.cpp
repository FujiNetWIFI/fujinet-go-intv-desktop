/*
 * See KeyForward.h for why this is shared rather than duplicated per window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

#include <QKeyEvent>

namespace {

/* Qt cannot answer either of the two questions this translation actually
 * needs, so both are answered from the native scancode instead:
 *
 *  1. "Which physical key is this, ignoring Shift?" QKeyEvent::key() and
 *     text() are both already shifted -- Shift+"1" arrives as Key_Exclam /
 *     "!". That is fatal rather than cosmetic here, because Shift is itself
 *     a controller binding (jzIntv's mapping.c binds LSHIFT/RSHIFT to the
 *     pads' top action buttons) while the number row IS the right
 *     controller's keypad. Read literally, holding an action button
 *     silently killed all twelve of that controller's keypad keys.
 *
 *  2. "Left or right modifier?" Qt reports both Shift keys as Qt::Key_Shift
 *     and both Ctrl/Alt keys as Key_Control/Key_Alt. mapping.c crosses the
 *     sides deliberately -- RSHIFT/RALT/RCTRL drive the LEFT controller's
 *     action buttons and LSHIFT/LALT/LCTRL the right -- so collapsing them
 *     left PD0L_A_T, PD0L_A_L and PD0L_A_R unreachable from a Qt keyboard
 *     entirely.
 *
 * Scancodes also settle NumLock: jzIntv reads the keypad through SDL, which
 * reports SDLK_KP_* whether or not NumLock is on, and the scancode is
 * likewise unaffected -- so the left controller's keypad no longer depends
 * on a NumLock LED either.
 *
 * The values below are Linux evdev codes (linux/input-event-codes.h). Both
 * Qt platform plugins this frontend runs under report nativeScanCode() as
 * the evdev code plus 8: xcb because that is the X11 keycode convention,
 * and QtWayland because it deliberately matches X11 there. */
enum {
    EVDEV_OFFSET = 8,

    EV_1 = 2, EV_2 = 3, EV_3 = 4, EV_4 = 5, EV_5 = 6,
    EV_6 = 7, EV_7 = 8, EV_8 = 9, EV_9 = 10, EV_0 = 11,
    EV_MINUS = 12, EV_EQUAL = 13,

    EV_SEMICOLON = 39, EV_COMMA = 51, EV_DOT = 52,

    EV_LEFTCTRL = 29, EV_RIGHTCTRL = 97,
    EV_LEFTSHIFT = 42, EV_RIGHTSHIFT = 54,
    EV_LEFTALT = 56, EV_RIGHTALT = 100,

    EV_KP7 = 71, EV_KP8 = 72, EV_KP9 = 73,
    EV_KP4 = 75, EV_KP5 = 76, EV_KP6 = 77,
    EV_KP1 = 79, EV_KP2 = 80, EV_KP3 = 81,
    EV_KP0 = 82, EV_KPDOT = 83, EV_KPENTER = 96,
};

quint32 keysymForScanCode(quint32 nativeScanCode)
{
    if (nativeScanCode < EVDEV_OFFSET)
        return 0;

    switch (nativeScanCode - EVDEV_OFFSET) {
    /* Number row -> right controller keypad (mapping.c: "1".."="). */
    case EV_1: return '1';
    case EV_2: return '2';
    case EV_3: return '3';
    case EV_4: return '4';
    case EV_5: return '5';
    case EV_6: return '6';
    case EV_7: return '7';
    case EV_8: return '8';
    case EV_9: return '9';
    case EV_0: return '0';
    case EV_MINUS: return '-';
    case EV_EQUAL: return '=';

    /* "," is the left disc's SE (mapping.c: ","); "." and ";" are
     * ECS-keyboard-only. All three would otherwise become "<", ">" and ":"
     * with Shift held. */
    case EV_COMMA: return ',';
    case EV_DOT: return '.';
    case EV_SEMICOLON: return ';';

    /* Numpad -> left controller keypad (mapping.c: KP_7..KP_ENTER). */
    case EV_KP7: return INTVSESSION_KEYSYM_KP_7;
    case EV_KP8: return INTVSESSION_KEYSYM_KP_8;
    case EV_KP9: return INTVSESSION_KEYSYM_KP_9;
    case EV_KP4: return INTVSESSION_KEYSYM_KP_4;
    case EV_KP5: return INTVSESSION_KEYSYM_KP_5;
    case EV_KP6: return INTVSESSION_KEYSYM_KP_6;
    case EV_KP1: return INTVSESSION_KEYSYM_KP_1;
    case EV_KP2: return INTVSESSION_KEYSYM_KP_2;
    case EV_KP3: return INTVSESSION_KEYSYM_KP_3;
    case EV_KP0: return INTVSESSION_KEYSYM_KP_0;
    case EV_KPDOT: return INTVSESSION_KEYSYM_KP_PERIOD;
    case EV_KPENTER: return INTVSESSION_KEYSYM_KP_ENTER;

    /* Action buttons -- upstream really does cross the sides, see the
     * comment above and intv_keymap.c's own. */
    case EV_RIGHTSHIFT: return INTVSESSION_KEYSYM_RSHIFT;
    case EV_RIGHTALT:   return INTVSESSION_KEYSYM_RALT;
    case EV_RIGHTCTRL:  return INTVSESSION_KEYSYM_RCTRL;
    case EV_LEFTSHIFT:  return INTVSESSION_KEYSYM_LSHIFT;
    case EV_LEFTALT:    return INTVSESSION_KEYSYM_LALT;
    case EV_LEFTCTRL:   return INTVSESSION_KEYSYM_LCTRL;

    default:
        return 0;
    }
}

/* The evdev+8 convention above is a de-facto Qt platform-plugin contract,
 * not something Qt's API guarantees, and getting it wrong would be worse
 * than useless: an unshifted offset would slide the number row down by six
 * and report "1".."4" for entirely unrelated keys rather than simply
 * failing to match. So it is checked rather than assumed.
 *
 * Modifiers are the natural probe -- they are the one group of keys that
 * appears both in the scancode table and in Qt's own shift-independent
 * key() -- so the first Shift/Control/Alt event decides. If the two
 * disagree, the scancode path switches off for the rest of the run and
 * everything falls back to key()/text(), which is exactly the behaviour
 * this file replaced: no worse than before, rather than actively wrong. */
bool g_scanCodesTrusted = true;
bool g_scanCodesChecked = false;

void checkScanCodes(const QKeyEvent *event)
{
    quint32 left, right;

    switch (event->key()) {
    case Qt::Key_Shift:
        left = INTVSESSION_KEYSYM_LSHIFT; right = INTVSESSION_KEYSYM_RSHIFT;
        break;
    case Qt::Key_Control:
        left = INTVSESSION_KEYSYM_LCTRL; right = INTVSESSION_KEYSYM_RCTRL;
        break;
    case Qt::Key_Alt:
        left = INTVSESSION_KEYSYM_LALT; right = INTVSESSION_KEYSYM_RALT;
        break;
    default:
        return; /* not a probe key -- decide on a later event */
    }

    /* Qt collapses the two sides into one key(), so either half of the pair
     * confirms the offset; which one it actually is, is the whole reason
     * the scancode path exists. */
    const quint32 got = keysymForScanCode(event->nativeScanCode());
    g_scanCodesTrusted = (got == left || got == right);
    g_scanCodesChecked = true;
}

} // namespace

quint32 intvKeysymForKeyEvent(const QKeyEvent *event)
{
    if (!g_scanCodesChecked)
        checkScanCodes(event);

    /* Physical keys first -- see keysymForScanCode. */
    if (g_scanCodesTrusted) {
        quint32 keysym = keysymForScanCode(event->nativeScanCode());
        if (keysym != 0)
            return keysym;
    }

    switch (event->key()) {
    case Qt::Key_Up:    return INTVSESSION_KEYSYM_UP;
    case Qt::Key_Down:  return INTVSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:  return INTVSESSION_KEYSYM_LEFT;
    case Qt::Key_Right: return INTVSESSION_KEYSYM_RIGHT;
    /* Non-printable keys the base controller map has no use for, but the
     * ECS keyboard map does. */
    case Qt::Key_Escape:    return INTVSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Return:    return INTVSESSION_KEYSYM_RETURN;
    case Qt::Key_Enter:     return INTVSESSION_KEYSYM_KP_ENTER;
    case Qt::Key_Backspace: return INTVSESSION_KEYSYM_BACKSPACE;
    /* Only reached when the scancode path is off (see checkScanCodes):
     * Qt cannot tell the sides apart, so this is the degraded behaviour
     * this frontend had before -- the left controller's action buttons
     * stay unreachable, but nothing is mis-mapped. */
    case Qt::Key_Shift:   return INTVSESSION_KEYSYM_LSHIFT;
    case Qt::Key_Control: return INTVSESSION_KEYSYM_LCTRL;
    case Qt::Key_Alt:     return INTVSESSION_KEYSYM_LALT;
    default:
        break;
    }

    /* Everything the scancode table deliberately leaves alone -- the
     * IJKM/DRWEASZXC disc letters above all -- resolves by character, which
     * keeps them layout-aware the way jzIntv's own SDL keycodes are.
     * Letters are unaffected by Shift here (intvsession_key_from_keysym
     * folds case itself), so only Ctrl needs the fallback: Qt reports no
     * text at all for a Ctrl combo. */
    if (event->modifiers().testFlag(Qt::ControlModifier) &&
        event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
        return 'a' + (quint32)(event->key() - Qt::Key_A);

    if (!event->text().isEmpty())
        return event->text().at(0).unicode();

    return 0;
}

void intvForwardKey(intvsession *session, const QKeyEvent *event, int down)
{
    const quint32 keysym = intvKeysymForKeyEvent(event);
    if (keysym == 0)
        return;

    /* "ECS Keyboard" input mode (Settings, or toggled live from there)
     * steals the host keyboard for the ECS's own keyboard instead of the
     * hand controllers -- see intvsession_ecs_key_from_keysym's own comment
     * on why the two can't both claim it at once. */
    if (intvsession_get_int(session, "keyboard_mode", 0)) {
        intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
        if (key != INTVSESSION_ECS_KEY_NONE)
            intvsession_ecs_key_set(session, key, down);
        return;
    }

    /* _bound, not the pure table: a keypad window "Map" remap has to reach
     * every keyboard-driven window, not just the one the remap happened
     * in -- see intvsession.h's own comment on why. */
    intvsession_key_mapping m = intvsession_key_from_keysym_bound(session, keysym);
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(session, m.side, down ? m.direction : -1);
}
