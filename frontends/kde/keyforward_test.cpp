/*
 * keyforward_test -- exercises KeyForward.cpp's Qt key-event translation
 * against synthetic QKeyEvents.
 *
 * QKeyEvent has a public constructor taking nativeScanCode, and building an
 * event needs no QGuiApplication and no display server -- so the scancode
 * path this frontend now depends on is testable directly, rather than only
 * by a human pressing keys on each of X11 and Wayland. The cases below are
 * exactly the ones the old key()/text()-based translation got wrong:
 * action-button + keypad chords, and left-versus-right modifiers.
 *
 * Scancodes are evdev + 8, the convention both Qt platform plugins use --
 * see KeyForward.cpp. They match `xmodmap -pke` on any Linux box.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QKeyEvent>

#include <cstdio>

#include "KeyForward.h"

namespace {

int failed = 0;

QKeyEvent makeEvent(int key, Qt::KeyboardModifiers mods, quint32 scanCode,
                    const QString &text)
{
    return QKeyEvent(QEvent::KeyPress, key, mods, scanCode, 0, 0, text);
}

void expectKey(const char *what, const QKeyEvent &event,
               intvsession_pad_side side, intvsession_key key)
{
    const intvsession_key_mapping m =
        intvsession_key_from_keysym(intvKeysymForKeyEvent(&event));
    if (m.kind != INTVSESSION_MAP_KEY || m.side != side || m.key != key) {
        std::fprintf(stderr, "keyforward_test: FAILED: %s\n", what);
        failed = 1;
    }
}

void expectDisc(const char *what, const QKeyEvent &event,
                intvsession_pad_side side, int direction)
{
    const intvsession_key_mapping m =
        intvsession_key_from_keysym(intvKeysymForKeyEvent(&event));
    if (m.kind != INTVSESSION_MAP_DISC || m.side != side ||
        m.direction != direction) {
        std::fprintf(stderr, "keyforward_test: FAILED: %s\n", what);
        failed = 1;
    }
}

} // namespace

int main()
{
    /* ---- the bug this all exists for -------------------------------------
     * Shift is the pads' top action button, so Qt reports Shift+"1" as
     * Key_Exclam with text "!". Read literally that maps to nothing, and
     * the right controller's whole keypad dies while a button is held. */
    expectKey("Shift+1 -> right keypad 1",
              makeEvent(Qt::Key_Exclam, Qt::ShiftModifier, 10, "!"),
              INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_1);
    expectKey("Shift+0 -> right keypad 0",
              makeEvent(Qt::Key_ParenRight, Qt::ShiftModifier, 19, ")"),
              INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_0);
    expectKey("Shift+- -> right keypad Clear",
              makeEvent(Qt::Key_Underscore, Qt::ShiftModifier, 20, "_"),
              INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_CLEAR);
    expectKey("Shift+= -> right keypad Enter",
              makeEvent(Qt::Key_Plus, Qt::ShiftModifier, 21, "+"),
              INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_ENTER);
    /* "," is the left disc's SE, and Shift turns it into "<". */
    expectDisc("Shift+, -> left disc SE",
               makeEvent(Qt::Key_Less, Qt::ShiftModifier, 59, "<"),
               INTVSESSION_PAD_LEFT, 14);

    /* Unshifted, the same keys must still work. */
    expectKey("1 -> right keypad 1",
              makeEvent(Qt::Key_1, Qt::NoModifier, 10, "1"),
              INTVSESSION_PAD_RIGHT, INTVSESSION_KEY_1);

    /* ---- left vs right modifiers -----------------------------------------
     * Qt reports both as the same Qt::Key, but upstream crosses the sides:
     * the RIGHT modifiers drive the LEFT controller. Without the scancode
     * these three were unreachable entirely. */
    expectKey("RShift -> left pad top action",
              makeEvent(Qt::Key_Shift, Qt::ShiftModifier, 62, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_TOP);
    expectKey("RAlt -> left pad lower-left action",
              makeEvent(Qt::Key_Alt, Qt::AltModifier, 108, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_LEFT);
    expectKey("RCtrl -> left pad lower-right action",
              makeEvent(Qt::Key_Control, Qt::ControlModifier, 105, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_ACTION_LOWER_RIGHT);
    expectKey("LShift -> right pad top action",
              makeEvent(Qt::Key_Shift, Qt::ShiftModifier, 50, ""),
              INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_TOP);
    expectKey("LAlt -> right pad lower-left action",
              makeEvent(Qt::Key_Alt, Qt::AltModifier, 64, ""),
              INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_LEFT);
    expectKey("LCtrl -> right pad lower-right action",
              makeEvent(Qt::Key_Control, Qt::ControlModifier, 37, ""),
              INTVSESSION_PAD_RIGHT, INTVSESSION_ACTION_LOWER_RIGHT);

    /* ---- the numpad, NumLock on and off -----------------------------------
     * Qt reports the NumLock-off keys with the navigation Qt::Keys; the
     * scancode is the same either way, which is the point. */
    expectKey("KP7 (NumLock on) -> left keypad 1",
              makeEvent(Qt::Key_7, Qt::KeypadModifier, 79, "7"),
              INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1);
    expectKey("KP7 (NumLock off) -> left keypad 1",
              makeEvent(Qt::Key_Home, Qt::KeypadModifier, 79, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1);
    expectKey("KP0 (NumLock off) -> left keypad Clear",
              makeEvent(Qt::Key_Insert, Qt::KeypadModifier, 90, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_KEY_CLEAR);
    expectKey("KP. (NumLock off) -> left keypad 0",
              makeEvent(Qt::Key_Delete, Qt::KeypadModifier, 91, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_KEY_0);
    expectKey("KPEnter -> left keypad Enter",
              makeEvent(Qt::Key_Enter, Qt::KeypadModifier, 104, ""),
              INTVSESSION_PAD_LEFT, INTVSESSION_KEY_ENTER);

    /* ---- keys that deliberately stay on the character path ---------------- */
    expectDisc("k -> left disc E", makeEvent(Qt::Key_K, Qt::NoModifier, 45, "k"),
               INTVSESSION_PAD_LEFT, 0);
    expectDisc("Shift+D -> right disc E (case folded)",
               makeEvent(Qt::Key_D, Qt::ShiftModifier, 40, "D"),
               INTVSESSION_PAD_RIGHT, 0);
    /* Qt gives no text for a Ctrl combo, hence KeyForward's letter fallback. */
    expectDisc("Ctrl+X -> right disc S",
               makeEvent(Qt::Key_X, Qt::ControlModifier, 53, ""),
               INTVSESSION_PAD_RIGHT, 12);

    /* Arrows carry no useful scancode entry and resolve by Qt::Key. */
    expectDisc("Up -> left disc N", makeEvent(Qt::Key_Up, Qt::NoModifier, 111, ""),
               INTVSESSION_PAD_LEFT, 4);

    if (failed) {
        std::fprintf(stderr, "keyforward_test: FAILED\n");
        return 1;
    }
    std::printf("keyforward_test: OK\n");
    return 0;
}
