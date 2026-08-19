/*
 * KeypadWindow (AppKit) -- both hand controllers side by side: a 3x4
 * keypad grid (1-9, Clear/0/Enter), three action buttons (top,
 * lower-left, lower-right), and a 16-position direction disc, each
 * driving intvsession_pad_key/intvsession_pad_disc directly. No
 * precedent in any sibling FujiNet Go Desktop project (this is the one
 * piece written from scratch for every one of the four Intv frontends
 * rather than transposed) -- matches the GNOME/KDE/Windows keypad
 * windows' own layout and scope exactly, ported here last.
 *
 * Digit/action buttons are a plain NSButton subclass overriding
 * mouseDown:/mouseUp: directly (a keypad digit is a real button on real
 * hardware, held for as long as the finger is down -- the default
 * target/action pair alone only fires once, on release, which is the
 * wrong shape for a press/release pair) -- the GNOME/KDE/Windows ports
 * need the analogous plumbing for the same reason, just through
 * GtkGestureClick / QAbstractButton's own pressed()/released() signals /
 * subclassed WM_LBUTTONDOWN/UP instead.
 *
 * The disc is a small custom NSView subclass snapping to 8 positions,
 * matching the fix already made to the GNOME/KDE/Windows ports' own disc
 * widgets. UNLIKE those three toolkits, AppKit's default (non-flipped)
 * view coordinate system already puts y increasing UPWARD -- the same
 * "compass" convention intv_host.h's disc_codes numbering assumes (0=E,
 * 4=N, 8=W, 12=S, standard math angles) -- so, deliberately, there is NO
 * dy negation here the way GNOME/KDE/Windows each need for their own
 * y-down coordinate systems; negating here would reintroduce the exact
 * N/S-inverted bug those three had to fix.
 *
 * FOCUS: like the GNOME/KDE keypad windows, this one forwards keyboard
 * input to the session. AppKit does give each control its own
 * first-responder status on click, but a key event no control consumes
 * still travels up the responder chain to the NSWindow itself -- so the
 * window is an IntvKeyWindow (see IntvKeyForward.h), which overrides
 * -keyDown:/-keyUp:/-flagsChanged: and forwards from there. No
 * session-wide keyboard hook is needed after all.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#import "IntvKeyForward.h"

#include <math.h>

#define DISC_SIZE 150
#define DISC_DEADZONE_FRAC 0.22

/* ---- digit/action buttons ---------------------------------------------- */

@interface PadKeyButton : NSButton
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_pad_side side;
@property(nonatomic, assign) intvsession_key key;
@end

@implementation PadKeyButton
- (void)mouseDown:(NSEvent *)event
{
    (void)event;
    intvsession_pad_key(self.session, self.side, self.key, 1);
}
- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    intvsession_pad_key(self.session, self.side, self.key, 0);
}
@end

/* ---- direction disc ------------------------------------------------------
 * See this file's header for why dy is used as-is here, unlike the GNOME/
 * KDE/Windows ports' own dy-negated versions. */

@interface DiscView : NSView
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_pad_side side;
@end

@implementation DiscView {
    int _direction; /* -1 = centered, else an even 0-14 (0=E, 4=N, 8=W,
                     * 12=S), matching intv_host.h's disc_codes */
    BOOL _held;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
        _direction = -1;
    return self;
}

static int directionFromPoint(double dx, double dy, double radius)
{
    double dist = sqrt(dx * dx + dy * dy);
    double angle_deg;
    int dir;

    if (dist < radius * DISC_DEADZONE_FRAC)
        return -1;

    angle_deg = atan2(dy, dx) * 180.0 / M_PI;
    if (angle_deg < 0)
        angle_deg += 360.0;
    dir = (int)floor(angle_deg / 45.0 + 0.5) % 8;
    if (dir < 0)
        dir += 8;
    return dir * 2;
}

