/*
 * EcsKeyboardWindow for the macOS frontend.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface EcsKeyboardWindow : NSObject

/* Shows (creating on first use) the ECS keyboard window for the session,
 * or hides it if already visible -- a toggle, matching the GNOME/KDE/
 * Windows ports' own intv_ecskbd_window_toggle. */
+ (void)toggleForSession:(intvsession *)session;

@end
