/*
 * KeyForward -- the one Qt key-event -> intvsession keysym translation, and
 * the one place that decides whether a keystroke drives the hand
 * controllers or the ECS keyboard.
 *
 * DisplayWidget, KeypadWindow and EcsKeyboardWindow each used to carry
 * their own local forwardKey; EcsKeyboardWindow.cpp's header argued that
 * was fine because "each Qt window's forwardKey is small enough that
 * duplicating the switch has been the pattern here." That stopped being
 * true once the translation had to work from native scancodes (see
 * KeyForward.cpp for why it must), and the copies had already drifted
 * apart in ways users could feel: KeypadWindow's handled arrows and
 * nothing else -- no numpad, no modifiers -- and neither it nor the GNOME
 * port's equivalent honoured "keyboard_mode", so the same keystroke meant
 * different things depending on which window happened to hold focus.
 * Hence one implementation, shared.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QtGlobal>

#include "intvsession.h"

class QKeyEvent;

/* Translates a Qt key event to intvsession.h's private INTVSESSION_KEYSYM_*
 * numbering (or a plain ASCII code, which that map also accepts). Returns 0
 * for keys the emulated machine has no use for. */
quint32 intvKeysymForKeyEvent(const QKeyEvent *event);

/* Translates and dispatches: routes to the ECS keyboard matrix when the
 * session's "keyboard_mode" setting is on, otherwise to the hand
 * controllers. `down` is 1 for a press, 0 for a release -- both matter,
 * since jzIntv's pad_t tracks a held key by level and a missed release
 * leaves it stuck down in the machine. */
void intvForwardKey(intvsession *session, const QKeyEvent *event, int down);
