/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet configuration (WKWebView) and console
 * log windows. Mirrors the GNOME/KDE/Windows frontends over the same
 * intvsession API, with the native debugger window in
 * debugger/DebuggerWindow.m, the Controllers panel in
 * controllers/ControllersWindow.m and the ECS keyboard panel in
 * ecskbd/EcsKeyboardWindow.m.
 *
 * No Machine/Display/CPU radio menus, unlike the CoCo/ADAM/MSX macOS
 * ports: the Intellivision has one fixed configuration, matching the
 * GNOME/KDE/Windows frontends' own menu scope (View: Controllers/ECS Keyboard/
 * Fullscreen/Debugger; FujiNet: Configuration/Console Log).
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#import "DisplayView.h"
#import "help/HelpWindow.h"
#import "debugger/DebuggerWindow.h"
#import "controllers/ControllersWindow.h"
#import "ecskbd/EcsKeyboardWindow.h"

#include <stdlib.h>
#include <string.h>

/* How long a message (a gamepad coming or going) stays over the picture. */
#define INTV_TOAST_SECONDS 3.0

/* Gamepad rows in Settings' Input group (the Controllers panel has the same). */
#define INTV_SETTINGS_PADS 4

@interface AppDelegate () <NSWindowDelegate, WKUIDelegate>
- (void)showToast:(NSString *)text;
- (void)refreshPadList;
- (void)fitSettings;
- (void)startSettingsTimer;
- (void)resetControls:(id)sender;
- (void)openControllers:(id)sender;
- (void)padAssignChanged:(NSPopUpButton *)sender;
@end

@implementation AppDelegate {
    intvsession *_session;
    NSWindow *_window;
    DisplayView *_display;
    id<NSObject> _activity;          /* no App Nap, no timer coalescing while we run */
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    NSWindow *_settingsWindow;
    NSTextField *_externalField;
    BOOL _sessionDirty;
    NSTimer *_sysactTimer;
    NSView *_noRoms;
    NSMutableArray<NSTextField *> *_romStatus;
    NSPopUpButton *_ecsPop;
    NSTextField *_toast;             /* the hot-plug message over the picture */
    unsigned _toastGeneration;       /* the pads' generation the toast has seen */
    NSUInteger _toastSerial;
    NSGridView *_settingsGrid;
    NSInteger _padRowBase;           /* the grid row of the first gamepad row */
    NSTextField *_padName[INTV_SETTINGS_PADS];
    NSPopUpButton *_padAssign[INTV_SETTINGS_PADS];
    unsigned _padGeneration;         /* the pads' generation the Input rows show */
    int _padAssignEcs;               /* the assign popups were filled for this ecs_on */
    NSTimer *_settingsTimer;
}

/* Settings keys shared with the GNOME/KDE/Windows frontends (all four read/
 * write the same settings.ini -- see frontends/gnome/prefs.c's own header
 * for the canonical key/default list):
 *   "ecs"/"ivoice"    INTVSESSION_HW_* (default AUTO)     -- restart to apply
 *   "video_standard"  INTVSESSION_VIDEO_* (default NTSC)  -- restart to apply
 *   "keyboard_mode"   0 = hand controllers, 1 = ECS keyboard -- applies live
 *   "enable_gamepad"  1 = run the SDL gamepad thread (default) -- restart
 *   "fujinet_external"/"fujinet_external_addr"  dial another FujiNet -- restart
 * The restart-class key set, mirroring the CoCo/ADAM macOS ports' own
 * sessionSettingKeys()/machineSettingKeys() helpers. */
static NSArray<NSString *> *restartSettingKeys(void)
{
    static NSArray<NSString *> *keys;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      keys = @[ @"ecs", @"ivoice", @"video_standard", @"enable_gamepad",
                @"fujinet_external", @"fujinet_external_addr" ];
    });
    return keys;
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* Stops and restarts the session with intvsession_default_opts() re-read
 * from the settings store -- the "apply" side of a Settings window change
 * to ECS/Intellivoice/video standard, matching the GNOME/KDE/Windows
 * frontends' own restart helpers. */
- (void)restartSession
{
    intvsession_start_opts opts;
    intvsession_stop(_session);
    if (!intvsession_has_system_roms(_session))
        return; /* the No BIOS page is showing; an import starts it */
    intvsession_default_opts(_session, &opts);
    if (intvsession_start(_session, &opts) != 0) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet Go Intv";
        alert.informativeText =
            [NSString stringWithUTF8String:intvsession_last_error(_session)];
        [alert runModal];
    }
}

