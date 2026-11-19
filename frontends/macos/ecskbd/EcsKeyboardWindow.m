/*
 * EcsKeyboardWindow (AppKit) -- the ECS keyboard as a picture: the beige case
 * with its label plate, ventilation ridges, red pinstripe and cable stub, and
 * the 48 black caps (two of them SHIFT, as on the real one, a white RTN, the
 * long SPACE bar) with their shifted legends above, all painted from the
 * session's layout (intvsession_ecs_layout) and hit-tested by the core
 * (intvsession_ecs_hit), so every frontend draws the same keyboard. The art
 * follows ~/Workspace/fujinet-go-all/controllers-port/DESIGN-intv.md section 4.
 *
 * Each cap drives intvsession_ecs_key_set directly, whether or not the
 * "keyboard_mode" setting routes the host keyboard to the ECS: opening this
 * panel never silently changes that setting, and typing in it always reaches
 * the ECS matrix. A cap is held for as long as the mouse is down on it. SHIFT
 * and CTL latch instead (a mouse cannot hold two buttons to chord a shifted
 * letter): a click sets the key, another clears it, and a latched cap stays
 * lit until then or until intvsession_ecs_keys_clear. A cap is lit while
 * intvsession_ecs_key_held says so, which also shows what the host keyboard
 * is typing.
 *
 * FIXED SIZE: an NSPanel without a resize mask, floating, hidden at start,
 * ordered out rather than closed. The 30 Hz poll runs only while the panel
 * can be seen. Everything held is let go when the panel stops being the key
 * window (its key events are the panel's own, and the main window's must not
 * strand a key either).
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "EcsKeyboardWindow.h"

#import "../controllers/ControllersWindow.h"
#import "../controllers/IntvCtlDraw.h"
#import "../debugger/DebuggerWindow.h"
#import "../help/HelpWindow.h"
#import "../IntvKeyForward.h"

#include <string.h>

#define IVE_UNIT         22.0     /* points per layout unit */
#define IVE_PAD           8.0
#define IVE_MARGIN       16.0
#define IVE_STATUS_H     34.0
#define IVE_SLOW_TICKS    8       /* the 30 Hz tick's 250 ms part */
#define IVE_MAX_CAPS     64
#define IVE_COL_STAGE_TOP 0x6B6C72
#define IVE_COL_STAGE_BOT 0x55565B
#define IVE_COL_STAGE_EDGE 0x3A3B3F
#define IVE_COL_BLACK    0x000000

/* The keys the app owns, as the HID band keysyms (usage 0x42..0x45). */
#define IVE_KEYSYM_F9   (INTVSESSION_KEYSYM_HID_BASE + 0x42u)
#define IVE_KEYSYM_F10  (INTVSESSION_KEYSYM_HID_BASE + 0x43u)
#define IVE_KEYSYM_F11  (INTVSESSION_KEYSYM_HID_BASE + 0x44u)
#define IVE_KEYSYM_F12  (INTVSESSION_KEYSYM_HID_BASE + 0x45u)

/* ---- views ---------------------------------------------------------------------- */

@interface IntvEcsFlippedView : NSView
@end

@implementation IntvEcsFlippedView
- (BOOL)isFlipped { return YES; }
@end

@class EcsKeyboardWindow;

/* Keys typed with the panel key go to the ECS matrix. */
@interface IntvEcsPanel : NSPanel
@property (nonatomic, weak) EcsKeyboardWindow *owner;
@end

/* Everything the panel and the views call on the window. */
@interface EcsKeyboardWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(intvsession *)session;
- (void)show;
- (void)hide;
- (BOOL)isShowing;
- (void)reload;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* The keyboard picture. */
@interface IntvEcsBoardView : IntvEcsFlippedView
@property (nonatomic) intvsession *session;
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads which caps are lit; redraws when anything changed. */
- (void)poll;
/* Lets go of the cap the mouse holds. */
- (void)letGo;
@end

