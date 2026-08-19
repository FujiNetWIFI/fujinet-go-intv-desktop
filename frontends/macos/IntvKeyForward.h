/*
 * IntvKeyForward -- the one macOS keyCode -> intvsession keysym translation,
 * and the one place that decides whether a keystroke drives the hand
 * controllers or the ECS keyboard.
 *
 * Shared by DisplayView and the keypad / ECS keyboard windows. Those two
 * windows used to forward nothing at all, on the reasoning that AppKit
 * gives each control its own first-responder status on click and there is
 * no session-wide keyboard hook to fall back on. There is a simpler answer:
 * a key event no control consumes travels up the responder chain to the
 * NSWindow itself, so those windows override -keyDown:/-keyUp:/-flagsChanged:
 * and call in here.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

/* Translates a macOS virtual keyCode to intvsession.h's private
 * INTVSESSION_KEYSYM_* numbering, or to a plain ASCII code (which that map
 * also accepts). 0 for keys resolved by character instead -- the caller
 * falls back to -charactersIgnoringModifiers for those. */
uint32_t IntvKeysymForKeyCode(unsigned short keyCode);

/* Full translation for an event, keyCode first and character as fallback.
 * 0 for keys the emulated machine has no use for. */
uint32_t IntvKeysymForEvent(NSEvent *event);

/* Translate and dispatch: routes to the ECS keyboard matrix when the
 * session's "keyboard_mode" setting is on, otherwise to the hand
 * controllers. `down` is 1 for a press, 0 for a release -- both matter,
 * since jzIntv's pad_t tracks a held key by level and a missed release
 * leaves it stuck down in the machine. */
void IntvForwardKeyEvent(intvsession *session, NSEvent *event, int down);

/* Always routes to the ECS keyboard matrix, ignoring "keyboard_mode" --
 * for the ECS keyboard window, which means the same thing either way. */
void IntvForwardEcsKeyEvent(intvsession *session, NSEvent *event, int down);

/* For -flagsChanged:, which carries no down/up of its own. Returns YES if
 * this event is one of the six modifier keys the controllers bind (Shift/
 * Control/Option, left and right), writing 1 or 0 to *down according to
 * whether that key's own side is now held -- the standard AppKit idiom.
 * NO, and *down untouched, for any other modifier. */
BOOL IntvModifierKeyState(NSEvent *event, int *down);

/* An NSWindow that forwards the key events its controls don't consume,
 * for the keypad and ECS keyboard panels: clicking a button in either
 * makes it first responder, and without this a keystroke would then reach
 * nothing at all.
 *
 * Set `ecsOnly` on the ECS keyboard window so its keystrokes always drive
 * the ECS matrix, matching its on-screen buttons; leave it NO on the
 * keypad so it follows the "keyboard_mode" setting like the main window. */
@interface IntvKeyWindow : NSWindow
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) BOOL ecsOnly;
@end
