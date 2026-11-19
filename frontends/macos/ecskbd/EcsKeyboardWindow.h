/*
 * EcsKeyboardWindow for the macOS frontend: the ECS keyboard panel (F10), a
 * fixed-size floating NSPanel drawn from the core's ECS layout, hidden at
 * start.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface EcsKeyboardWindow : NSObject

/* Shows (creating on first use) the ECS keyboard panel for the session, or
 * hides it if already visible -- a toggle, matching the GNOME/KDE/Windows
 * ports' own intv_ecskbd_window_toggle. */
+ (void)toggleForSession:(intvsession *)session;
+ (BOOL)isVisible;
/* The keyboard mode (Settings) changed: read it again. */
+ (void)refresh;

@end