@implementation IntvEcsBoardView {
    const intvsession_ecs_cap *_caps;   /* the core's static table */
    int _count;
    float _dimW, _dimH;
    NSSize _size;
    BOOL _lit[IVE_MAX_CAPS];            /* which caps were lit at the last poll */
    int _downIdx;                       /* the held (not latched) cap the mouse is on, or -1 */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSSize)contentSize { return _size; }

- (void)configure
{
    _caps = intvsession_ecs_layout(&_count, &_dimW, &_dimH);
    if (!_caps || _count < 0) _count = 0;
    if (_count > IVE_MAX_CAPS) _count = IVE_MAX_CAPS;
    _size = NSMakeSize(ceil(_dimW * IVE_UNIT) + 2 * IVE_PAD, ceil(_dimH * IVE_UNIT) + 2 * IVE_PAD);
    [self setNeedsDisplay:YES];
}

- (NSRect)rectOf:(const intvsession_ecs_cap *)cap
{
    return NSMakeRect(IVE_PAD + cap->x * IVE_UNIT, IVE_PAD + cap->y * IVE_UNIT, cap->w * IVE_UNIT,
                      cap->h * IVE_UNIT);
}

/* The cap under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    return intvsession_ecs_hit((float)((pnt.x - IVE_PAD) / IVE_UNIT), (float)((pnt.y - IVE_PAD) / IVE_UNIT));
}

- (void)poll
{
    BOOL dirty = NO;
    for (int idx = 0; idx < _count; idx++) {
        const intvsession_ecs_cap *cap = &_caps[idx];
        const BOOL lit = cap->kind == INTVSESSION_ECS_KEYCAP && cap->key >= 0 &&
                         intvsession_ecs_key_held(_session, (intvsession_ecs_key)cap->key) != 0;
        if (lit != _lit[idx]) { _lit[idx] = lit; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing ---------------------------------------------------------------------- */

/* The label plate: white INTELLIVISION over COMPUTER KEYBOARD on dark. */
- (void)drawLabelPlate:(NSRect)frame text:(const char *)text
{
    NSBezierPath *plate = IntvCtlRoundRect(frame, 0.3 * IVE_UNIT);
    [IntvCtlColor(INTVSESSION_ECS_LABEL_RGB) setFill];
    [plate fill];
    NSString *both = IntvCtlString(text);
    NSArray<NSString *> *lines = [both componentsSeparatedByString:@"|"];
    NSString *top = lines.count > 0 ? lines[0] : @"";
    NSString *bottom = lines.count > 1 ? lines[1] : @"";
    IntvCtlDrawText(top, NSMakeRect(NSMinX(frame) + 0.4 * IVE_UNIT, NSMinY(frame) + 0.1 * IVE_UNIT,
                                    NSWidth(frame) - 0.8 * IVE_UNIT, NSHeight(frame) * 0.5),
                    14, YES, NO, NSTextAlignmentLeft, IntvCtlColor(INTVSESSION_LEGEND_RGB));
    IntvCtlDrawText(bottom, NSMakeRect(NSMinX(frame) + 0.4 * IVE_UNIT, NSMinY(frame) + NSHeight(frame) * 0.58,
                                       4.6 * IVE_UNIT, NSHeight(frame) * 0.36),
                    8, NO, NO, NSTextAlignmentLeft, IntvCtlColorAlpha(INTVSESSION_LEGEND_RGB, 0.85));
}

