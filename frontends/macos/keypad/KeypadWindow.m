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
 * MAP MODE: a bottom row (MapSelectTarget and friends, near the end of
 * this file) lets a click on any of the 15 digit/action buttons above be
 * rebound to a different keyboard key or gamepad button, through
 * core/src/bindings.c's remappable layer -- see intvsession.h's own
 * "remappable bindings" section for the contract. While armed,
 * PadKeyButton's own -mouseDown:/-mouseUp: (which would normally inject a
 * pad key) instead picks the map target, and IntvKeyWindow's keyInterceptor
 * hook (IntvKeyForward.h) intercepts the next keystroke instead of letting
 * it reach the machine. A gamepad button press is polled for on a short
 * NSTimer (gGamepadPollTimer below) since core/src/gamepad_sdl.c's capture
 * result lands on its own background thread, not this one.
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

/* ---- Map mode (see this file's own header) --------------------------------
 * File-static, same reasoning as the singleton g_keypad below: there is
 * never more than one KeypadWindow instance, so plain C globals let
 * PadKeyButton's -mouseDown:/-mouseUp: (which have no reference back to the
 * owning KeypadWindow) reach the shared state directly. */
typedef NS_ENUM(NSInteger, IntvMapState) {
    IntvMapIdle = 0,       /* normal play */
    IntvMapPickTarget,     /* Map armed, waiting for a keypad button click */
    IntvMapWaitInput,      /* target picked, waiting for a key/pad press */
};

static IntvMapState gMapState = IntvMapIdle;
static intvsession_pad_side gMapSide;
static intvsession_key gMapKey;
static intvsession *gMapSession;
static NSButton *gMapBtn;
static NSTextField *gStatusLabel;
static NSTimer *gGamepadPollTimer;
/* [side][key] -- only INTVSESSION_PAD_LEFT/_RIGHT are ever populated, the
 * two panels this window builds. */
static NSButton *gKeyButtons[2][INTVSESSION_KEY_COUNT];

static void MapSelectTarget(intvsession_pad_side side, intvsession_key key);
static void MapResetUi(void);
static void MapCompleteButton(intvsession_pad_button button);
static void MapCompleteKey(uint32_t keysym);

/* ---- digit/action buttons ---------------------------------------------- */

@interface PadKeyButton : NSButton
@property(nonatomic, assign) intvsession *session;
@property(nonatomic, assign) intvsession_pad_side side;
@property(nonatomic, assign) intvsession_key key;
@end

/* Map mode intercepts both -mouseDown:/-mouseUp: (see this file's own
 * header): PickTarget turns a press into "select this button as the map
 * target" instead of an injection, and WaitInput swallows clicks outright
 * -- at that point the window wants a *keyboard or gamepad* press, not
 * another mouse click reinterpreted as one. */
@implementation PadKeyButton
- (void)mouseDown:(NSEvent *)event
{
    (void)event;
    if (gMapState == IntvMapPickTarget) {
        MapSelectTarget(self.side, self.key);
        return;
    }
    if (gMapState == IntvMapWaitInput)
        return;
    intvsession_pad_key(self.session, self.side, self.key, 1);
}
- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    if (gMapState != IntvMapIdle)
        return;
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

/* ---- Map mode helpers (see this file's own header) ------------------------
 * Free functions rather than KeypadWindow methods, matching PadKeyButton's
 * own reach into the file-static state above -- there is only ever one
 * KeypadWindow, so a method dispatch through it would buy nothing. */

/* controlAccentColor tracks the user's own accent-color preference and
 * light/dark appearance automatically -- simplest way to get a "selected"
 * look without hand-picking a color per appearance. */
static void SetHighlighted(NSButton *btn, BOOL on)
{
    if (!btn)
        return;
    btn.bezelColor = on ? [NSColor controlAccentColor] : nil;
}

static void MapSetStatus(NSString *text)
{
    if (gStatusLabel)
        gStatusLabel.stringValue = text ?: @"";
}

/* Back to IntvMapIdle from any state, undoing whatever highlight is showing
 * and disarming gamepad capture -- shared by Map-to-abort, Reset, and the
 * window being hidden mid-sequence. */
static void MapResetUi(void)
{
    if (gMapState == IntvMapWaitInput)
        SetHighlighted(gKeyButtons[gMapSide][gMapKey], NO);
    SetHighlighted(gMapBtn, NO);
    gMapState = IntvMapIdle;
    MapSetStatus(@"");
    if (gMapSession)
        intvsession_gamepad_capture_cancel(gMapSession);
    [gGamepadPollTimer invalidate];
    gGamepadPollTimer = nil;
}

static void MapSelectTarget(intvsession_pad_side side, intvsession_key key)
{
    char desc[128];
    intvsession_binding b = intvsession_binding_get(gMapSession, side, key);

    gMapSide = side;
    gMapKey = key;
    gMapState = IntvMapWaitInput;
    SetHighlighted(gKeyButtons[side][key], YES);
    intvsession_binding_describe(b, desc, sizeof(desc));
    MapSetStatus([NSString
        stringWithFormat:@"Currently mapped to %s. Press target key or "
                         @"gamepad button, or press MAP to abort.",
                         desc]);
    intvsession_gamepad_capture_begin(gMapSession);
    if (!gGamepadPollTimer)
        gGamepadPollTimer = [NSTimer
            scheduledTimerWithTimeInterval:0.066 /* ~15Hz */
                                    repeats:YES
                                      block:^(NSTimer *timer) {
                                          intvsession_pad_button button;
                                          (void)timer;
                                          if (gMapState != IntvMapWaitInput)
                                              return;
                                          if (intvsession_gamepad_capture_poll(
                                                  gMapSession, &button))
                                              MapCompleteButton(button);
                                      }];
}

