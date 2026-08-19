/*
 * See IntvKeyForward.h for why this is shared rather than duplicated.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "IntvKeyForward.h"

/* macOS virtual keyCodes for arrows/numpad/modifiers/number row --
 * layout-independent, unlike characters.
 *
 * The number row is here for a second reason on top of layout: AppKit's
 * -charactersIgnoringModifiers ignores every modifier EXCEPT Shift, so
 * Shift+"1" still arrives as "!". Shift is itself a controller binding
 * (jzIntv's mapping.c binds LSHIFT/RSHIFT to the pads' top action buttons)
 * while the number row IS the right controller's keypad, so reading the
 * character meant holding an action button silently killed all twelve of
 * that controller's keypad keys -- and turned "," (the left disc's SE)
 * into "<". Pressing a button and a keypad key at once is ordinary
 * Intellivision play, so these resolve by keyCode like the numpad already
 * did. Letters stay on -charactersIgnoringModifiers below: Shift does not
 * disturb them (intvsession_key_from_keysym folds case itself) and going
 * by character keeps them layout-aware, the way jzIntv's own SDL keycodes
 * are.
 *
 * The 5/6 and 7/8/9 codes really are out of numeric order, as are Minus
 * and Equal -- that is Apple's ANSI layout, not a transcription slip. */
enum {
    kVK_ANSI_1 = 0x12,
    kVK_ANSI_2 = 0x13,
    kVK_ANSI_3 = 0x14,
    kVK_ANSI_4 = 0x15,
    kVK_ANSI_6 = 0x16,
    kVK_ANSI_5 = 0x17,
    kVK_ANSI_Equal = 0x18,
    kVK_ANSI_9 = 0x19,
    kVK_ANSI_7 = 0x1A,
    kVK_ANSI_Minus = 0x1B,
    kVK_ANSI_8 = 0x1C,
    kVK_ANSI_0 = 0x1D,
    kVK_ANSI_Semicolon = 0x29,
    kVK_ANSI_Comma = 0x2B,
    kVK_ANSI_Period = 0x2F,
    kVK_ANSI_KeypadDecimal = 0x41,
    kVK_ANSI_KeypadMultiply = 0x43,
    kVK_ANSI_KeypadPlus = 0x45,
    kVK_ANSI_KeypadClear = 0x47,
    kVK_ANSI_KeypadDivide = 0x4B,
    kVK_ANSI_KeypadEnter = 0x4C,
    kVK_ANSI_KeypadMinus = 0x4E,
    kVK_ANSI_KeypadEquals = 0x51,
    kVK_ANSI_Keypad0 = 0x52,
    kVK_ANSI_Keypad1 = 0x53,
    kVK_ANSI_Keypad2 = 0x54,
    kVK_ANSI_Keypad3 = 0x55,
    kVK_ANSI_Keypad4 = 0x56,
    kVK_ANSI_Keypad5 = 0x57,
    kVK_ANSI_Keypad6 = 0x58,
    kVK_ANSI_Keypad7 = 0x59,
    kVK_ANSI_Keypad8 = 0x5B,
    kVK_ANSI_Keypad9 = 0x5C,
    kVK_Shift = 0x38,
    kVK_RightShift = 0x3C,
    kVK_Control = 0x3B,
    kVK_RightControl = 0x3E,
    kVK_Option = 0x3A,
    kVK_RightOption = 0x3D,
    kVK_LeftArrow = 0x7B,
    kVK_RightArrow = 0x7C,
    kVK_DownArrow = 0x7D,
    kVK_UpArrow = 0x7E,
    kVK_Return = 0x24,
    kVK_Delete = 0x33, /* "Backspace" on a Mac keyboard */
    kVK_Escape = 0x35,
};

uint32_t IntvKeysymForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case kVK_UpArrow:    return INTVSESSION_KEYSYM_UP;
    case kVK_DownArrow:  return INTVSESSION_KEYSYM_DOWN;
    case kVK_LeftArrow:  return INTVSESSION_KEYSYM_LEFT;
    case kVK_RightArrow: return INTVSESSION_KEYSYM_RIGHT;
    case kVK_ANSI_Keypad0: return INTVSESSION_KEYSYM_KP_0;
    case kVK_ANSI_Keypad1: return INTVSESSION_KEYSYM_KP_1;
    case kVK_ANSI_Keypad2: return INTVSESSION_KEYSYM_KP_2;
    case kVK_ANSI_Keypad3: return INTVSESSION_KEYSYM_KP_3;
    case kVK_ANSI_Keypad4: return INTVSESSION_KEYSYM_KP_4;
    case kVK_ANSI_Keypad5: return INTVSESSION_KEYSYM_KP_5;
    case kVK_ANSI_Keypad6: return INTVSESSION_KEYSYM_KP_6;
    case kVK_ANSI_Keypad7: return INTVSESSION_KEYSYM_KP_7;
    case kVK_ANSI_Keypad8: return INTVSESSION_KEYSYM_KP_8;
    case kVK_ANSI_Keypad9: return INTVSESSION_KEYSYM_KP_9;
    case kVK_ANSI_KeypadDecimal: return INTVSESSION_KEYSYM_KP_PERIOD;
    case kVK_ANSI_KeypadEnter:   return INTVSESSION_KEYSYM_KP_ENTER;
    /* Number row -> right controller keypad, plus the three punctuation
     * keys Shift would otherwise mangle -- see the keyCode enum's comment. */
    case kVK_ANSI_1: return '1';
    case kVK_ANSI_2: return '2';
    case kVK_ANSI_3: return '3';
    case kVK_ANSI_4: return '4';
    case kVK_ANSI_5: return '5';
    case kVK_ANSI_6: return '6';
    case kVK_ANSI_7: return '7';
    case kVK_ANSI_8: return '8';
    case kVK_ANSI_9: return '9';
    case kVK_ANSI_0: return '0';
    case kVK_ANSI_Minus: return '-';
    case kVK_ANSI_Equal: return '=';
    case kVK_ANSI_Comma: return ',';
    case kVK_ANSI_Period: return '.';
    case kVK_ANSI_Semicolon: return ';';
    case kVK_Shift:        return INTVSESSION_KEYSYM_LSHIFT;
    case kVK_RightShift:   return INTVSESSION_KEYSYM_RSHIFT;
    case kVK_Control:      return INTVSESSION_KEYSYM_LCTRL;
    case kVK_RightControl: return INTVSESSION_KEYSYM_RCTRL;
    case kVK_Option:       return INTVSESSION_KEYSYM_LALT;
    case kVK_RightOption:  return INTVSESSION_KEYSYM_RALT;
    /* Non-printable keys the base controller map has no use for, but the
     * ECS keyboard map (forwarded below when "keyboard_mode" is on) does. */
    case kVK_Return: return INTVSESSION_KEYSYM_RETURN;
    case kVK_Delete: return INTVSESSION_KEYSYM_BACKSPACE;
    case kVK_Escape: return INTVSESSION_KEYSYM_ESCAPE;
    default:
        return 0;
    }
}