- (void)drawBodyCap:(const intvsession_ecs_cap *)cap
{
    const NSRect frame = [self rectOf:cap];
    const char *name = cap->label ? cap->label : "";
    if (!strcmp(name, "Case")) {
        NSBezierPath *shell = IntvCtlRoundRect(frame, 1.0 * IVE_UNIT);
        IntvCtlDropShadow(shell);
        IntvCtlGradientFill(shell, IntvCtlMix(INTVSESSION_ECS_CASE_RGB, 0xFFFFFF, 0.3), INTVSESSION_ECS_CASE_RGB);
        [IntvCtlColor(IntvCtlMix(INTVSESSION_ECS_CASE_RGB, IVE_COL_BLACK, 0.3)) setStroke];
        shell.lineWidth = 1.5;
        [shell stroke];
    } else if (!strcmp(name, "Plate")) {
        NSBezierPath *plate = IntvCtlRoundRect(frame, 0.5 * IVE_UNIT);
        [IntvCtlColor(INTVSESSION_ECS_PLATE_RGB) setFill];
        [plate fill];
        [IntvCtlColor(IntvCtlMix(INTVSESSION_ECS_PLATE_RGB, IVE_COL_BLACK, 0.18)) setStroke];
        plate.lineWidth = 1;
        [plate stroke];
    } else if (!strcmp(name, "Label")) {
        [self drawLabelPlate:frame text:cap->shifted];
    } else if (!strcmp(name, "Mattel")) {
        /* the red script across the label plate's lower right */
        IntvCtlDrawText(IntvCtlString(cap->shifted), frame, 11, YES, YES, NSTextAlignmentRight,
                        IntvCtlColor(INTVSESSION_MATTEL_RED_RGB));
    } else if (!strcmp(name, "Vents")) {
        const int ridges = 5;
        const CGFloat pitch = NSHeight(frame) / ridges;
        for (int ridge = 0; ridge < ridges; ridge++) {
            const CGFloat ypos = NSMinY(frame) + (ridge + 0.5) * pitch;
            IntvCtlStrokeSegment(NSMakePoint(NSMinX(frame), ypos), NSMakePoint(NSMaxX(frame), ypos), 2.0,
                                 IntvCtlColor(INTVSESSION_ECS_RIDGE_RGB));
        }
    } else if (!strcmp(name, "Pinstripe")) {
        [IntvCtlColor(INTVSESSION_ECS_PINSTRIPE_RGB) setFill];
        NSRectFill(NSMakeRect(NSMinX(frame), NSMinY(frame), NSWidth(frame), fmax(2.0, NSHeight(frame))));
    } else if (!strcmp(name, "Cable")) {
        [IntvCtlColor(INTVSESSION_CABLE_RGB) setFill];
        [IntvCtlRoundRect(frame, 0.25 * IVE_UNIT) fill];
    }
}