- (void)setDirection:(int)dir
{
    if (dir == _direction)
        return;
    _direction = dir;
    intvsession_pad_disc(self.session, self.side, dir);
    self.needsDisplay = YES;
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    double r = self.bounds.size.width / 2.0;
    _held = YES;
    [self setDirection:directionFromPoint(p.x - r, p.y - r, r)];
}

- (void)mouseDragged:(NSEvent *)event
{
    if (!_held)
        return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    double r = self.bounds.size.width / 2.0;
    [self setDirection:directionFromPoint(p.x - r, p.y - r, r)];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    _held = NO;
    [self setDirection:-1];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect b = self.bounds;
    double cx = b.size.width / 2.0, cy = b.size.height / 2.0;
    double r = MIN(cx, cy) - 4;

    [[NSColor blackColor] setFill];
    NSRectFill(b);

    NSBezierPath *disc =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - r, cy - r,
                                                           r * 2, r * 2)];
    [[NSColor colorWithCalibratedRed:0.15 green:0.15 blue:0.17 alpha:1.0]
        setFill];
    [disc fill];

    if (_direction >= 0) {
        /* Standard math convention (0=East, counter-clockwise positive) --
         * NSBezierPath's own arc convention already matches, so unlike the
         * GNOME/KDE/Windows discs (each drawing in a y-down system), no
         * angle inversion is needed here either. */
        NSBezierPath *wedge = [NSBezierPath bezierPath];
        [wedge moveToPoint:NSMakePoint(cx, cy)];
        [wedge appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy)
                                          radius:r
                                      startAngle:_direction * 22.5 - 33.75
                                        endAngle:_direction * 22.5 + 33.75];
        [wedge closePath];
        [[NSColor colorWithCalibratedRed:0.30 green:0.55 blue:0.90 alpha:1.0]
            setFill];
        [wedge fill];
    }

    [[NSColor colorWithCalibratedRed:0.45 green:0.45 blue:0.50 alpha:1.0]
        setStroke];
    for (int i = 0; i < 8; i++) {
        double a = (i * 45.0 - 22.5) * M_PI / 180.0;
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line moveToPoint:NSMakePoint(cx, cy)];
        [line lineToPoint:NSMakePoint(cx + r * cos(a), cy + r * sin(a))];
        [line stroke];
    }
    [disc stroke];

    double dz = r * DISC_DEADZONE_FRAC;
    NSBezierPath *center =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - dz, cy - dz,
                                                           dz * 2, dz * 2)];
    [[NSColor colorWithCalibratedRed:0.25 green:0.25 blue:0.28 alpha:1.0]
        setFill];
    [center fill];
}
@end

/* ---- top-level window ---------------------------------------------------- */

static KeypadWindow *g_keypad;

static const struct {
    const char *label;
    intvsession_key key;
} kDigits[12] = {
    {"1", INTVSESSION_KEY_1}, {"2", INTVSESSION_KEY_2},
    {"3", INTVSESSION_KEY_3}, {"4", INTVSESSION_KEY_4},
    {"5", INTVSESSION_KEY_5}, {"6", INTVSESSION_KEY_6},
    {"7", INTVSESSION_KEY_7}, {"8", INTVSESSION_KEY_8},
    {"9", INTVSESSION_KEY_9}, {"Clear", INTVSESSION_KEY_CLEAR},
    {"0", INTVSESSION_KEY_0}, {"Enter", INTVSESSION_KEY_ENTER},
};

@implementation KeypadWindow {
    intvsession *_session;
    IntvKeyWindow *_window;
}

