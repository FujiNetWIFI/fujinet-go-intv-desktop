/*
 * DisplayExtras.inc -- the keyboard, included into the display view's
 * @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* ---- keyboard -------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    IntvForwardKeyEvent(_session, event, down);
}

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat)
        return; /* pad_t tracks a held key by level, not by repeat count */
    [self forwardKeyEvent:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKeyEvent:event down:0];
}

/* Losing first-responder status shouldn't leave a key stuck down in
 * whichever matrix was active -- see intvsession_ecs_keys_clear's own
 * comment. */
- (BOOL)resignFirstResponder
{
    intvsession_ecs_keys_clear(_session);
    return [super resignFirstResponder];
}

- (void)flagsChanged:(NSEvent *)event
{
    int down;
    if (IntvModifierKeyState(event, &down))
        [self forwardKeyEvent:event down:down];
}