- (void)drawKeyCap:(const intvsession_ecs_cap *)cap lit:(BOOL)lit
{
    const NSRect frame = [self rectOf:cap];
    const BOOL white = (cap->flags & INTVSESSION_ECSCAP_WHITE) != 0;
    NSBezierPath *face = IntvCtlRoundRect(frame, 0.3 * IVE_UNIT);
    if (lit) {
        [IntvCtlColor(INTVSESSION_HELD_RGB) setFill];
        [face fill];
    } else {
        const uint32_t base = white ? INTVSESSION_ECS_RTN_RGB : INTVSESSION_ECS_KEY_RGB;
        IntvCtlGradientFill(face, IntvCtlMix(base, 0xFFFFFF, white ? 0.25 : 0.10), base);
    }
    [IntvCtlColorAlpha(IVE_COL_BLACK, 0.55) setStroke];
    face.lineWidth = 1;
    [face stroke];

    NSColor *ink = lit ? IntvCtlColor(IVE_COL_BLACK)
                       : (white ? IntvCtlColor(INTVSESSION_ECS_RTN_INK_RGB) : IntvCtlColor(INTVSESSION_LEGEND_RGB));
    NSString *label = IntvCtlString(cap->label);
    const BOOL word = label.length > 1;
    if (cap->shifted && cap->shifted[0]) {
        /* the shifted legend small above, the main one below */
        IntvCtlDrawText(IntvCtlString(cap->shifted),
                        NSMakeRect(NSMinX(frame), NSMinY(frame) + 0.05 * NSHeight(frame), NSWidth(frame),
                                   0.42 * NSHeight(frame)),
                        9, YES, NO, NSTextAlignmentCenter, ink);
        IntvCtlDrawText(label,
                        NSMakeRect(NSMinX(frame), NSMinY(frame) + 0.42 * NSHeight(frame), NSWidth(frame),
                                   0.54 * NSHeight(frame)),
                        word ? 9 : 13, YES, NO, NSTextAlignmentCenter, ink);
    } else {
        IntvCtlDrawText(label, NSInsetRect(frame, 2, 1), word ? 10 : 14, YES, NO, NSTextAlignmentCenter, ink);
    }
    if (lit) {
        NSBezierPath *ring = IntvCtlRoundRect(NSInsetRect(frame, -2, -2), 0.4 * IVE_UNIT);
        [IntvCtlColor(INTVSESSION_HELD_RGB) setStroke];
        ring.lineWidth = fmax(2.0, 0.12 * IVE_UNIT);
        [ring stroke];
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = IntvCtlRoundRect(NSMakeRect(0, 0, _size.width, _size.height), 10);
    IntvCtlGradientFill(stage, IVE_COL_STAGE_TOP, IVE_COL_STAGE_BOT);
    [IntvCtlColor(IVE_COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const intvsession_ecs_cap *cap = &_caps[idx];
        if (cap->kind == INTVSESSION_ECS_BODY) [self drawBodyCap:cap];
        else if (cap->kind == INTVSESSION_ECS_KEYCAP) [self drawKeyCap:cap lit:_lit[idx]];
    }
}

/* ---- the mouse -------------------------------------------------------------------- */

- (void)letGo
{
    if (_downIdx >= 0 && _downIdx < _count)
        intvsession_ecs_key_set(_session, (intvsession_ecs_key)_caps[_downIdx].key, 0);
    _downIdx = -1;
    [self poll];
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const int idx = [self hit:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx < 0 || idx >= _count) return;
    const intvsession_ecs_cap *cap = &_caps[idx];
    if (cap->kind != INTVSESSION_ECS_KEYCAP || cap->key < 0) return;
    const intvsession_ecs_key key = (intvsession_ecs_key)cap->key;
    if (cap->flags & INTVSESSION_ECSCAP_LATCH) {
        /* SHIFT and CTL: this click sets the key, the next clears it */
        intvsession_ecs_key_set(_session, key, intvsession_ecs_key_held(_session, key) ? 0 : 1);
    } else {
        _downIdx = idx;
        intvsession_ecs_key_set(_session, key, 1);
    }
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0 || _downIdx >= _count) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (!NSPointInRect(pnt, [self rectOf:&_caps[_downIdx]])) [self letGo];
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx >= 0) [self letGo];
}
@end

@implementation IntvEcsPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static EcsKeyboardWindow *g_intvEcsKeyboard;

@implementation EcsKeyboardWindow {
    intvsession *_session;
    IntvEcsPanel *_panel;
    IntvEcsFlippedView *_content;
    IntvEcsBoardView *_board;
    NSTextField *_status;
    NSButton *_helpButton;
    NSTimer *_timer;
    int _slow;
    BOOL _placed;
}

+ (EcsKeyboardWindow *)instanceForSession:(intvsession *)session
{
    if (!g_intvEcsKeyboard) g_intvEcsKeyboard = [[EcsKeyboardWindow alloc] initWithSession:session];
    return g_intvEcsKeyboard;
}

+ (void)toggleForSession:(intvsession *)session
{
    EcsKeyboardWindow *inst = [self instanceForSession:session];
    if ([inst isShowing]) [inst hide];
    else [inst show];
}

+ (BOOL)isVisible
{
    return g_intvEcsKeyboard && [g_intvEcsKeyboard isShowing];
}