+ (void)toggleForSession:(intvsession *)session
{
    if (!g_keypad)
        g_keypad = [[KeypadWindow alloc] initWithSession:session];
    if (g_keypad->_window.visible) {
        [g_keypad->_window orderOut:nil];
    } else {
        [g_keypad->_window makeKeyAndOrderFront:nil];
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

- (PadKeyButton *)keyButtonWithTitle:(NSString *)title
                                side:(intvsession_pad_side)side
                                 key:(intvsession_key)key
{
    PadKeyButton *b = [[PadKeyButton alloc] init];
    b.title = title;
    /* Click-only: without this the button takes first responder on
     * click and then eats Space/Enter as "press me", so those keys
     * would never reach the window's own key forwarding. */
    b.refusesFirstResponder = YES;
    b.bezelStyle = NSBezelStyleRounded;
    b.session = _session;
    b.side = side;
    b.key = key;
    [b.widthAnchor constraintEqualToConstant:48].active = YES;
    [b.heightAnchor constraintEqualToConstant:30].active = YES;
    return b;
}

/* Every row here is centered within the panel (via NSStackView's default
 * centered alignment on the cross axis when embedded in a vertical column
 * with alignment CenterX), matching the GNOME port's own
 * gtk_widget_set_halign(..., GTK_ALIGN_CENTER) on each row -- and the
 * layout fix applied to the Windows port's own build_controller() for the
 * same reason (its digit grid was left-justified against the disc). */
- (NSView *)buildControllerForSide:(intvsession_pad_side)side
                              title:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.alignment = NSTextAlignmentCenter;

    NSMutableArray<NSView *> *digitRows = [NSMutableArray array];
    for (int row = 0; row < 4; row++) {
        NSMutableArray<NSView *> *rowViews = [NSMutableArray array];
        for (int col = 0; col < 3; col++) {
            int i = row * 3 + col;
            [rowViews addObject:[self keyButtonWithTitle:
                                          [NSString stringWithUTF8String:
                                                        kDigits[i].label]
                                                     side:side
                                                      key:kDigits[i].key]];
        }
        NSStackView *rowStack = [NSStackView stackViewWithViews:rowViews];
        rowStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        rowStack.spacing = 4;
        [digitRows addObject:rowStack];
    }
    NSStackView *digitGrid = [NSStackView stackViewWithViews:digitRows];
    digitGrid.orientation = NSUserInterfaceLayoutOrientationVertical;
    digitGrid.spacing = 4;
    digitGrid.alignment = NSLayoutAttributeCenterX;

    PadKeyButton *top = [self keyButtonWithTitle:@"Top"
                                            side:side
                                             key:INTVSESSION_ACTION_TOP];
    PadKeyButton *ll =
        [self keyButtonWithTitle:@"L-Lower"
                            side:side
                             key:INTVSESSION_ACTION_LOWER_LEFT];
    PadKeyButton *rl =
        [self keyButtonWithTitle:@"R-Lower"
                            side:side
                             key:INTVSESSION_ACTION_LOWER_RIGHT];
    NSStackView *actionRow = [NSStackView stackViewWithViews:@[ top, ll, rl ]];
    actionRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    actionRow.spacing = 4;

    DiscView *disc =
        [[DiscView alloc] initWithFrame:NSMakeRect(0, 0, DISC_SIZE,
                                                    DISC_SIZE)];
    disc.session = _session;
    disc.side = side;
    [disc.widthAnchor constraintEqualToConstant:DISC_SIZE].active = YES;
    [disc.heightAnchor constraintEqualToConstant:DISC_SIZE].active = YES;

    NSStackView *col = [NSStackView stackViewWithViews:@[
        label, digitGrid, actionRow, disc
    ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeCenterX;
    col.spacing = 8;
    col.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    return col;
}

- (void)buildWindow
{
    NSView *left = [self buildControllerForSide:INTVSESSION_PAD_LEFT
                                          title:@"Left Controller"];
    NSView *right = [self buildControllerForSide:INTVSESSION_PAD_RIGHT
                                           title:@"Right Controller"];

    NSStackView *root = [NSStackView stackViewWithViews:@[ left, right ]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 16;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

    _window = [[IntvKeyWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 480, 420)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Keypad";
    /* Follows the "keyboard_mode" setting, unlike the ECS keyboard window
     * -- typing here means whatever it would mean in the main window. */
    _window.session = _session;
    _window.releasedWhenClosed = NO;
    _window.contentView = root;
    [_window setContentSize:NSMakeSize(480, 420)];
    [_window center];
}

@end