/* ---- BIOS import ---------------------------------------------------------- */

/* In place of the picture until the Executive and Graphics ROMs are there. */
- (NSView *)buildNoRoms:(NSRect)frame
{
    NSImageView *icon =
        [NSImageView imageViewWithImage:[NSApp applicationIconImage]];
    [icon.widthAnchor constraintEqualToConstant:96].active = YES;
    [icon.heightAnchor constraintEqualToConstant:96].active = YES;
    NSTextField *title = [NSTextField labelWithString:@"No Intellivision BIOS"];
    title.font = [NSFont boldSystemFontOfSize:22];
    NSTextField *desc = [NSTextField
        wrappingLabelWithString:
            @"The Intellivision Executive and Graphics ROMs are not part of "
            @"the app. Import the ones you have \u2014 exec.bin and grom.bin, "
            @"a zip such as MAME\u2019s intv set, or a folder \u2014 and the "
            @"Intellivision starts."];
    desc.alignment = NSTextAlignmentCenter;
    desc.textColor = NSColor.secondaryLabelColor;
    desc.preferredMaxLayoutWidth = 460;
    [desc.widthAnchor constraintEqualToConstant:460].active = YES;
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS\u2026"
                                         target:self
                                         action:@selector(importBios:)];
    files.bezelColor = NSColor.controlAccentColor;
    files.keyEquivalent = @"\r";
    NSButton *folder = [NSButton buttonWithTitle:@"Import a Folder\u2026"
                                          target:self
                                          action:@selector(importBiosFromFolder:)];
    NSButton *helpBtn = IntvHelpButton(@"nobios");
    NSStackView *buttons =
        [NSStackView stackViewWithViews:@[ files, folder, helpBtn ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 12;
    NSStackView *column =
        [NSStackView stackViewWithViews:@[ icon, title, desc, buttons ]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeCenterX;
    column.spacing = 14;
    column.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *page = [[NSView alloc] initWithFrame:frame];
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [page addSubview:column];
    [column.centerXAnchor constraintEqualToAnchor:page.centerXAnchor].active =
        YES;
    [column.centerYAnchor constraintEqualToAnchor:page.centerYAnchor].active =
        YES;
    return page;
}

/* The picture while the machine can run, else the No-BIOS page. */
- (void)showPage
{
    BOOL ok = intvsession_is_running(_session) ||
              intvsession_has_system_roms(_session);
    _display.hidden = !ok;
    _noRoms.hidden = ok;
    IntvSetHelpContext(_window, ok ? @"main" : @"nobios");
    if (ok)
        [_window makeFirstResponder:_display];
}

- (void)refreshRomRows
{
    for (NSUInteger i = 0; i < _romStatus.count; i++)
        _romStatus[i].stringValue =
            intvsession_rom_item_available(_session, (int)i) ? @"Imported"
                                                             : @"Not imported";
    if (_ecsPop) {
        BOOL have = intvsession_has_ecs_rom(_session) != 0;
        _ecsPop.enabled = have;
        _ecsPop.toolTip =
            have ? @"Entertainment Computer System \u2014 second controller "
                   @"pair, ECS keyboard, extra sound"
                 : @"Unavailable: ecs.bin not found in the ROM directory";
    }
}

/* (Re)starts the machine once its ROMs are there. A running session has to be
 * stopped and started again for new ROMs to take effect. */
- (void)startMachine
{
    if (intvsession_has_system_roms(_session))
        [self restartSession];
    [self showPage];
}

/* Each chosen path through the import, then one alert says what came of it. */
- (void)importBiosURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSURL *u in urls) {
        char msg[1024];
        msg[0] = '\0';
        if (intvsession_rom_import(_session, u.fileSystemRepresentation, msg,
                                   (int)sizeof msg) == 0)
            imported++;
        if (msg[0])
            [log addObject:[NSString stringWithFormat:@"%@: %s",
                                                      u.lastPathComponent, msg]];
    }
    if (imported)
        [self startMachine];
    [self refreshRomRows];
    if (log.count) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = imported ? @"BIOS Imported" : @"Nothing Imported";
        a.informativeText = [log componentsJoinedByString:@"\n"];
        [a runModal];
    }
}

