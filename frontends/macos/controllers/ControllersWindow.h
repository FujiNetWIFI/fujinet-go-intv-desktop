/*
 * The Controllers panel (F9): the Intellivision II handsets (four with the ECS
 * on) over the console strip, the gamepads with their assignments, and Map
 * mode for rebinding. Hidden at start; a fixed-size floating NSPanel. One
 * panel for the process: every entry point is a class method.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface IntvControllersWindow : NSObject
/* Show the panel, or hide it when it is showing. */
+ (void)toggleWithSession:(intvsession *)session;
/* Show the panel (it stays up when it already is). */
+ (void)showWithSession:(intvsession *)session;
+ (BOOL)isVisible;
/* The panel is the key window (it has the keyboard). */
+ (BOOL)isActive;
/* Settings changed the gamepads or the bindings: read them again. */
+ (void)refresh;
@end