uint32_t IntvKeysymForEvent(NSEvent *event)
{
    uint32_t keysym = IntvKeysymForKeyCode(event.keyCode);
    if (keysym)
        return keysym;

    /* Everything the keyCode table deliberately leaves alone -- the
     * IJKM/DRWEASZXC disc letters above all -- resolves by character, which
     * keeps them layout-aware the way jzIntv's own SDL keycodes are. */
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    return (c >= 0x20 && c < 0x7F) ? (uint32_t)c : 0;
}

void IntvForwardKeyEvent(intvsession *session, NSEvent *event, int down)
{
    uint32_t keysym = IntvKeysymForEvent(event);
    intvsession_key_mapping m;

    if (!keysym)
        return; /* not a key the emulated controller has */

    /* "ECS Keyboard" input mode (Settings, or toggled live from there)
     * steals the keyboard for the ECS's own keyboard instead of the hand
     * controllers -- see intvsession_ecs_key_from_keysym's own comment on
     * why the two can't both claim it at once. */
    if (intvsession_get_int(session, "keyboard_mode", 0)) {
        intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
        if (key != INTVSESSION_ECS_KEY_NONE)
            intvsession_ecs_key_set(session, key, down);
        return;
    }

    m = intvsession_key_from_keysym(keysym);
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(session, m.side, down ? m.direction : -1);
}

void IntvForwardEcsKeyEvent(intvsession *session, NSEvent *event, int down)
{
    uint32_t keysym = IntvKeysymForEvent(event);
    intvsession_ecs_key key;

    if (!keysym)
        return;

    key = intvsession_ecs_key_from_keysym(keysym);
    if (key != INTVSESSION_ECS_KEY_NONE)
        intvsession_ecs_key_set(session, key, down);
}

BOOL IntvModifierKeyState(NSEvent *event, int *down)
{
    /* NSEventTypeFlagsChanged carries no down/up of its own -- infer it
     * from whether the bit for this specific key's side is now set, the
     * standard AppKit idiom (matches the CoCo/ADAM ports' own
     * flagsChanged: handling). */
    const NSEventModifierFlags f = event.modifierFlags;

    switch (event.keyCode) {
    case kVK_Shift:
    case kVK_RightShift:
        *down = (f & NSEventModifierFlagShift) != 0;
        return YES;
    case kVK_Control:
    case kVK_RightControl:
        *down = (f & NSEventModifierFlagControl) != 0;
        return YES;
    case kVK_Option:
    case kVK_RightOption:
        *down = (f & NSEventModifierFlagOption) != 0;
        return YES;
    default:
        return NO;
    }
}

@implementation IntvKeyWindow

/* NSWindow's own -keyDown: beeps at anything it doesn't recognise; these
 * overrides claim the event instead. Only events no control consumed get
 * this far up the responder chain, which is what makes clicking a keypad
 * button and then typing work. */
- (void)forward:(NSEvent *)event down:(int)down
{
    if (!_session)
        return;
    if (_ecsOnly)
        IntvForwardEcsKeyEvent(_session, event, down);
    else
        IntvForwardKeyEvent(_session, event, down);
}

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat)
        return; /* pad_t tracks a held key by level, not by repeat count */
    [self forward:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forward:event down:0];
}

- (void)flagsChanged:(NSEvent *)event
{
    int down;
    if (IntvModifierKeyState(event, &down))
        [self forward:event down:down];
    else
        [super flagsChanged:event];
}

/* Closing or backgrounding the panel must not leave a key stuck down in
 * whichever matrix was active -- see intvsession_ecs_keys_clear's own
 * comment. */
- (void)resignKeyWindow
{
    if (_session)
        intvsession_ecs_keys_clear(_session);
    [super resignKeyWindow];
}

@end
