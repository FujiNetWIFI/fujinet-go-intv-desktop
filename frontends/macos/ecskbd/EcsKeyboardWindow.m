/*
 * EcsKeyboardWindow (AppKit) -- an on-screen ECS keyboard: 48 buttons
 * covering every key of the ECS's own 7x8 scan-matrix keyboard (see
 * core/jzintv/intv_host.h's intv_ecs_key), each driving
 * intvsession_ecs_key_set directly. Independent of the "keyboard_mode"
 * setting (DisplayView.m's key handling) -- these on-screen buttons work
 * whether or not the physical keyboard is currently routed to the ECS, so
 * opening this window never silently changes that setting.
 *
 * Structured like KeypadWindow.m: ordinary keys are a plain NSButton
 * subclass overriding mouseDown:/mouseUp: directly (a real ECS key is held
 * for as long as it's down, which the default target/action pair alone
 * doesn't give). Shift and Ctrl are NSButtons with setButtonType:
 * NSButtonTypePushOnPushOff instead -- a mouse can't hold two buttons down
 * to chord a Shifted letter the way a hand can, so they latch, and
 * intv_host_ecs_key's OR-in-a-bit chording (core/jzintv/intv_host.c) does
 * the rest.
 *
 * FOCUS: like KeypadWindow, this forwards keyboard input through an
 * IntvKeyWindow -- see that file's own FOCUS note for how. Here `ecsOnly`
 * is set, so typed keys reach the ECS matrix regardless of the
 * "keyboard_mode" setting, exactly as the on-screen buttons above do.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "EcsKeyboardWindow.h"

#import "IntvKeyForward.h"

/* ---- ordinary keys ------------------------------------------------------ */

@interface EcsKeyButton : NSButton
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_ecs_key key;
@end

@implementation EcsKeyButton
- (void)mouseDown:(NSEvent *)event
{
    (void)event;
    intvsession_ecs_key_set(self.session, self.key, 1);
}
- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    intvsession_ecs_key_set(self.session, self.key, 0);
}
@end

/* ---- Shift/Ctrl (latching -- see file header) ---------------------------- */

@interface EcsModButton : NSButton
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_ecs_key key;
@end

@implementation EcsModButton
- (void)modToggled:(id)sender
{
    (void)sender;
    intvsession_ecs_key_set(self.session, self.key, self.state == NSControlStateValueOn);
}
@end

/* ---- top-level window ------------------------------------------------------ */

static EcsKeyboardWindow *g_ecskbd;

typedef struct {
    const char *label;
    intvsession_ecs_key key;
} EcsKeyLabel;

static const EcsKeyLabel kRowDigits[10] = {
    {"1", INTVSESSION_ECS_KEY_1}, {"2", INTVSESSION_ECS_KEY_2},
    {"3", INTVSESSION_ECS_KEY_3}, {"4", INTVSESSION_ECS_KEY_4},
    {"5", INTVSESSION_ECS_KEY_5}, {"6", INTVSESSION_ECS_KEY_6},
    {"7", INTVSESSION_ECS_KEY_7}, {"8", INTVSESSION_ECS_KEY_8},
    {"9", INTVSESSION_ECS_KEY_9}, {"0", INTVSESSION_ECS_KEY_0},
};
static const EcsKeyLabel kRowQwerty[10] = {
    {"Q", INTVSESSION_ECS_KEY_Q}, {"W", INTVSESSION_ECS_KEY_W},
    {"E", INTVSESSION_ECS_KEY_E}, {"R", INTVSESSION_ECS_KEY_R},
    {"T", INTVSESSION_ECS_KEY_T}, {"Y", INTVSESSION_ECS_KEY_Y},
    {"U", INTVSESSION_ECS_KEY_U}, {"I", INTVSESSION_ECS_KEY_I},
    {"O", INTVSESSION_ECS_KEY_O}, {"P", INTVSESSION_ECS_KEY_P},
};
static const EcsKeyLabel kRowAsdf[10] = {
    {"A", INTVSESSION_ECS_KEY_A}, {"S", INTVSESSION_ECS_KEY_S},
    {"D", INTVSESSION_ECS_KEY_D}, {"F", INTVSESSION_ECS_KEY_F},
    {"G", INTVSESSION_ECS_KEY_G}, {"H", INTVSESSION_ECS_KEY_H},
    {"J", INTVSESSION_ECS_KEY_J}, {"K", INTVSESSION_ECS_KEY_K},
    {"L", INTVSESSION_ECS_KEY_L}, {";", INTVSESSION_ECS_KEY_SEMI},
};
static const EcsKeyLabel kRowZxcv[10] = {
    {"Z", INTVSESSION_ECS_KEY_Z}, {"X", INTVSESSION_ECS_KEY_X},
    {"C", INTVSESSION_ECS_KEY_C}, {"V", INTVSESSION_ECS_KEY_V},
    {"B", INTVSESSION_ECS_KEY_B}, {"N", INTVSESSION_ECS_KEY_N},
    {"M", INTVSESSION_ECS_KEY_M}, {",", INTVSESSION_ECS_KEY_COMMA},
    {".", INTVSESSION_ECS_KEY_PERIOD}, {"←", INTVSESSION_ECS_KEY_LEFT},
};

@implementation EcsKeyboardWindow {
    intvsession *_session;
    IntvKeyWindow *_window;
    NSTextField *_notice;
}

