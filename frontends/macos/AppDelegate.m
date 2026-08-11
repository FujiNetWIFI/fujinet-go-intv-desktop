/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet configuration (WKWebView) and console
 * log windows. Mirrors the GNOME/KDE/Windows frontends over the same
 * intvsession API, with the native debugger window in
 * debugger/DebuggerWindow.m and the keypad window in
 * keypad/KeypadWindow.m.
 *
 * No Machine/Display/CPU radio menus, unlike the CoCo/ADAM/MSX macOS
 * ports: the Intellivision has one fixed configuration, matching the
 * GNOME/KDE/Windows frontends' own menu scope (View: Keypad/Fullscreen/
 * Debugger; FujiNet: Configuration/Console Log).
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <WebKit/WebKit.h>

#import "DisplayView.h"
#import "debugger/DebuggerWindow.h"
#import "keypad/KeypadWindow.h"

#include <stdlib.h>

@interface AppDelegate () <NSWindowDelegate, WKUIDelegate>
@end

@implementation AppDelegate {
    intvsession *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- actions ------------------------------------------------------------ */

- (void)showKeypad:(id)sender
{
    (void)sender;
    [KeypadWindow toggleForSession:_session];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    if (_configWindow) {
        [_configWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 1000, 760);
    /* Miniaturizable so Window ▸ Minimize (and the yellow button) apply to
     * the auxiliary windows too, not just the machine window -- matches
     * the CoCo/ADAM ports' own reasoning. */
    _configWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _configWindow.title = @"FujiNet Configuration";
    _configWindow.releasedWhenClosed = NO;
    WKWebView *web = [[WKWebView alloc] initWithFrame:frame];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web.UIDelegate = self;
    NSString *url =
        [NSString stringWithUTF8String:intvsession_fujinet_webui_url(_session)];
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
    _configWindow.contentView = web;
    [_configWindow center];
    [_configWindow makeKeyAndOrderFront:nil];
}

/* The FujiNet web UI's OneDrive/Google Drive "Authorize" buttons open the
 * provider's consent page via window.open(). WKWebView has no popup window
 * of its own to show it in unless a UIDelegate supplies one, so hand the URL
 * to the system browser instead: Google and Microsoft both reject OAuth
 * flows from an embedded webview's user-agent anyway. Matches the CoCo/ADAM
 * ports' own webView:createWebViewWithConfiguration:. */
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    if (navigationAction.request.URL)
        [[NSWorkspace sharedWorkspace] openURL:navigationAction.request.URL];
    return nil;
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 820, 560);
    _logWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    _logWindow.contentView = scroll;

    _logTimer = [NSTimer
        scheduledTimerWithTimeInterval:1.0
                               repeats:YES
                                 block:^(NSTimer *timer) {
                                   (void)timer;
                                   [self refreshLog];
                                 }];
    [self refreshLog];
    [_logWindow center];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = intvsession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (text)
        _logView.string = text;
    [_logView scrollToEndOfDocument:nil];
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
}

/* ---- menus ---------------------------------------------------------------- */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:sel
                                           keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go Intv";
    NSMenu *menubar = [[NSMenu alloc] init];

    /* A menu bar built in code gets nothing for free: the standard items
     * every Mac app is expected to have (Services, Hide/Hide Others/Show
     * All, Minimize/Zoom, the window list) exist only if they are added
     * here -- and Services and the window list stay empty until AppKit is
     * told which menus they belong to (servicesMenu / windowsMenu). */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [appMenu addItemWithTitle:@"Services"
                                                  action:NULL
                                           keyEquivalent:@""];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menubar addItem:appItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    /* View: Keypad, Fullscreen, Debugger -- matching the GNOME/KDE/Windows
     * frontends' own View menu exactly (no Machine/Display/CPU groups). */
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *keypad =
        [self item:@"Keypad"
             action:@selector(showKeypad:)
                key:[NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey]];
    keypad.keyEquivalentModifierMask = 0;
    [view addItem:keypad];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                     action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    NSMenuItem *dbg =
        [self item:@"Debugger"
             action:@selector(showDebugger:)
                key:[NSString stringWithFormat:@"%C",
                              (unichar)NSF12FunctionKey]];
    dbg.keyEquivalentModifierMask = 0;
    [view addItem:dbg];
    viewItem.submenu = view;
    [menubar addItem:viewItem];

    /* Window menu. AppKit appends the list of open windows (the main
     * window, the debugger, the keypad, the FujiNet console log) to
     * whichever menu is handed to windowsMenu, and manages their check
     * marks -- that list is what "show or hide windows" means to a Mac
     * user. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    /* Close lives here rather than in a File menu: there is no File menu,
     * and the debugger / config / log / keypad windows need a keyboard
     * close. */
    [windowMenu addItemWithTitle:@"Close"
                          action:@selector(performClose:)
                   keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize"
                          action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    [menubar addItem:windowItem];
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = menubar;
}

/* ---- lifecycle --------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    [self buildMenus];

    /* 4:3 content area plus room for the title bar -- matches the display's
     * own letterbox aspect (DisplayView.m) instead of the raw 160x200
     * frame buffer's portrait shape, so the window doesn't open with big
     * black bars top and bottom. Same reasoning as GNOME's window.c. */
    NSRect frame = NSMakeRect(0, 0, 800, 650);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Intv";

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    /* Developer affordance shared with the other frontends. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
    if (getenv("INTV_OPEN_KEYPAD"))
        [KeypadWindow toggleForSession:_session];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_display stop];
    intvsession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end
