/*
 * FujiNet Go Intv -- macOS frontend entry point.
 *
 * NOT BUILT OR RUN-VERIFIED: written on Linux, following the GNOME/KDE/
 * Windows ports' already-verified structure, but there is no way to build
 * or run an AppKit target in this environment. Treat this file (and the
 * rest of frontends/macos/) as unverified until it has actually been built
 * and run on real macOS hardware or CI -- see TODO.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "intvsession.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        /* Unlike cocosession/msxsession, intvsession_paths has no
         * fujinet_lib/fujinet_runtime_src override fields -- paths.c's own
         * exe_dir() search already finds a runtime bundled right next to
         * the executable (Contents/MacOS, where this CMakeLists.txt copies
         * it), so passing NULL here is enough. */
        intvsession *session = intvsession_new(NULL);
        if (!session) {
            fprintf(stderr, "fatal: could not create the session\n");
            return 1;
        }
        intvsession_start_opts opts;
        intvsession_default_opts(session, &opts);
        if (intvsession_start(session, &opts) != 0)
            fprintf(stderr, "session start: %s\n",
                    intvsession_last_error(session));

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [[AppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app run];

        intvsession_free(session);
    }
    return 0;
}
