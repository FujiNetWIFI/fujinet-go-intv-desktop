/*
 * DisplayView: the emulator video view for the macOS frontend.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface DisplayView : NSView

- (instancetype)initWithSession:(intvsession *)session;
- (void)start;
- (void)stop;

@end