/* Completes the mapping, keyboard or gamepad half alike -- both land here
 * once bindings.c has already made the change, just with a different verb
 * for the status line. */
static void MapFinish(NSString *boundTo, const char *stolen)
{
    NSString *status = [NSString
        stringWithFormat:@"%s %s mapped to %@",
                         intvsession_pad_side_name(gMapSide),
                         intvsession_pad_key_name(gMapKey), boundTo];
    if (stolen && stolen[0])
        status = [status
            stringByAppendingFormat:@" (taken from %s)", stolen];

    SetHighlighted(gKeyButtons[gMapSide][gMapKey], NO);
    SetHighlighted(gMapBtn, NO);
    gMapState = IntvMapIdle;
    [gGamepadPollTimer invalidate];
    gGamepadPollTimer = nil;
    MapSetStatus(status);
}

static void MapCompleteKey(uint32_t keysym)
{
    char stolen[128];
    char namebuf[64];

    intvsession_binding_set_key(gMapSession, gMapSide, gMapKey, keysym,
                                stolen, sizeof(stolen));
    intvsession_gamepad_capture_cancel(gMapSession);
    intvsession_keysym_name(keysym, namebuf, sizeof(namebuf));
    MapFinish([NSString stringWithUTF8String:namebuf], stolen);
}

static void MapCompleteButton(intvsession_pad_button button)
{
    char stolen[128];

    intvsession_binding_set_button(gMapSession, gMapSide, gMapKey, button,
                                   stolen, sizeof(stolen));
    MapFinish([NSString stringWithFormat:@"Gamepad %s",
                                        intvsession_pad_button_name(button)],
             stolen);
}

@implementation KeypadWindow {
    intvsession *_session;
    IntvKeyWindow *_window;
}

+ (void)toggleForSession:(intvsession *)session
{
    if (!g_keypad)
        g_keypad = [[KeypadWindow alloc] initWithSession:session];
    if (g_keypad->_window.visible) {
        MapResetUi(); /* a Map sequence left armed behind a hidden window
                       * would still be capturing gamepad input on the
                       * next show -- abort it, same as clicking Map again
                       * to cancel. */
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
    gMapSession = session;
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
    gKeyButtons[side][key] = b;
    return b;
}

/* The Map/Reset row beneath both controller panels -- see this file's own
 * header for the state machine MapSelectTarget/MapComplete* and
 * -onMapClicked:/-onResetClicked: below implement together. */
- (NSView *)buildMapRow
{
    NSButton *mapBtn = [NSButton buttonWithTitle:@"Map"
                                          target:self
                                          action:@selector(onMapClicked:)];
    NSButton *resetBtn =
        [NSButton buttonWithTitle:@"Reset Bindings"
                            target:self
                            action:@selector(onResetClicked:)];
    gMapBtn = mapBtn;

    NSStackView *buttonRow =
        [NSStackView stackViewWithViews:@[ mapBtn, resetBtn ]];
    buttonRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttonRow.spacing = 8;

    gStatusLabel = [NSTextField labelWithString:@""];
    gStatusLabel.alignment = NSTextAlignmentCenter;
    gStatusLabel.lineBreakMode = NSLineBreakByWordWrapping;
    gStatusLabel.maximumNumberOfLines = 2;
    [gStatusLabel.widthAnchor constraintEqualToConstant:440].active = YES;

    NSStackView *row =
        [NSStackView stackViewWithViews:@[ buttonRow, gStatusLabel ]];
    row.orientation = NSUserInterfaceLayoutOrientationVertical;
    row.alignment = NSLayoutAttributeCenterX;
    row.spacing = 6;
    row.edgeInsets = NSEdgeInsetsMake(4, 8, 8, 8);
    return row;
}

- (void)onMapClicked:(id)sender
{
    (void)sender;
    if (gMapState != IntvMapIdle) {
        MapResetUi();
        return;
    }
    gMapState = IntvMapPickTarget;
    SetHighlighted(gMapBtn, YES);
    MapSetStatus(@"Press button to map");
}

- (void)onResetClicked:(id)sender
{
    (void)sender;
    MapResetUi();
    if (gMapSession) {
        intvsession_bindings_reset(gMapSession);
        MapSetStatus(@"Bindings reset to default");
    }
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

    NSStackView *panels = [NSStackView stackViewWithViews:@[ left, right ]];
    panels.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    panels.alignment = NSLayoutAttributeTop;
    panels.spacing = 16;

    NSStackView *root =
        [NSStackView stackViewWithViews:@[ panels, [self buildMapRow] ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeCenterX;
    root.spacing = 4;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

    _window = [[IntvKeyWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 480, 480)
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
    [_window setContentSize:NSMakeSize(480, 480)];
    [_window center];

    /* Map mode's keyboard half: a press while waiting for input completes
     * the mapping instead of reaching the machine at all (see this file's
     * own header). Only the press matters -- ignore the matching release
     * the same way a mouse click's own release is swallowed in
     * PadKeyButton's -mouseUp: above, so releasing the just-mapped key
     * doesn't also get forwarded as a live keystroke. PickTarget still
     * consumes the event (returns YES) -- waiting for a button click, not
     * this. */
    _window.keyInterceptor = ^BOOL(NSEvent *event, int down) {
        if (gMapState == IntvMapWaitInput) {
            if (down) {
                uint32_t keysym = IntvKeysymForEvent(event);
                if (keysym)
                    MapCompleteKey(keysym);
            }
            return YES;
        }
        if (gMapState == IntvMapPickTarget)
            return YES;
        return NO;
    };
}

@end