+ (void)refresh
{
    if (g_intvEcsKeyboard) [g_intvEcsKeyboard reload];
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;

    _board = [[IntvEcsBoardView alloc] initWithFrame:NSZeroRect];
    _board.session = _session;
    [_board configure];
    const NSSize bsize = _board.contentSize;
    const CGFloat total = bsize.height + 8 + IVE_STATUS_H + IVE_MARGIN;

    _panel = [[IntvEcsPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, bsize.width, total)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    IntvSetHelpContext(_panel, @"ecskbd");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[IntvEcsFlippedView alloc] initWithFrame:NSMakeRect(0, 0, bsize.width, total)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];
    [_board setFrame:NSMakeRect(0, 0, bsize.width, bsize.height)];
    [_content addSubview:_board];

    _status = [NSTextField wrappingLabelWithString:@""];
    [_status setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_status setTextColor:[NSColor secondaryLabelColor]];
    [_status setMaximumNumberOfLines:2];
    [_content addSubview:_status];
    _helpButton = IntvHelpButton(@"ecskbd");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    [_helpButton setFrameOrigin:NSMakePoint(bsize.width - IVE_MARGIN - _helpButton.frame.size.width,
                                            bsize.height + 10)];
    [_status setFrame:NSMakeRect(IVE_MARGIN, bsize.height + 6, bsize.width - 2 * IVE_MARGIN - 40, IVE_STATUS_H)];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];
    [self reload];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* The title and the status line say which way the main window's keyboard is
 * routed and whether the ECS is there at all. */
- (void)reload
{
    const BOOL ecsMode = intvsession_get_int(_session, "keyboard_mode", 0) != 0;
    const BOOL missing = !intvsession_has_ecs_rom(_session) ||
                         intvsession_get_int(_session, "ecs", INTVSESSION_HW_AUTO) == INTVSESSION_HW_OFF;
    NSString *title = ecsMode ? @"ECS Keyboard (F10) - keyboard mode: ECS" : @"ECS Keyboard (F10) - keyboard mode: controllers";
    if (![[_panel title] isEqualToString:title]) [_panel setTitle:title];
    NSString *note = ecsMode
        ? @"The main window's keyboard types on this keyboard. SHIFT and CTL latch: click again to release."
        : @"The main window's keyboard drives the hand controllers (Settings > Input). Typing here always reaches the ECS. SHIFT and CTL latch.";
    if (missing)
        note = @"ECS is not enabled - turn it on in Settings (applied when it closes), and import ecs.bin if it is missing.";
    if (![_status.stringValue isEqualToString:note]) _status.stringValue = note;
}

/* ---- the poll ------------------------------------------------------------------- */

- (void)tick
{
    if (++_slow >= IVE_SLOW_TICKS) {
        _slow = 0;
        [self reload];
    }
    [_board poll];
}

/* The timer runs only while the panel is on screen and not occluded or hidden
 * with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak EcsKeyboardWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ---------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = IntvKeysymForEvent(evt);
    if (!keysym) return;
    /* the keys the app owns (the menus take them first; this is for when they did not) */
    if (keysym == IVE_KEYSYM_F9 || keysym == IVE_KEYSYM_F10 || keysym == IVE_KEYSYM_F11 || keysym == IVE_KEYSYM_F12) {
        if ([evt isARepeat]) return;
        if (keysym == IVE_KEYSYM_F9) [IntvControllersWindow toggleWithSession:_session];
        else if (keysym == IVE_KEYSYM_F10) [self hide];
        else if (keysym == IVE_KEYSYM_F11) [[NSApp mainWindow] toggleFullScreen:nil];
        else [DebuggerWindow showForSession:_session];
        return;
    }
    if ([evt isARepeat]) return;    /* the matrix tracks a held key by level */
    IntvForwardEcsKeyEvent(_session, evt, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    IntvForwardEcsKeyEvent(_session, evt, 0);
}

- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    if (IntvModifierKeyState(evt, &down)) IntvForwardEcsKeyEvent(_session, evt, down);
}

/* ---- the window --------------------------------------------------------------------- */

- (BOOL)isShowing
{
    return [_panel isVisible];
}

- (void)show
{
    [self reload];
    [_board configure];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
    [_board poll];
}

- (void)hide
{
    [_panel orderOut:nil];
    [_board letGo];
    intvsession_ecs_keys_clear(_session);
    [_timer invalidate];
    _timer = nil;
    [_board poll];
}

/* No cap may stay down in the matrix when the panel stops having the keyboard
 * (a latched SHIFT or CTL included: it is cleared with the rest). */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [_board letGo];
    intvsession_ecs_keys_clear(_session);
    [_board poll];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hide];
}
@end
