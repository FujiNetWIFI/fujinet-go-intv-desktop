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
 * The disc is a small custom NSView subclass offering all 16 positions the
 * hardware has, marking all 16 sectors, and lighting exactly the one under
 * the pointer. Hit testing is intvsession_disc_from_point (core/src/
 * disc_geom.c), shared with the GNOME/KDE/Windows keypad windows, and the
 * drawing follows the same step-by-step recipe they do.
 *
 * That sharing is why DiscView answers YES to -isFlipped: AppKit's default
 * view coordinates put y increasing UPWARD, which used to make this the
 * one file of the four that must NOT negate dy -- a standing invitation to
 * "fix" it into the N/S-inverted bug the other three each had to fix for
 * real. Flipping puts mouse points and -drawRect: in the same y-DOWN frame
 * the shared helper and the other three toolkits use, so this file's
 * arithmetic is now line-for-line theirs.
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

/* Segments the highlighted sector's arc edge is drawn with -- see
 * -drawRect:. */
#define DISC_WEDGE_STEPS 6

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
 * See this file's header for why this view is flipped, and intvsession.h
 * for the position numbering. */

@interface DiscView : NSView
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_pad_side side;
@end

@implementation DiscView {
    int _direction; /* -1 = centered, else 0-15 (0=E, 4=N, 8=W, 12=S, odd
                     * codes the half-steps between), matching
                     * intv_host.h's disc_codes */
    BOOL _held;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
        _direction = -1;
    return self;
}

/* y increases DOWNWARD in this view, like every other frontend's disc --
 * see the file header. */
- (BOOL)isFlipped
{
    return YES;
}

/* Angles below are ordinary compass/math degrees (0 = East, growing
 * counter-clockwise), converted to this view's y-DOWN coordinates at the
 * point of use: cos for x, MINUS sin for y. */
static NSPoint discPoint(double cx, double cy, double radius, double deg)
{
    double a = deg * M_PI / 180.0;
    return NSMakePoint(cx + radius * cos(a), cy - radius * sin(a));
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
    [self setDirection:intvsession_disc_from_point(p.x - r, p.y - r, r)];
}

- (void)mouseDragged:(NSEvent *)event
{
    if (!_held)
        return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    double r = self.bounds.size.width / 2.0;
    [self setDirection:intvsession_disc_from_point(p.x - r, p.y - r, r)];
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
    double hub = r * INTVSESSION_DISC_DEADZONE_FRAC;

    /* No backdrop fill: the corners outside the disc show the window's own
     * background, as they do on GNOME and KDE (this used to paint them
     * black, which is why the macOS keypad had a black square behind each
     * disc and the others did not). */
    NSBezierPath *disc =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - r, cy - r,
                                                           r * 2, r * 2)];
    [[NSColor colorWithCalibratedRed:0.15 green:0.15 blue:0.17 alpha:1.0]
        setFill];
    [disc fill];

    /* The held sector: exactly the 22.5 degrees centred on the direction
     * intvsession_disc_from_point returned, so the lit wedge is always the
     * one bounded by the two spokes the pointer is between. It used to
     * span 67.5 degrees -- three sectors' worth, spilling well past the
     * one actually pressed (the same ~33.75 mistake commit f6eb131 fixed
     * on GNOME/KDE/Windows and missed here).
     *
     * Drawn as a filled polygon sampled along the arc rather than with
     * -appendBezierPathWithArcWithCenter:...: every toolkit in this
     * project has its own answer to which way an arc sweeps -- and in a
     * flipped view AppKit's answer inverts again. A polygon has no such
     * convention. At r ~= 71px a 3.75-degree chord bulges 0.04px inside
     * the true arc. */
    if (_direction >= 0) {
        double centreDeg = _direction * INTVSESSION_DISC_SECTOR_DEG;
        NSBezierPath *wedge = [NSBezierPath bezierPath];
        [wedge moveToPoint:NSMakePoint(cx, cy)];
        for (int i = 0; i <= DISC_WEDGE_STEPS; i++) {
            double a = centreDeg - INTVSESSION_DISC_SECTOR_DEG / 2.0 +
                       INTVSESSION_DISC_SECTOR_DEG * i / DISC_WEDGE_STEPS;
            [wedge lineToPoint:discPoint(cx, cy, r, a)];
        }
        [wedge closePath];
        [[NSColor colorWithCalibratedRed:0.30 green:0.55 blue:0.90 alpha:1.0]
            setFill];
        [wedge fill];
    }

    /* One spoke per sector boundary -- 16 of them, at the half-way angles
     * BETWEEN the 16 positions, so each position gets a visible sector of
     * its own to aim at. */
    [[NSColor colorWithCalibratedRed:0.45 green:0.45 blue:0.50 alpha:1.0]
        setStroke];
    for (int i = 0; i < INTVSESSION_DISC_POSITIONS; i++) {
        double a = i * INTVSESSION_DISC_SECTOR_DEG -
                   INTVSESSION_DISC_SECTOR_DEG / 2.0;
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line setLineWidth:1.5];
        [line moveToPoint:discPoint(cx, cy, hub, a)];
        [line lineToPoint:discPoint(cx, cy, r, a)];
        [line stroke];
    }
    [disc setLineWidth:1.5];
    [disc stroke];

    NSBezierPath *center =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - hub, cy - hub,
                                                           hub * 2, hub * 2)];
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
