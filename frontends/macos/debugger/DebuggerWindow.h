/*
 * Debugger window for the macOS frontend.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface DebuggerWindow : NSObject

/* Shows (creating on first use) the debugger window for the session. */
+ (void)showForSession:(intvsession *)session;

@end