- (void)importBiosChoosingFolder:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = folder
                        ? @"Import BIOS from a Folder (MAME\u2019s intv set works)"
                        : @"Import BIOS: ROM images, zips or folders";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = !folder;
    panel.allowsMultipleSelection = !folder;
    if (!folder) {
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *e in @[ @"zip", @"rom", @"bin" ]) {
            UTType *t = [UTType typeWithFilenameExtension:e];
            if (t)
                [types addObject:t];
        }
        [types addObject:UTTypeData];
        panel.allowedContentTypes = types;
    }
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;
    [self importBiosURLs:panel.URLs];
}

- (void)importBios:(id)sender
{
    (void)sender;
    [self importBiosChoosingFolder:NO];
}

- (void)importBiosFromFolder:(id)sender
{
    (void)sender;
    [self importBiosChoosingFolder:YES];
}

/* ---- SD host ------------------------------------------------------------- */

- (void)importCartToSd:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Import a cartridge to the SD host";
    panel.allowsMultipleSelection = YES;
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *e in @[ @"rom", @"bin", @"int", @"itv", @"cfg", @"zip" ]) {
        UTType *t = [UTType typeWithFilenameExtension:e];
        if (t)
            [types addObject:t];
    }
    [types addObject:UTTypeData];
    panel.allowedContentTypes = types;
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;

    NSUInteger ok = 0;
    NSString *firstName = nil;
    for (NSURL *u in panel.URLs) {
        char dest[1024];
        dest[0] = '\0';
        if (intvsession_import_cart_to_sd(_session, u.fileSystemRepresentation,
                                          dest, (int)sizeof dest) != 0) {
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = @"Import failed";
            a.informativeText = [NSString
                stringWithUTF8String:intvsession_last_error(_session)];
            [a runModal];
            continue;
        }
        if (!ok)
            firstName = u.lastPathComponent;
        ok++;
    }
    if (!ok)
        return;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = ok == 1 ? @"Cartridge Imported" : @"Cartridges Imported";
    a.informativeText =
        ok == 1 ? [NSString stringWithFormat:
                                @"%@ is on the SD host. Boot it from CONFIG.",
                                firstName]
                : [NSString stringWithFormat:@"%lu files are on the SD host. "
                                             @"Boot them from CONFIG.",
                                             (unsigned long)ok];
    [a runModal];
}

- (void)openSdFolder:(id)sender
{
    (void)sender;
    const char *p = intvsession_sd_path(_session);
    if (!p || !*p)
        return;
    [[NSFileManager defaultManager]
              createDirectoryAtPath:@(p)
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(p)]];
}

/* ---- actions ------------------------------------------------------------ */

- (void)showControllers:(id)sender
{
    (void)sender;
    [IntvControllersWindow toggleWithSession:_session];
}

- (void)showEcsKeyboard:(id)sender
{
    (void)sender;
    [EcsKeyboardWindow toggleForSession:_session];
}

- (void)resetToConfig:(id)sender
{
    (void)sender;
    if (intvsession_reset_to_config(_session) != 0) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet Go Intv";
        alert.informativeText =
            [NSString stringWithUTF8String:intvsession_last_error(_session)];
        [alert runModal];
    }
}

- (void)resetGame:(id)sender
{
    (void)sender;
    if (intvsession_reset_game(_session) != 0) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet Go Intv";
        alert.informativeText =
            [NSString stringWithUTF8String:intvsession_last_error(_session)];
        [alert runModal];
    }
}

/* Drains intvsession_sysaction_take -- the one caller that can't fire a
 * sysaction directly is gamepad_sdl.c's SDL thread (queuing RESET_CONFIG so
 * it never joins itself); this, the UI thread, fires each on the tick
 * below. The Controllers panel's RESET and system buttons are queued here too,
 * so a failure is reported in one place. Lives on the app delegate rather than
 * the panel: the panel hides rather than closes but is hidden far more often
 * than shown, and a gamepad-bound sysaction has to keep working regardless.
 * Handles all three actions (Reset Game, Reset to CONFIG, Stop in debugger). */
