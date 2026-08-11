/*
 * AppDelegate: application lifecycle for the macOS frontend.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "intvsession.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>

- (instancetype)initWithSession:(intvsession *)session;

@end