+ (void)toggleForSession:(intvsession *)session
{
    if (!g_ecskbd)
        g_ecskbd = [[EcsKeyboardWindow alloc] initWithSession:session];
    if (g_ecskbd->_window.visible) {
        [g_ecskbd->_window orderOut:nil];
        intvsession_ecs_keys_clear(session);
    } else {
        [g_ecskbd updateNotice];
        [g_ecskbd->_window makeKeyAndOrderFront:nil];
    }
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    [self buildWindow];
    return self;
}

- (void)updateNotice
{
    BOOL show = !intvsession_has_ecs_rom(_session) ||
               intvsession_get_int(_session, "ecs", INTVSESSION_HW_AUTO) ==
                   INTVSESSION_HW_OFF;
    _notice.hidden = !show;
}

- (EcsKeyButton *)keyButtonWithTitle:(NSString *)title
                                 key:(intvsession_ecs_key)key
                               width:(CGFloat)width
{
    EcsKeyButton *b = [[EcsKeyButton alloc] init];
    b.title = title;
    /* Click-only: without this the button takes first responder on
     * click and then eats Space/Enter as "press me", so those keys
     * would never reach the window's own key forwarding. */
    b.refusesFirstResponder = YES;
    b.bezelStyle = NSBezelStyleRounded;
    b.session = _session;
    b.key = key;
    [b.widthAnchor constraintEqualToConstant:width].active = YES;
    [b.heightAnchor constraintEqualToConstant:30].active = YES;
    return b;
}

- (EcsModButton *)modButtonWithTitle:(NSString *)title
                                  key:(intvsession_ecs_key)key
{
    EcsModButton *b = [[EcsModButton alloc] init];
    b.title = title;
    /* Click-only: without this the button takes first responder on
     * click and then eats Space/Enter as "press me", so those keys
     * would never reach the window's own key forwarding. */
    b.refusesFirstResponder = YES;
    b.bezelStyle = NSBezelStyleRounded;
    [b setButtonType:NSButtonTypePushOnPushOff];
    b.session = _session;
    b.key = key;
    b.target = b;
    b.action = @selector(modToggled:);
    [b.widthAnchor constraintEqualToConstant:56].active = YES;
    [b.heightAnchor constraintEqualToConstant:30].active = YES;
    return b;
}

- (NSStackView *)rowFromKeys:(const EcsKeyLabel *)keys count:(int)count
{
    NSMutableArray<NSView *> *views = [NSMutableArray array];
    for (int i = 0; i < count; i++)
        [views addObject:[self keyButtonWithTitle:[NSString stringWithUTF8String:keys[i].label]
                                               key:keys[i].key
                                             width:36]];
    NSStackView *row = [NSStackView stackViewWithViews:views];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 4;
    return row;
}

- (void)buildWindow
{
    _notice = [NSTextField labelWithString:
                   @"ECS is not enabled -- see Settings to turn it on."];
    _notice.alignment = NSTextAlignmentCenter;

    NSStackView *row1 = [self rowFromKeys:kRowDigits count:10];
    NSStackView *row2 = [self rowFromKeys:kRowQwerty count:10];
    NSStackView *row3 = [self rowFromKeys:kRowAsdf count:10];
    NSStackView *row4 = [self rowFromKeys:kRowZxcv count:10];

    EcsKeyButton *esc = [self keyButtonWithTitle:@"Esc"
                                             key:INTVSESSION_ECS_KEY_ESC
                                           width:48];
    EcsModButton *ctrl = [self modButtonWithTitle:@"Ctrl"
                                              key:INTVSESSION_ECS_KEY_CTRL];
    EcsModButton *shift = [self modButtonWithTitle:@"Shift"
                                               key:INTVSESSION_ECS_KEY_SHIFT];
    EcsKeyButton *space = [self keyButtonWithTitle:@"Space"
                                               key:INTVSESSION_ECS_KEY_SPACE
                                             width:140];
    EcsKeyButton *up = [self keyButtonWithTitle:@"↑"
                                            key:INTVSESSION_ECS_KEY_UP
                                          width:36];
    EcsKeyButton *down = [self keyButtonWithTitle:@"↓"
                                              key:INTVSESSION_ECS_KEY_DOWN
                                            width:36];
    EcsKeyButton *right = [self keyButtonWithTitle:@"→"
                                               key:INTVSESSION_ECS_KEY_RIGHT
                                             width:36];
    EcsKeyButton *enter = [self keyButtonWithTitle:@"Enter"
                                               key:INTVSESSION_ECS_KEY_ENTER
                                             width:56];
    NSStackView *fnRow = [NSStackView stackViewWithViews:@[
        esc, ctrl, shift, space, up, down, right, enter
    ]];
    fnRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    fnRow.spacing = 4;

    NSStackView *col = [NSStackView stackViewWithViews:@[
        _notice, row1, row2, row3, row4, fnRow
    ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeCenterX;
    col.spacing = 8;
    col.edgeInsets = NSEdgeInsetsMake(12, 16, 16, 16);

    _window = [[IntvKeyWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 620, 260)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"ECS Keyboard";
    _window.session = _session;
    /* This window IS the ECS keyboard, so its keystrokes drive the ECS
     * matrix whether or not "keyboard_mode" is on -- matching its
     * on-screen buttons (see this file's header) and the GNOME and KDE
     * ports' equivalents. */
    _window.ecsOnly = YES;
    _window.releasedWhenClosed = NO;
    _window.contentView = col;
    [_window setContentSize:NSMakeSize(620, 260)];
    [_window center];
    [self updateNotice];
}

@end