- (void)drainSysactions
{
    intvsession_sysaction a;
    while (intvsession_sysaction_take(_session, &a)) {
        if (intvsession_sysaction_fire(_session, a) != 0) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"FujiNet Go Intv";
            alert.informativeText = [NSString
                stringWithUTF8String:intvsession_last_error(_session)];
            [alert runModal];
            continue;
        }
        switch (a) {
        case INTVSESSION_SYSACT_DEBUG_STOP:
            /* the machine is stopping; the debugger window follows the stop */
            [DebuggerWindow showForSession:_session];
            break;
        case INTVSESSION_SYSACT_RESET_GAME:
        case INTVSESSION_SYSACT_RESET_CONFIG:
        default:
            break;
        }
    }
}

/* A message over the top of the picture that fades (a gamepad coming or
 * going). A newer message restarts the clock; hidden when faded, so it never
 * takes a click. */
- (void)showToast:(NSString *)text
{
    if (!text || !_toast) return;
    _toast.stringValue = text;
    _toast.hidden = NO;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    __weak AppDelegate *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(INTV_TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        AppDelegate *strongSelf = weakSelf;
        if (!strongSelf || serial != strongSelf->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            strongSelf->_toast.animator.alphaValue = 0.0;
        } completionHandler:^{
            if (serial == strongSelf->_toastSerial) strongSelf->_toast.hidden = YES;
        }];
    });
}

/* Pads coming and going: the panels read them again, the picture says so. */
- (void)checkGamepadEvents
{
    const unsigned gen = intvsession_gamepad_generation(_session);
    if (gen == _toastGeneration) return;
    _toastGeneration = gen;
    char msg[160];
    msg[0] = '\0';
    if (intvsession_gamepad_last_event(_session, msg, (int)sizeof msg) > 0)
        [self showToast:[NSString stringWithUTF8String:msg]];
    [IntvControllersWindow refresh];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    if (intvsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of the FujiNet instance "
                                @"this app dials.";
        [alert runModal];
        return;
    }
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
    if (intvsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process "
                                @"that is running it.";
        [alert runModal];
        return;
    }
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

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 820, 524)];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:frame];
        NSButton *help = IntvHelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    IntvSetHelpContext(_logWindow, @"fujilog");

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

/* ---- settings -------------------------------------------------------- */

- (NSPopUpButton *)hwModePopUpForKey:(const char *)key
{
    NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                     pullsDown:NO];
    pop.target = self;
    pop.action = @selector(settingChanged:);
    pop.identifier = [NSString stringWithUTF8String:key];
    for (int i = 0; intvsession_hw_mode_name(i); i++)
        [pop addItemWithTitle:[NSString
                                  stringWithUTF8String:intvsession_hw_mode_name(i)]];
    [pop selectItemAtIndex:intvsession_get_int(_session, key,
                                               INTVSESSION_HW_AUTO)];
    return pop;
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    NSString *key = control.identifier;
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    intvsession_set_int(_session, key.UTF8String, value);

    if ([key isEqualToString:@"fujinet_external"])
        _externalField.enabled = value != 0;
    if ([restartSettingKeys() containsObject:key]) {
        _sessionDirty = YES;
    } else if ([key isEqualToString:@"keyboard_mode"]) {
        if (value == 0) {
            /* Leaving ECS keyboard mode -- release anything still held; see
             * intvsession_ecs_keys_clear's own comment. */
            intvsession_ecs_keys_clear(_session);
        }
        [EcsKeyboardWindow refresh];
    }
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:65503) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (intvsession_parse_hostport(text.UTF8String, host, sizeof host,
                                   &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    intvsession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _sessionDirty = YES;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [self refreshPadList];
        [self startSettingsTimer];
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    BOOL haveEcsRom = intvsession_has_ecs_rom(_session);
    NSPopUpButton *ecsPop = [self hwModePopUpForKey:"ecs"];
    _ecsPop = ecsPop;
    ecsPop.enabled = haveEcsRom;
    ecsPop.toolTip =
        haveEcsRom
            ? @"Entertainment Computer System -- second controller pair, "
              @"ECS keyboard, extra sound"
            : @"Unavailable: ecs.bin not found in the ROM directory";

    NSPopUpButton *videoPop =
        [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    videoPop.target = self;
    videoPop.action = @selector(settingChanged:);
    videoPop.identifier = @"video_standard";
    for (int i = 0; intvsession_video_name(i); i++)
        [videoPop
            addItemWithTitle:[NSString
                                 stringWithUTF8String:intvsession_video_name(i)]];
    [videoPop selectItemAtIndex:intvsession_get_int(_session, "video_standard",
                                                     INTVSESSION_VIDEO_NTSC)];

    NSButton *ecsKeyboard =
        [NSButton checkboxWithTitle:@"The keyboard types on the ECS keyboard"
                             target:self
                             action:@selector(settingChanged:)];
    ecsKeyboard.state = intvsession_get_int(_session, "keyboard_mode", 0)
                            ? NSControlStateValueOn
                            : NSControlStateValueOff;
    ecsKeyboard.identifier = @"keyboard_mode";

    NSButton *externalBox =
        [NSButton checkboxWithTitle:@""
                             target:self
                             action:@selector(settingChanged:)];
    externalBox.state = intvsession_get_int(_session, "fujinet_external", 0)
                            ? NSControlStateValueOn
                            : NSControlStateValueOff;
    externalBox.identifier = @"fujinet_external";
    _externalField = [NSTextField textFieldWithString:@(
        intvsession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:65503";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled =
        intvsession_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    NSButton *gamepadsBox =
        [NSButton checkboxWithTitle:@"Gamepads (applied when this window closes)"
                             target:self
                             action:@selector(settingChanged:)];
    gamepadsBox.state = intvsession_get_int(_session, "enable_gamepad", 1)
                            ? NSControlStateValueOn
                            : NSControlStateValueOff;
    gamepadsBox.identifier = @"enable_gamepad";

    NSMutableArray<NSArray<NSView *> *> *all = [NSMutableArray arrayWithArray:@[
        @[ [NSTextField labelWithString:@"ECS"], ecsPop ],
        @[ [NSTextField labelWithString:@"Intellivoice"],
           [self hwModePopUpForKey:"ivoice"] ],
        @[ [NSTextField labelWithString:@"Video Standard"], videoPop ],
        @[ [NSTextField labelWithString:@"Use external FujiNet"], externalBox ],
        @[ [NSTextField labelWithString:@"Address:port"], _externalField ],
    ]];
    /* Input: the keyboard's route, the gamepads with the side each drives. */
    {
        NSTextField *inputHead = [NSTextField labelWithString:@"Input"];
        inputHead.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
        NSTextField *inputNote = [NSTextField labelWithString:@"applied immediately, except Gamepads"];
        inputNote.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        inputNote.textColor = NSColor.secondaryLabelColor;
        [all addObject:@[ inputHead, inputNote ]];
        [all addObject:@[ [NSTextField labelWithString:@"ECS Keyboard"], ecsKeyboard ]];
        [all addObject:@[ [NSTextField labelWithString:@""], gamepadsBox ]];
        _padRowBase = (NSInteger)all.count;
        _padAssignEcs = -1;
        for (int row = 0; row < INTV_SETTINGS_PADS; row++) {
            _padName[row] = [NSTextField labelWithString:@""];
            _padName[row].font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
            _padName[row].lineBreakMode = NSLineBreakByTruncatingTail;
            [_padName[row].widthAnchor constraintEqualToConstant:230].active = YES;
            _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            _padAssign[row].tag = row;
            _padAssign[row].target = self;
            _padAssign[row].action = @selector(padAssignChanged:);
            NSStackView *padRow = [NSStackView stackViewWithViews:@[ _padName[row], _padAssign[row] ]];
            padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            [all addObject:@[ [NSTextField labelWithString:(row == 0 ? @"Gamepads" : @"")], padRow ]];
        }
        NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls"
                                             target:self
                                             action:@selector(resetControls:)];
        NSButton *open = [NSButton buttonWithTitle:@"Open Controllers"
                                            target:self
                                            action:@selector(openControllers:)];
        NSButton *inputHelp = IntvHelpButton(@"prefs/Input");
        NSStackView *buttons = [NSStackView stackViewWithViews:@[ reset, open, inputHelp ]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        [all addObject:@[ [NSTextField labelWithString:@""], buttons ]];
        NSTextField *mapNote = [NSTextField labelWithString:@"Keys and gamepad buttons are mapped in the Controllers window (F9)."];
        mapNote.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        mapNote.textColor = NSColor.secondaryLabelColor;
        [all addObject:@[ [NSTextField labelWithString:@""], mapNote ]];
    }
    /* ROMs: one row per item under its group, refreshed after an import. */
    NSTextField *romsHead = [NSTextField labelWithString:@"ROMs"];
    romsHead.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    [all addObject:@[ romsHead, [NSTextField labelWithString:@""] ]];
    _romStatus = [NSMutableArray array];
    const char *group = "";
    for (int i = 0; i < intvsession_rom_item_count(); i++) {
        const intv_rom_item *it = intvsession_rom_item(i);
        if (strcmp(group, it->group) != 0) {
            group = it->group;
            NSTextField *g = [NSTextField
                labelWithString:[NSString stringWithUTF8String:group]];
            g.textColor = NSColor.secondaryLabelColor;
            [all addObject:@[ g, [NSTextField labelWithString:@""] ]];
        }
        NSTextField *st = [NSTextField labelWithString:@""];
        [_romStatus addObject:st];
        [all addObject:@[
            [NSTextField
                labelWithString:[NSString stringWithUTF8String:it->name]],
            st
        ]];
    }
    NSButton *impFiles = [NSButton buttonWithTitle:@"Import\u2026"
                                            target:self
                                            action:@selector(importBios:)];
    NSButton *impFolder = [NSButton buttonWithTitle:@"Import a Folder\u2026"
                                             target:self
                                             action:@selector(importBiosFromFolder:)];
    NSStackView *impRow =
        [NSStackView stackViewWithViews:@[ impFiles, impFolder ]];
    impRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    impRow.spacing = 8;
    [all addObject:@[ [NSTextField labelWithString:@""], impRow ]];
    [self refreshRomRows];

    NSGridView *grid = [NSGridView gridViewWithViews:all];
    _settingsGrid = grid;
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"ECS/Intellivoice/Video Standard and Gamepads apply "
                        @"when this window is closed (the session "
                        @"restarts). ECS Keyboard applies immediately. An external FujiNet "
                        @"is dialled at the address above: start it "
                        @"listening there (its [BOIP] port).\n\nWarning: the "
                        @"FujiNet CONFIG ROM has no ECS keyboard support "
                        @"yet, so while ECS Keyboard is on its menus can "
                        @"only be driven by a gamepad or the Controllers "
                        @"window."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    note.preferredMaxLayoutWidth = 360;
    note.lineBreakMode = NSLineBreakByWordWrapping;
    note.maximumNumberOfLines = 0;

    NSTextField *kept = [NSTextField
        labelWithString:[NSString stringWithFormat:@"Kept in %s.",
                                                   intvsession_roms_path(_session)]];
    kept.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    kept.textColor = NSColor.secondaryLabelColor;
    kept.preferredMaxLayoutWidth = 360;
    kept.lineBreakMode = NSLineBreakByWordWrapping;
    kept.maximumNumberOfLines = 0;
    kept.selectable = YES;

    NSStackView *root =
        [NSStackView stackViewWithViews:@[ grid, kept, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    [root layoutSubtreeIfNeeded];
    NSSize fit = root.fittingSize;
    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, MAX(fit.width, 420), fit.height)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    {
        /* the settings over a strip with the Help button */
        NSButton *helpBtn = IntvHelpButton(@"prefs/Machine");
        NSStackView *wrap = [NSStackView stackViewWithViews:@[root, helpBtn]];
        wrap.orientation = NSUserInterfaceLayoutOrientationVertical;
        wrap.alignment = NSLayoutAttributeLeading;
        wrap.edgeInsets = NSEdgeInsetsMake(0, 16, 12, 16);
        _settingsWindow.contentView = wrap;
        IntvSetHelpContext(_settingsWindow, @"prefs/Machine");
    }
    [self fitSettings];
    [_settingsWindow center];
    _padGeneration = intvsession_gamepad_generation(_session);
    [self refreshPadList];
    [self startSettingsTimer];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Automatic, then one item per side the session has: Left and Right, and with
 * the ECS on, ECS Left and ECS Right. Filled again when ecs_on changes (a
 * restart), as the Settings window outlives the session. */
- (void)fillPadAssignPopups
{
    const int ecs = intvsession_ecs_on(_session) ? 1 : 0;
    if (ecs == _padAssignEcs) return;
    _padAssignEcs = ecs;
    for (int row = 0; row < INTV_SETTINGS_PADS; row++) {
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemWithTitle:@"Automatic"];
        for (int side = 0; side < (ecs ? 4 : 2); side++)
            [_padAssign[row] addItemWithTitle:[NSString stringWithUTF8String:intvsession_pad_side_name((intvsession_pad_side)side)]];
    }
}

/* The Input group's gamepad rows: one per connected pad (up to four), its name
 * and the side it drives, and the side it is set to. Rows with no pad are
 * hidden; the first says so. */
- (void)refreshPadList
{
    if (!_settingsWindow) return;
    [self fillPadAssignPopups];
    const int count = intvsession_gamepad_count(_session);
    const int sides = _padAssignEcs ? 4 : 2;
    _padGeneration = intvsession_gamepad_generation(_session);
    for (int row = 0; row < INTV_SETTINGS_PADS; row++) {
        const BOOL shown = row < count;
        char name[128];
        name[0] = '\0';
        if (shown) intvsession_gamepad_name(_session, row, name, (int)sizeof name);
        const int eff = shown ? intvsession_gamepad_effective_side(_session, row) : -1;
        NSString *text;
        if (!shown)
            text = row == 0 ? @"(no gamepads connected)" : @"";
        else if (eff >= 0)
            text = [NSString stringWithFormat:@"%s  [%s]", name, intvsession_pad_side_name((intvsession_pad_side)eff)];
        else
            text = [NSString stringWithFormat:@"%s  [unused]", name];
        _padName[row].stringValue = text;
        _padAssign[row].hidden = !shown;
        if (shown) {
            const int assign = intvsession_gamepad_assignment(_session, row);
            [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= sides ? 0 : assign + 1)];
        }
        if (_settingsGrid) [_settingsGrid rowAtIndex:_padRowBase + row].hidden = !shown && row > 0;
    }
    [self fitSettings];
}

/* The window follows its content when gamepad rows come and go. */
- (void)fitSettings
{
    if (!_settingsWindow || !_settingsWindow.contentView) return;
    const NSSize fit = _settingsWindow.contentView.fittingSize;
    if (fit.width > 0 && fit.height > 0) [_settingsWindow setContentSize:fit];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    intvsession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadList];
    [IntvControllersWindow refresh];
}

/* Reset Controls: the default keys and gamepad buttons, every gamepad back on
 * Automatic. */
- (void)resetControls:(id)sender
{
    (void)sender;
    intvsession_bindings_reset(_session);
    for (int idx = 0, num = intvsession_gamepad_count(_session); idx < num; idx++)
        intvsession_gamepad_assign(_session, idx, -1);
    [self refreshPadList];
    [IntvControllersWindow refresh];
}

- (void)openControllers:(id)sender
{
    (void)sender;
    [IntvControllersWindow showWithSession:_session];
}

/* Gamepads coming and going, and what Auto resolves to, while the window is
 * up. The timer goes with the window: reopening starts it again. */
- (void)startSettingsTimer
{
    if (_settingsTimer) return;
    __weak AppDelegate *weakSelf = self;
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        AppDelegate *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (intvsession_gamepad_generation(strongSelf->_session) != strongSelf->_padGeneration ||
            (intvsession_ecs_on(strongSelf->_session) ? 1 : 0) != strongSelf->_padAssignEcs)
            [strongSelf refreshPadList];
    }];
}

/* Deferred restart-class apply -- collected while the Settings window is
 * open, applied once when it closes (mirroring the GNOME/KDE/Windows
 * frontends' own "apply on close" idiom). */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _settingsWindow) {
        [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
        [_settingsTimer invalidate];
        _settingsTimer = nil;
    }
    if (note.object != _settingsWindow || !_sessionDirty)
        return;
    _sessionDirty = NO;
    [self restartSession];
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

    [appMenu addItem:[self item:@"Settings…"
                         action:@selector(showSettings:)
                            key:@","]];
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
    /* No keyEquivalent here, deliberately: Backspace is a *remappable*
     * default (core/src/bindings.c), not a fixed accelerator like Cmd+R
     * below. AppKit's -performKeyEquivalent: chain runs before a window's
     * own -keyDown:, so binding one here would fire Reset Game on plain
     * Backspace forever, even after a Controllers window Map-mode remap moved
     * that action to a different key. The mouse-clickable menu entry is
     * enough; the real trigger is IntvForwardKeyEvent's MAP_SYSACT dispatch
     * (IntvKeyForward.m), which honours whatever the user has actually
     * bound. */
    [fuji addItem:[self item:@"Reset Game (Backspace)"
                      action:@selector(resetGame:)
                         key:@""]];
    [fuji addItem:[self item:@"Reset to CONFIG"
                      action:@selector(resetToConfig:)
                         key:@"r"]];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machineMenu addItem:[self item:@"Import BIOS\u2026"
                             action:@selector(importBios:)
                                key:@""]];
    [machineMenu addItem:[self item:@"Import BIOS from a Folder\u2026"
                             action:@selector(importBiosFromFolder:)
                                key:@""]];
    [machineMenu addItem:[NSMenuItem separatorItem]];
    [machineMenu addItem:[self item:@"Import Cartridge to SD\u2026"
                             action:@selector(importCartToSd:)
                                key:@""]];
    [machineMenu addItem:[self item:@"Open SD Folder"
                             action:@selector(openSdFolder:)
                                key:@""]];
    machineItem.submenu = machineMenu;
    [menubar addItem:machineItem];

    /* View: Controllers, ECS Keyboard, Fullscreen, Debugger -- matching the GNOME/KDE/Windows
     * frontends' own View menu exactly (no Machine/Display/CPU groups). */
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *controllers =
        [self item:@"Controllers (F9)"
             action:@selector(showControllers:)
                key:[NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey]];
    controllers.keyEquivalentModifierMask = 0;
    [view addItem:controllers];
    NSMenuItem *ecsKeyboard =
        [self item:@"ECS Keyboard (F10)"
             action:@selector(showEcsKeyboard:)
                key:[NSString stringWithFormat:@"%C",
                              (unichar)NSF10FunctionKey]];
    ecsKeyboard.keyEquivalentModifierMask = 0;
    [view addItem:ecsKeyboard];
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
     * window, the debugger, the controllers, the FujiNet console log) to
     * whichever menu is handed to windowsMenu, and manages their check
     * marks -- that list is what "show or hide windows" means to a Mac
     * user. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    /* Close lives here rather than in a File menu: there is no File menu,
     * and the debugger / config / log / controllers windows need a keyboard
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

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [help addItem:[self item:@"FujiNet Go Intv Help" action:@selector(openHelp:) key:@"?"]];
    helpItem.submenu = help;
    [menubar addItem:helpItem];

    NSApp.mainMenu = menubar;
    NSApp.helpMenu = help;
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[IntvHelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[IntvHelpWindow shared] showFor:[NSApp keyWindow]];
}

/* ---- lifecycle --------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* An emulator is latency-critical work: App Nap must not throttle the
     * machine thread or coalesce its timers when the window is behind
     * another, and Game Mode (Info.plist) asks the system to favour it. */
    _activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                          reason:@"Emulating an Intellivision"];
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
    /* Full screen is where Game Mode (Info.plist) engages. */
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];
    _noRoms = [self buildNoRoms:((NSView *)_window.contentView).bounds];
    [_window.contentView addSubview:_noRoms];

    /* The toast: a label over the top of the picture that fades. Hidden when
     * faded, so it never takes a click. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    _toast.hidden = YES;
    {
        const NSRect bounds = ((NSView *)_window.contentView).bounds;
        _toast.frame = NSMakeRect(0, bounds.size.height - 34, bounds.size.width, 26);
    }
    _toast.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [_window.contentView addSubview:_toast];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];
    [self showPage];

    /* ~100ms: fast enough that a gamepad press feels immediate, cheap
     * enough to run for the app's whole lifetime. See -drainSysactions's
     * own comment. It also watches the pads coming and going. */
    _toastGeneration = intvsession_gamepad_generation(_session);
    __weak AppDelegate *weakSelf = self;
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1
                                                     repeats:YES
                                                       block:^(NSTimer *timer) {
                                                         (void)timer;
                                                         [weakSelf drainSysactions];
                                                         [weakSelf checkGamepadEvents];
                                                       }];

    /* Developer affordance shared with the other frontends. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
    if (getenv("INTV_OPEN_KEYPAD") || getenv("INTV_OPEN_CONTROLLERS"))
        [IntvControllersWindow toggleWithSession:_session];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_sysactTimer invalidate];
    [_display stop];
    if (_activity) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    intvsession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end
