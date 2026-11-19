/*
 * IntvControllersWindow -- the AppKit Controllers panel: the Intellivision II
 * handsets (a warm light grey body, a black keypad of red-outlined keys, the
 * black disc in its cream ring, four grey side tabs and the cable stub; two
 * handsets, four when the ECS is on) over the console strip (the black label
 * band, the red line, the black ON/OFF + RESET block, the grille and the LED),
 * all painted from the session's layout (intvsession_controls_layout) so every
 * frontend shows the same thing. Below it the system buttons, the gamepads
 * with the side each drives, and Map mode for rebinding. The art follows
 * ~/Workspace/fujinet-go-all/controllers-port/DESIGN-intv.md (colours are the
 * INTVSESSION_*_RGB defines, read from photos of the real hardware).
 *
 * Gestures: press a keypad key or a side tab and it is held until it is
 * released or dragged off; press on the disc and drag to steer it (all 16
 * positions, intvsession_pointer_set; it lets go on release); press the
 * console's RESET and let go over it to reset the game. The pointer is its own
 * input source, so the panel lets go of everything the mouse holds when it is
 * hidden or loses key status (intvsession_release_pointer); what the machine
 * sees held from any source (the host keyboard, a gamepad, the mouse) is
 * painted in the held colour, so the panel doubles as an input tester for a new
 * gamepad. A keypad press also grounds disc and button lines on the real
 * hardware, so nothing is lit from the machine's decoded byte, only from the
 * session's source table.
 *
 * The action buttons are crossed by design (the right-hand modifier keys drive
 * the LEFT controller in the default keys); the panel says so under the
 * picture.
 *
 * Keys typed with the panel key go to the machine as in the main window
 * (the "keyboard_mode" setting included); no view here takes the keyboard, so
 * the panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * more pads than the two rows kept for them, or the ECS option changing at a
 * restart, change its size (the top-left corner stays put). Hidden at start,
 * ordered out rather than closed, so its position survives. The 30 Hz poll
 * runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (a
 * key, a side tab, a disc sector, RESET) or choose one from the list (every
 * target, the system actions included); every control other than the pick dims
 * to 40%. Then press a key or a gamepad button; the next one is bound and
 * whatever held it is told so in the hint line. Clear unbinds the pick's key
 * and pad button. Esc backs out of the pick. F9 to F12 are the app's and never
 * bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "IntvCtlDraw.h"
#import "../IntvKeyForward.h"
#import "../debugger/DebuggerWindow.h"
#import "../ecskbd/EcsKeyboardWindow.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define IVC_UNIT          18.0    /* points per layout unit */
#define IVC_MIN_WIN_W     600.0   /* the rows under the picture need this much */
#define IVC_MARGIN        16.0
#define IVC_STAGE_PAD      8.0
#define IVC_TITLE_H       18.0
#define IVC_ROW_H         26.0
#define IVC_EFF_W        150.0
#define IVC_POPUP_W      130.0
#define IVC_BTN_H         28.0
#define IVC_SYS_ROW_H     34.0
#define IVC_MIN_PAD_ROWS    2     /* rows kept for pads, more when more are connected */
#define IVC_MAX_PAD_ROWS    4
#define IVC_SLOW_TICKS      8     /* the 30 Hz tick's 250 ms part */
#define IVC_PILL_H         14.0
#define IVC_MAX_PILLS     192
#define IVC_DIM_ALPHA      0.4    /* controls that are not the Map pick */
#define IVC_DISC_WEDGE_STEPS 8
#define IVC_HELD_FILL_ALPHA  0.35

/* The keys the app owns, as the HID band keysyms (usage 0x42..0x45). */
#define IVC_KEYSYM_F9   (INTVSESSION_KEYSYM_HID_BASE + 0x42u)
#define IVC_KEYSYM_F10  (INTVSESSION_KEYSYM_HID_BASE + 0x43u)
#define IVC_KEYSYM_F11  (INTVSESSION_KEYSYM_HID_BASE + 0x44u)
#define IVC_KEYSYM_F12  (INTVSESSION_KEYSYM_HID_BASE + 0x45u)

/* Map mode states. */
enum { IVC_MAP_IDLE = 0, IVC_MAP_ARMED = 1, IVC_MAP_PICKED = 2 };

/* The stage behind the handsets. */
#define IVC_COL_STAGE_TOP   0x6B6C72
#define IVC_COL_STAGE_BOT   0x55565B
#define IVC_COL_STAGE_EDGE  0x3A3B3F
#define IVC_COL_CAPTION     0xEEEEF2
#define IVC_COL_PILL        0x1A1A1C
#define IVC_COL_BLACK       0x000000

/* ---- helpers --------------------------------------------------------------------- */

static NSString *IntvCtlKeyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) intvsession_keysym_name(keysym, name, (int)sizeof name);
    return IntvCtlString(name);
}

static NSString *IntvCtlTargetName(intvsession_key_mapping target)
{
    char name[96];
    name[0] = '\0';
    intvsession_target_name(target, name, (int)sizeof name);
    return IntvCtlString(name);
}

static BOOL IntvCtlSameTarget(intvsession_key_mapping lhs, intvsession_key_mapping rhs)
{
    if (lhs.kind != rhs.kind) return NO;
    switch (lhs.kind) {
    case INTVSESSION_MAP_KEY:
        return lhs.side == rhs.side && lhs.key == rhs.key;
    case INTVSESSION_MAP_DISC:
        return lhs.side == rhs.side && lhs.direction == rhs.direction;
    case INTVSESSION_MAP_SYSACT:
        return lhs.sysact == rhs.sysact;
    default:
        return NO;
    }
}

/* The index of a target among intvsession_target_from_index's, or -1. */
static int IntvCtlTargetIndex(intvsession_key_mapping target)
{
    const int total = intvsession_target_count();
    for (int idx = 0; idx < total; idx++)
        if (IntvCtlSameTarget(intvsession_target_from_index(idx), target)) return idx;
    return -1;
}

/* Toggled-on look for a push button: accent bezel, white title. */
static void IntvCtlButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = IntvCtlColor(INTVSESSION_ACCENT_RGB);
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : IntvCtlColor(INTVSESSION_ACCENT_INK_RGB),
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A label on a dark pill, filled with the accent when picked. */
static void IntvCtlDrawPill(NSRect frame, NSString *label, BOOL picked)
{
    NSBezierPath *pill = IntvCtlRoundRect(frame, frame.size.height / 2);
    [(picked ? IntvCtlColor(INTVSESSION_ACCENT_RGB) : IntvCtlColorAlpha(IVC_COL_PILL, 0.9)) setFill];
    [pill fill];
    IntvCtlDrawText(label, NSInsetRect(frame, 3, 0), 9, YES, NO, NSTextAlignmentCenter, NSColor.whiteColor);
}

/* The centre angle of a disc position on a y-down screen: 0 = East, counter-
 * clockwise on the picture, so the screen angle is negated. */
static double IntvCtlDiscAngle(int position)
{
    return -(double)position * (360.0 / INTVSESSION_DISC_POSITIONS) * M_PI / 180.0;
}

/* ---- views ------------------------------------------------------------------------ */

/* Laid out from the top, like the other frontends' panels. */
@interface IntvCtlFlippedView : NSView
@end

@implementation IntvCtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@interface IntvControllersPanel : NSPanel
@property (nonatomic, weak) IntvControllersWindow *owner;
@end

/* Everything the views and the class methods call on the window. */
@interface IntvControllersWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(intvsession *)session;
- (void)show;
- (void)hide;
- (void)toggle;
- (BOOL)isVisible;
- (BOOL)isActive;
- (void)refresh;
- (void)mapPick:(intvsession_key_mapping)target;
- (NSString *)legendForTarget:(intvsession_key_mapping)target;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect frame;
    intvsession_key_mapping target;
    char text[64];
} IntvCtlPill;

/* The stage: the handsets and the console on a grey backdrop. Everything is
 * drawn here and hit-tested by the core (the disc's sectors are the core's
 * too). */
@interface IntvCtlStageView : IntvCtlFlippedView
@property (nonatomic) intvsession *session;
@property (nonatomic, weak) IntvControllersWindow *owner;
@property (nonatomic) BOOL mapping;                 /* Map armed: bindings shown, nothing lit */
@property (nonatomic) BOOL hasPick;                 /* a control waits for a key */
@property (nonatomic) intvsession_key_mapping pick; /* ... this one */
@property (nonatomic) BOOL showKeys;                /* the bound host key as a small legend */
@property (nonatomic) CGFloat winWidth;             /* the panel's content width */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Whether the layout is out of date: the ECS option changed at a restart. */
- (BOOL)layoutStale;
/* Reads what the machine sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation IntvCtlStageView {
    const intvsession_control *_ctls;   /* the core's static table */
    int _count;
    float _dimW, _dimH;
    int _ecs;                           /* the layout was read for this ecs_on */
    NSSize _size;

    unsigned _held[4];                  /* what the machine sees held, a bit per intvsession_key */
    int _disc[4];                       /* the held disc position per side, or -1 */

    /* the mouse */
    int _downIdx;                       /* the control pressed, or -1 */
    BOOL _discDrag;
    BOOL _buttonDown;                   /* the console RESET is pressed */
    int _hover;                         /* the pressable part under the pointer (Map), or -1 */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _hover = -1;
    _winWidth = IVC_MIN_WIN_W;
    for (int side = 0; side < 4; side++) _disc[side] = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

/* (Re)reads the layout for the handsets the session has. */
- (void)configure
{
    _ecs = intvsession_ecs_on(_session) ? 1 : 0;
    _ctls = intvsession_controls_layout(_ecs, &_count, &_dimW, &_dimH);
    if (!_ctls || _count < 0) _count = 0;
    _winWidth = fmax(IVC_MIN_WIN_W, ceil(_dimW * IVC_UNIT) + 2 * IVC_STAGE_PAD);
    _size = NSMakeSize(_winWidth, IVC_STAGE_PAD * 2 + ceil(_dimH * IVC_UNIT));
    _hover = -1;
    [self setNeedsDisplay:YES];
}

- (BOOL)layoutStale
{
    return (intvsession_ecs_on(_session) ? 1 : 0) != _ecs;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ----------------------------------------------------------------------- */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((_winWidth - _dimW * IVC_UNIT) / 2), IVC_STAGE_PAD);
}

- (NSRect)rectOf:(const intvsession_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * IVC_UNIT, org.y + ctl->y * IVC_UNIT, ctl->w * IVC_UNIT,
                      ctl->h * IVC_UNIT);
}

- (const intvsession_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable part under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return intvsession_controls_hit(_ecs, (float)((pnt.x - org.x) / IVC_UNIT),
                                    (float)((pnt.y - org.y) / IVC_UNIT));
}

/* The disc position under a view point on a disc control (-1: dead centre). */
- (int)discDirOf:(const intvsession_control *)ctl at:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return intvsession_controls_disc_dir(ctl, (float)((pnt.x - org.x) / IVC_UNIT),
                                         (float)((pnt.y - org.y) / IVC_UNIT));
}

static BOOL IntvCtlIsPressable(int kind)
{
    return kind == INTVSESSION_CTL_KEY || kind == INTVSESSION_CTL_ACTION ||
           kind == INTVSESSION_CTL_DISC || kind == INTVSESSION_CTL_BUTTON;
}

static BOOL IntvCtlSideOK(int side)
{
    return side >= 0 && side < 4;
}

/* Whether the machine sees a key or a tab held (the window is a gamepad tester). */
- (BOOL)heldCtl:(const intvsession_control *)ctl
{
    if (self.mapping) return NO;
    if (ctl->kind == INTVSESSION_CTL_BUTTON) return _buttonDown;
    if (!IntvCtlSideOK(ctl->side) || ctl->target < 0 || ctl->target >= INTVSESSION_KEY_COUNT) return NO;
    return ((_held[ctl->side] >> ctl->target) & 1u) != 0;
}

/* The Map target a control on the picture stands for (a disc: the sector). */
- (intvsession_key_mapping)mappingOf:(const intvsession_control *)ctl dir:(int)dir
{
    if (ctl->kind == INTVSESSION_CTL_DISC) {
        if (dir < 0 || !IntvCtlSideOK(ctl->side)) {
            intvsession_key_mapping none = { .kind = INTVSESSION_MAP_NONE };
            return none;
        }
        return intvsession_target_disc((intvsession_pad_side)ctl->side, dir);
    }
    return intvsession_control_target(ctl);
}

- (BOOL)ctlPicked:(const intvsession_control *)ctl
{
    if (!self.mapping || !self.hasPick) return NO;
    const intvsession_key_mapping pick = self.pick;
    if (ctl->kind == INTVSESSION_CTL_DISC)
        return pick.kind == INTVSESSION_MAP_DISC && (int)pick.side == ctl->side;
    return IntvCtlSameTarget([self mappingOf:ctl dir:-1], pick);
}

/* Whether a part is drawn faint: in Map mode every pressable part but the pick
 * (or, while nothing is picked yet, the one under the pointer). */
- (BOOL)dimmedIdx:(int)idx
{
    if (!self.mapping) return NO;
    const intvsession_control *ctl = &_ctls[idx];
    if (!IntvCtlIsPressable(ctl->kind)) return NO;
    if ([self ctlPicked:ctl]) return NO;
    if (self.hasPick) return YES;
    if (_hover < 0 || _hover >= _count) return YES;
    if (idx == _hover) return NO;
    const intvsession_control *over = &_ctls[_hover];
    /* the two upper tabs are one signal: they light as one */
    if (ctl->kind != INTVSESSION_CTL_DISC && over->kind != INTVSESSION_CTL_DISC &&
        IntvCtlSameTarget([self mappingOf:ctl dir:-1], [self mappingOf:over dir:-1]))
        return NO;
    return YES;
}

- (void)beginDim:(BOOL)dim alpha:(CGFloat)alpha
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, alpha);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* ---- the labels shown over the controls --------------------------------------------- */

- (int)pills:(IntvCtlPill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
    for (int idx = 0; idx < _count; idx++) {
        const intvsession_control *ctl = &_ctls[idx];
        const NSRect frame = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(frame), NSMidY(frame));
        NSRect pillFrame = NSZeroRect;
        intvsession_key_mapping target = { .kind = INTVSESSION_MAP_NONE };
        switch (ctl->kind) {
        case INTVSESSION_CTL_KEY:
            pillFrame = NSMakeRect(ctr.x - (NSWidth(frame) - 4) / 2, ctr.y - IVC_PILL_H / 2,
                                   NSWidth(frame) - 4, IVC_PILL_H);
            target = [self mappingOf:ctl dir:-1];
            break;
        case INTVSESSION_CTL_ACTION:
            pillFrame = NSMakeRect(ctr.x - 17, ctr.y - IVC_PILL_H / 2, 34, IVC_PILL_H);
            target = [self mappingOf:ctl dir:-1];
            break;
        case INTVSESSION_CTL_BUTTON:
            pillFrame = NSMakeRect(ctr.x - 25, ctr.y - IVC_PILL_H / 2, 50, IVC_PILL_H);
            target = [self mappingOf:ctl dir:-1];
            break;
        case INTVSESSION_CTL_DISC: {
            /* the eight compass positions round the pad, and any half-step
             * that has a binding, nearer in */
            const CGFloat rad = NSWidth(frame) / 2;
            for (int pos = 0; pos < INTVSESSION_DISC_POSITIONS; pos++) {
                const intvsession_key_mapping dirTarget = [self mappingOf:ctl dir:pos];
                const intvsession_binding bound = intvsession_target_binding_get(_session, dirTarget);
                const BOOL even = (pos % 2) == 0;
                if (!even && bound.keysym == 0 && bound.button == INTVSESSION_PAD_BTN_NONE) continue;
                if (num >= max) break;
                const double ang = IntvCtlDiscAngle(pos);
                const CGFloat dist = rad * (even ? 0.80 : 0.50);
                IntvCtlPill *pill = &out[num++];
                pill->frame = NSMakeRect(ctr.x + dist * cos(ang) - 15, ctr.y + dist * sin(ang) - IVC_PILL_H / 2,
                                         30, IVC_PILL_H);
                pill->target = dirTarget;
                NSString *name = [self.owner legendForTarget:dirTarget];
                NSString *text = name.length ? name : @"-";
                strncpy(pill->text, text.UTF8String, sizeof pill->text - 1);
                pill->text[sizeof pill->text - 1] = '\0';
            }
            continue;
        }
        default:
            continue;
        }
        if (target.kind == INTVSESSION_MAP_NONE || num >= max) continue;
        IntvCtlPill *pill = &out[num++];
        pill->frame = pillFrame;
        pill->target = target;
        NSString *name = [self.owner legendForTarget:target];
        NSString *text = name.length ? name : @"-";
        strncpy(pill->text, text.UTF8String, sizeof pill->text - 1);
        pill->text[sizeof pill->text - 1] = '\0';
    }
    return num;
}

/* The target Map would pick at a point (view points): YES and *out set, or NO. */
- (BOOL)mapTargetAt:(NSPoint)pnt into:(intvsession_key_mapping *)out
{
    IntvCtlPill pills[IVC_MAX_PILLS];
    const int num = [self pills:pills max:IVC_MAX_PILLS];
    for (int idx = num - 1; idx >= 0; idx--) {
        if (NSPointInRect(pnt, pills[idx].frame)) {
            *out = pills[idx].target;
            return YES;
        }
    }
    const intvsession_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return NO;
    intvsession_key_mapping target = [self mappingOf:ctl
                                                 dir:(ctl->kind == INTVSESSION_CTL_DISC ? [self discDirOf:ctl at:pnt] : -1)];
    if (target.kind == INTVSESSION_MAP_NONE) return NO;
    *out = target;
    return YES;
}

/* ---- polling ------------------------------------------------------------------------ */

- (void)poll
{
    BOOL dirty = NO;
    const int sides = _ecs ? 4 : 2;
    for (int side = 0; side < sides; side++) {
        const intvsession_pad_side pad = (intvsession_pad_side)side;
        const unsigned held = self.mapping ? 0u : intvsession_buttons_held(_session, pad);
        const int disc = self.mapping ? -1 : intvsession_disc_held(_session, pad);
        if (held != _held[side]) { _held[side] = held; dirty = YES; }
        if (disc != _disc[side]) { _disc[side] = disc; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

/* The ring of a held or picked part. */
- (void)ringPath:(NSBezierPath *)path width:(CGFloat)width
{
    [IntvCtlColor(INTVSESSION_HELD_RGB) setStroke];
    path.lineWidth = width;
    [path stroke];
}

- (void)markHeld:(NSBezierPath *)path
{
    [IntvCtlColorAlpha(INTVSESSION_HELD_RGB, IVC_HELD_FILL_ALPHA) setFill];
    [path fill];
    [self ringPath:path width:0.18 * IVC_UNIT];
}

/* -- the handset (DESIGN-intv.md section 1) -- */

- (void)drawHandsetBody:(NSRect)frame
{
    NSBezierPath *body = IntvCtlRoundRect(frame, 0.8 * IVC_UNIT);
    IntvCtlDropShadow(body);
    IntvCtlGradientFill(body, INTVSESSION_BEVEL_RGB, INTVSESSION_HANDSET_RGB);
    [IntvCtlColor(INTVSESSION_SHADE_RGB) setStroke];
    body.lineWidth = 1.5;
    [body stroke];
    NSBezierPath *inner = IntvCtlRoundRect(NSInsetRect(frame, 0.25 * IVC_UNIT, 0.25 * IVC_UNIT), 0.6 * IVC_UNIT);
    inner.lineWidth = 1;
    [IntvCtlColorAlpha(INTVSESSION_BEVEL_RGB, 0.9) setStroke];
    [inner stroke];
}

/* The cable stub leaving the top centre. */
- (void)drawCableStub:(NSRect)frame
{
    NSBezierPath *cable = IntvCtlRoundRect(frame, 0.3 * IVC_UNIT);
    [IntvCtlColor(INTVSESSION_CABLE_RGB) setFill];
    [cable fill];
    for (int coil = 0; coil < 3; coil++) {
        const CGFloat ypos = NSMinY(frame) + (0.15 + 0.22 * coil) * IVC_UNIT;
        IntvCtlStrokeSegment(NSMakePoint(NSMinX(frame) + 1, ypos), NSMakePoint(NSMaxX(frame) - 1, ypos), 0.8,
                             IntvCtlColorAlpha(IVC_COL_STAGE_TOP, 0.35));
    }
}

- (void)drawKeypadPlate:(NSRect)frame
{
    NSBezierPath *plate = IntvCtlRoundRect(frame, 0.4 * IVC_UNIT);
    [IntvCtlColor(INTVSESSION_KEYPAD_RGB) setFill];
    [plate fill];
    [IntvCtlColorAlpha(INTVSESSION_SHADE_RGB, 0.9) setStroke];
    plate.lineWidth = 1;
    [plate stroke];
}

/* The thin edge line of the overlay card, just outside the plate (the card
 * itself is not drawn: it would hide the keys). */
- (void)drawCardEdge:(NSRect)frame
{
    NSBezierPath *edge = IntvCtlRoundRect(frame, 0.5 * IVC_UNIT);
    [IntvCtlColor(INTVSESSION_CARD_EDGE_RGB) setStroke];
    edge.lineWidth = 1;
    [edge stroke];
}

/* The cream ring round the disc. */
- (void)drawDiscRing:(NSRect)frame
{
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:frame];
    IntvCtlGradientFill(ring, IntvCtlMix(INTVSESSION_RING_RGB, 0xFFFFFF, 0.25), INTVSESSION_RING_RGB);
    [IntvCtlColor(INTVSESSION_SHADE_RGB) setStroke];
    ring.lineWidth = 1.25;
    [ring stroke];
}

/* One keypad key: black face, red outline, white legend; held, the face turns
 * to the held colour and the legend to black. */
- (void)drawKeypadKey:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    const BOOL held = [self heldCtl:ctl];
    const CGFloat outline = fmax(1.5, 0.14 * IVC_UNIT);
    NSBezierPath *face = IntvCtlRoundRect(NSInsetRect(frame, outline / 2, outline / 2), 0.4 * IVC_UNIT);
    [(held ? IntvCtlColor(INTVSESSION_HELD_RGB) : IntvCtlColor(INTVSESSION_KEYFACE_RGB)) setFill];
    [face fill];
    [IntvCtlColor(INTVSESSION_KEYRED_RGB) setStroke];
    face.lineWidth = outline;
    [face stroke];
    NSString *label = IntvCtlString(ctl->label);
    const BOOL word = label.length > 1;
    IntvCtlDrawText(label, NSInsetRect(frame, 2, 1), word ? 10 : 15, !word, NO, NSTextAlignmentCenter,
                    held ? IntvCtlColor(IVC_COL_BLACK) : IntvCtlColor(INTVSESSION_LEGEND_RGB));
    if ([self ctlPicked:ctl])
        [self ringPath:IntvCtlRoundRect(NSInsetRect(frame, -3, -3), 0.5 * IVC_UNIT) width:2.5];
}

/* A side tab: slightly darker grey than the body, its letter faint; held, a
 * ring and a fill of the held colour. */
- (void)drawSideTab:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    const BOOL held = [self heldCtl:ctl];
    NSBezierPath *tab = IntvCtlRoundRect(frame, 0.3 * IVC_UNIT);
    IntvCtlGradientFill(tab, IntvCtlMix(INTVSESSION_TAB_RGB, 0xFFFFFF, 0.2), INTVSESSION_TAB_RGB);
    [IntvCtlColor(INTVSESSION_TAB_EDGE_RGB) setStroke];
    tab.lineWidth = 1;
    [tab stroke];
    IntvCtlDrawText(IntvCtlString(ctl->label), frame, 9, YES, NO, NSTextAlignmentCenter,
                    IntvCtlColor(INTVSESSION_TAB_LEGEND_RGB));
    if (held) [self markHeld:IntvCtlRoundRect(NSInsetRect(frame, -2, -2), 0.4 * IVC_UNIT)];
    if ([self ctlPicked:ctl])
        [self ringPath:IntvCtlRoundRect(NSInsetRect(frame, -4, -4), 0.5 * IVC_UNIT) width:2.5];
}

/* A wedge of the disc between two radii (fractions of its radius) round a
 * position. */
- (NSBezierPath *)discWedge:(NSPoint)ctr radius:(CGFloat)rad position:(int)pos from:(double)inner
{
    NSBezierPath *wedge = [NSBezierPath bezierPath];
    const double mid = IntvCtlDiscAngle(pos);
    const double half = (360.0 / INTVSESSION_DISC_POSITIONS) / 2.0 * M_PI / 180.0;
    for (int step = 0; step <= IVC_DISC_WEDGE_STEPS; step++) {
        const double ang = mid - half + 2 * half * step / IVC_DISC_WEDGE_STEPS;
        const NSPoint pnt = NSMakePoint(ctr.x + rad * cos(ang), ctr.y + rad * sin(ang));
        if (step == 0) [wedge moveToPoint:pnt];
        else [wedge lineToPoint:pnt];
    }
    for (int step = IVC_DISC_WEDGE_STEPS; step >= 0; step--) {
        const double ang = mid - half + 2 * half * step / IVC_DISC_WEDGE_STEPS;
        [wedge lineToPoint:NSMakePoint(ctr.x + inner * rad * cos(ang), ctr.y + inner * rad * sin(ang))];
    }
    [wedge closePath];
    return wedge;
}

/* The disc: the black pad with its lathe rings, its 16 sector divisions, and
 * the held sector as a wedge from the dead centre (22% of the radius) out. */
- (void)drawDisc:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(frame), NSMidY(frame));
    const CGFloat rad = NSWidth(frame) / 2;
    const int side = ctl->side;
    const int held = (!self.mapping && IntvCtlSideOK(side)) ? _disc[side] : -1;

    IntvCtlFillOval(frame, IntvCtlColor(INTVSESSION_DISC_RGB));
    const double rings[3] = { 0.40, 0.65, 0.88 };
    for (int idx = 0; idx < 3; idx++) {
        NSBezierPath *lathe = [NSBezierPath bezierPathWithOvalInRect:IntvCtlCircleRect(ctr, rad * rings[idx])];
        lathe.lineWidth = 1;
        [IntvCtlColor(INTVSESSION_DISC_RING_RGB) setStroke];
        [lathe stroke];
    }
    /* the sector divisions, faint, from the dead centre out */
    for (int pos = 0; pos < INTVSESSION_DISC_POSITIONS; pos++) {
        const double ang = IntvCtlDiscAngle(pos) - (360.0 / INTVSESSION_DISC_POSITIONS) / 2.0 * M_PI / 180.0;
        const CGFloat innerRad = rad * INTVSESSION_DISC_DEADZONE_FRAC;
        IntvCtlStrokeSegment(NSMakePoint(ctr.x + innerRad * cos(ang), ctr.y + innerRad * sin(ang)),
                             NSMakePoint(ctr.x + rad * cos(ang), ctr.y + rad * sin(ang)), 0.75,
                             IntvCtlColorAlpha(INTVSESSION_DISC_RING_RGB, 0.9));
    }
    NSBezierPath *dead = [NSBezierPath bezierPathWithOvalInRect:IntvCtlCircleRect(ctr, rad * INTVSESSION_DISC_DEADZONE_FRAC)];
    dead.lineWidth = 1;
    [IntvCtlColor(INTVSESSION_DISC_RING_RGB) setStroke];
    [dead stroke];

    if (held >= 0 && held < INTVSESSION_DISC_POSITIONS) {
        NSBezierPath *wedge = [self discWedge:ctr radius:rad position:held from:INTVSESSION_DISC_DEADZONE_FRAC];
        [IntvCtlColorAlpha(INTVSESSION_HELD_RGB, 0.8) setFill];
        [wedge fill];
        [self ringPath:wedge width:1.5];
    }
    if ([self ctlPicked:ctl]) {
        [self ringPath:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(frame, -5, -5)] width:2.5];
        const intvsession_key_mapping pick = self.pick;
        if (pick.kind == INTVSESSION_MAP_DISC && pick.direction >= 0 && pick.direction < INTVSESSION_DISC_POSITIONS) {
            NSBezierPath *wedge = [self discWedge:ctr radius:rad position:pick.direction from:INTVSESSION_DISC_DEADZONE_FRAC];
            [self ringPath:wedge width:2.5];
        }
    }
}

/* -- the console strip (DESIGN-intv.md section 2) -- */

- (void)drawConsoleBar:(NSRect)frame
{
    NSBezierPath *bar = IntvCtlRoundRect(frame, 0.6 * IVC_UNIT);
    IntvCtlDropShadow(bar);
    IntvCtlGradientFill(bar, IntvCtlMix(INTVSESSION_CONSOLE_RGB, 0xFFFFFF, 0.3), INTVSESSION_CONSOLE_RGB);
    [IntvCtlColor(INTVSESSION_SHADE_RGB) setStroke];
    bar.lineWidth = 1.5;
    [bar stroke];
}

- (void)drawLabelBand:(NSRect)frame
{
    NSBezierPath *band = IntvCtlRoundRect(frame, 0.25 * IVC_UNIT);
    [IntvCtlColor(INTVSESSION_BAND_RGB) setFill];
    [band fill];
}

- (void)drawRedLine:(NSRect)frame
{
    [IntvCtlColor(INTVSESSION_MATTEL_RED_RGB) setFill];
    NSRectFill(NSMakeRect(NSMinX(frame), NSMinY(frame), NSWidth(frame), fmax(1.5, NSHeight(frame))));
}

/* The speaker grille: horizontal slats. */
- (void)drawGrille:(NSRect)frame
{
    const int slats = 6;
    const CGFloat pitch = NSHeight(frame) / slats;
    for (int slat = 0; slat < slats; slat++) {
        const NSRect slot = NSMakeRect(NSMinX(frame), NSMinY(frame) + slat * pitch + pitch * 0.2, NSWidth(frame),
                                       pitch * 0.55);
        [IntvCtlColor(INTVSESSION_GRILLE_RGB) setFill];
        [IntvCtlRoundRect(slot, slot.size.height / 2) fill];
    }
}

- (void)drawBlock:(NSRect)frame
{
    NSBezierPath *block = IntvCtlRoundRect(frame, 0.25 * IVC_UNIT);
    [IntvCtlColor(INTVSESSION_BLOCK_RGB) setFill];
    [block fill];
    [IntvCtlColorAlpha(IVC_COL_BLACK, 0.5) setStroke];
    block.lineWidth = 1;
    [block stroke];
}

/* The ON/OFF half of the block: the machine is on, so it is drawn dimmed and is
 * not a target. */
- (void)drawOnOff:(NSRect)frame
{
    IntvCtlDrawText(@"ON/OFF", NSInsetRect(frame, 2, 1), 9, YES, NO, NSTextAlignmentCenter,
                    IntvCtlColorAlpha(INTVSESSION_BLOCK_LEGEND_RGB, 0.55));
}

- (void)drawLed:(NSRect)frame
{
    IntvCtlFillOval(frame, IntvCtlColor(INTVSESSION_LED_RGB));
    IntvCtlFillOval(NSMakeRect(NSMinX(frame) + NSWidth(frame) * 0.2, NSMinY(frame) + NSHeight(frame) * 0.15,
                               NSWidth(frame) * 0.35, NSHeight(frame) * 0.3),
                    IntvCtlColorAlpha(0xFFFFFF, 0.3));
}

/* The RESET half of the block: Reset Game. */
- (void)drawResetButton:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    const BOOL held = [self heldCtl:ctl];
    NSBezierPath *face = IntvCtlRoundRect(frame, 0.3 * IVC_UNIT);
    [(held ? IntvCtlColorAlpha(INTVSESSION_HELD_RGB, 0.85)
           : IntvCtlColor(IntvCtlMix(INTVSESSION_BLOCK_RGB, 0xFFFFFF, 0.08))) setFill];
    [face fill];
    [IntvCtlColorAlpha(IVC_COL_BLACK, 0.45) setStroke];
    face.lineWidth = 1;
    [face stroke];
    IntvCtlDrawText(IntvCtlString(ctl->label), NSInsetRect(frame, 2, 1), 9, YES, NO, NSTextAlignmentCenter,
                    held ? IntvCtlColor(IVC_COL_BLACK) : IntvCtlColor(INTVSESSION_BLOCK_LEGEND_RGB));
    if ([self ctlPicked:ctl])
        [self ringPath:IntvCtlRoundRect(NSInsetRect(frame, -3, -3), 0.4 * IVC_UNIT) width:2.5];
}

- (void)drawBody:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    const char *name = ctl->label ? ctl->label : "";
    if (!strcmp(name, "Cable")) [self drawCableStub:frame];
    else if (!strcmp(name, "Handset")) [self drawHandsetBody:frame];
    else if (!strcmp(name, "Keypad")) [self drawKeypadPlate:frame];
    else if (!strcmp(name, "Card")) [self drawCardEdge:frame];
    else if (!strcmp(name, "Ring")) [self drawDiscRing:frame];
    else if (!strcmp(name, "Console")) [self drawConsoleBar:frame];
    else if (!strcmp(name, "Band")) [self drawLabelBand:frame];
    else if (!strcmp(name, "RedLine")) [self drawRedLine:frame];
    else if (!strcmp(name, "Grille")) [self drawGrille:frame];
    else if (!strcmp(name, "Block")) [self drawBlock:frame];
    else if (!strcmp(name, "OnOff")) [self drawOnOff:frame];
    else if (!strcmp(name, "Led")) [self drawLed:frame];
}

- (void)drawCaption:(const intvsession_control *)ctl
{
    const NSRect frame = [self rectOf:ctl];
    if (ctl->side < 0) {
        /* the wordmarks printed on the band: white at the left, Mattel's red at the right */
        if (ctl->label && !strcmp(ctl->label, "MATTEL ELECTRONICS"))
            IntvCtlDrawText(IntvCtlString(ctl->label), frame, 11, YES, YES, NSTextAlignmentRight,
                            IntvCtlColor(INTVSESSION_MATTEL_RED_RGB));
        else
            IntvCtlDrawText(IntvCtlString(ctl->label), frame, 12, YES, YES, NSTextAlignmentLeft,
                            IntvCtlColor(INTVSESSION_LEGEND_RGB));
        return;
    }
    IntvCtlDrawText(IntvCtlString(ctl->label), NSInsetRect(frame, 0, -3), 12, YES, NO, NSTextAlignmentCenter,
                    IntvCtlColorAlpha(IVC_COL_CAPTION, 0.9));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = IntvCtlRoundRect(NSMakeRect(0, 0, _size.width, _size.height), 10);
    IntvCtlGradientFill(stage, IVC_COL_STAGE_TOP, IVC_COL_STAGE_BOT);
    [IntvCtlColor(IVC_COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const intvsession_control *ctl = &_ctls[idx];
        const BOOL dim = [self dimmedIdx:idx];
        [self beginDim:dim alpha:IVC_DIM_ALPHA];
        switch (ctl->kind) {
        case INTVSESSION_CTL_CAPTION:
            [self drawCaption:ctl];
            break;
        case INTVSESSION_CTL_BODY:
            [self drawBody:ctl];
            break;
        case INTVSESSION_CTL_KEY:
            [self drawKeypadKey:ctl];
            break;
        case INTVSESSION_CTL_ACTION:
            [self drawSideTab:ctl];
            break;
        case INTVSESSION_CTL_DISC:
            [self drawDisc:ctl];
            break;
        case INTVSESSION_CTL_BUTTON:
            [self drawResetButton:ctl];
            break;
        default:
            break;
        }
        [self endDim:dim];
    }

    /* the Map / Show bindings labels, over everything */
    IntvCtlPill pills[IVC_MAX_PILLS];
    const int num = [self pills:pills max:IVC_MAX_PILLS];
    for (int idx = 0; idx < num; idx++) {
        const BOOL picked = self.hasPick && self.mapping && IntvCtlSameTarget(pills[idx].target, self.pick);
        IntvCtlDrawPill(pills[idx].frame, IntvCtlString(pills[idx].text), picked);
    }
}

/* ---- the mouse ------------------------------------------------------------------------ */

- (void)letGo
{
    const intvsession_control *ctl = [self ctlAt:_downIdx];
    if (ctl && IntvCtlSideOK(ctl->side) &&
        (ctl->kind == INTVSESSION_CTL_KEY || ctl->kind == INTVSESSION_CTL_ACTION || ctl->kind == INTVSESSION_CTL_DISC))
        intvsession_pointer_set(_session, (intvsession_pad_side)ctl->side, -1, -1);
    _downIdx = -1;
    _discDrag = NO;
    _buttonDown = NO;
    _hover = -1;
    [self setNeedsDisplay:YES];
}

/* The disc follows the pointer: the core turns the offset from its centre into
 * one of the 16 positions (none in the dead centre, which lets go). */
- (void)dragDiscAt:(NSPoint)pnt
{
    const intvsession_control *ctl = [self ctlAt:_downIdx];
    if (!ctl || !IntvCtlSideOK(ctl->side)) return;
    intvsession_pointer_set(_session, (intvsession_pad_side)ctl->side, -1, [self discDirOf:ctl at:pnt]);
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a key, a tab, a disc sector or RESET is picked */
        intvsession_key_mapping target;
        if ([self mapTargetAt:pnt into:&target]) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const intvsession_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case INTVSESSION_CTL_KEY:
    case INTVSESSION_CTL_ACTION:
        if (!IntvCtlSideOK(ctl->side) || ctl->target < 0) break;
        _downIdx = idx;
        intvsession_pointer_set(_session, (intvsession_pad_side)ctl->side, ctl->target, -1);
        break;
    case INTVSESSION_CTL_DISC:
        _downIdx = idx;
        _discDrag = YES;
        [self dragDiscAt:pnt];
        break;
    case INTVSESSION_CTL_BUTTON:
        /* the system action fires when the button is let go over it */
        _downIdx = idx;
        _buttonDown = YES;
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_discDrag) {
        [self dragDiscAt:pnt];
        [self poll];
        return;
    }
    /* a key, a tab or RESET: dragging off lets go (RESET then does nothing) */
    const intvsession_control *ctl = [self ctlAt:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    const intvsession_control *ctl = [self ctlAt:_downIdx];
    if (ctl && ctl->kind == INTVSESSION_CTL_BUTTON && ctl->target >= 0 && ctl->target < INTVSESSION_SYSACT_COUNT)
        /* queued for the app's own drain, which also reports a failure */
        intvsession_sysaction_post(_session, (intvsession_sysaction)ctl->target);
    [self letGo];
    [self poll];
}

/* In Map mode the part under the pointer stays bright until one is picked. */
- (void)mouseMoved:(NSEvent *)evt
{
    if (!self.mapping || self.hasPick) return;
    const int idx = [self hit:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx != _hover) {
        _hover = idx;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const intvsession_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return nil;
    int dir = -1;
    if (ctl->kind == INTVSESSION_CTL_DISC) {
        dir = [self discDirOf:ctl at:pnt];
        if (dir < 0) return [NSString stringWithFormat:@"%s disc: drag to steer", intvsession_pad_side_name((intvsession_pad_side)ctl->side)];
    }
    const intvsession_key_mapping target = [self mappingOf:ctl dir:dir];
    if (target.kind == INTVSESSION_MAP_NONE) return nil;
    const intvsession_binding bind = intvsession_target_binding_get(_session, target);
    NSString *key = bind.keysym ? IntvCtlKeyName(bind.keysym) : @"none";
    NSString *padText = bind.button != INTVSESSION_PAD_BTN_NONE
        ? IntvCtlString(intvsession_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", IntvCtlTargetName(target), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation IntvControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape is a key of the machine's (or cancels a Map pick), not "close the
 * panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel --------------------------------------------------------------------- */

static IntvControllersWindow *g_intvControllersSingleton;

@implementation IntvControllersWindow {
    intvsession *_session;
    IntvControllersPanel *_panel;
    IntvCtlFlippedView *_content;
    IntvCtlStageView *_stage;
    NSButton *_resetConfigButton;
    NSButton *_stopButton;
    NSTextField *_crossedNote;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[IVC_MAX_PAD_ROWS];
    NSTextField *_padEff[IVC_MAX_PAD_ROWS];     /* each pad's "drives Left" */
    NSPopUpButton *_padAssign[IVC_MAX_PAD_ROWS];
    int _padRows;                               /* the rows showing a pad */
    int _reservedRows;                          /* the rows the panel has room for */
    int _assignEcs;                             /* the assign popups were filled for this ecs_on */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSPopUpButton *_mapList;                    /* every target, the system actions too */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _winW;
    uint32_t _swallowKey;                       /* the key Map took: its repeats and release too */
    int _mapState;                              /* IVC_MAP_* */
    intvsession_key_mapping _pick;              /* the target waiting for a key, in IVC_MAP_PICKED */
    BOOL _placed;
}

+ (IntvControllersWindow *)instanceForSession:(intvsession *)session
{
    if (!g_intvControllersSingleton) g_intvControllersSingleton = [[IntvControllersWindow alloc] initWithSession:session];
    return g_intvControllersSingleton;
}

+ (void)toggleWithSession:(intvsession *)session
{
    [[self instanceForSession:session] toggle];
}

+ (void)showWithSession:(intvsession *)session
{
    [[self instanceForSession:session] show];
}

+ (BOOL)isVisible
{
    return g_intvControllersSingleton && [g_intvControllersSingleton isVisible];
}

+ (BOOL)isActive
{
    return g_intvControllersSingleton && [g_intvControllersSingleton isActive];
}

+ (void)refresh
{
    if (g_intvControllersSingleton) [g_intvControllersSingleton refresh];
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = IVC_MAP_IDLE;
    _pick.kind = INTVSESSION_MAP_NONE;
    _padGeneration = (unsigned)-1;
    _reservedRows = IVC_MIN_PAD_ROWS;
    _assignEcs = -1;
    _winW = IVC_MIN_WIN_W;

    _panel = [[IntvControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, IVC_MIN_WIN_W, 760)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers (F9)"];
    IntvSetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[IntvCtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, IVC_MIN_WIN_W, 760)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[IntvCtlStageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.showKeys = intvsession_get_int(_session, "controllers_show_bindings", 0) > 0;
    [_content addSubview:_stage];

    /* The two system actions that are not on the console. */
    _resetConfigButton = [NSButton buttonWithTitle:@"Reset to CONFIG" target:self action:@selector(resetToConfig:)];
    [_resetConfigButton setRefusesFirstResponder:YES];
    [_content addSubview:_resetConfigButton];
    _stopButton = [NSButton buttonWithTitle:@"Stop in debugger" target:self action:@selector(stopInDebugger:)];
    [_stopButton setRefusesFirstResponder:YES];
    [_content addSubview:_stopButton];
    _crossedNote = [NSTextField wrappingLabelWithString:
        @"The action buttons are crossed by design: the right-hand modifier keys drive the LEFT controller."];
    [_crossedNote setFont:[NSFont systemFontOfSize:10]];
    [_crossedNote setTextColor:[NSColor secondaryLabelColor]];
    [_crossedNote setMaximumNumberOfLines:3];
    [_content addSubview:_crossedNote];

    /* Gamepads: one row per pad, its name, the side it drives and the one it is
     * set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < IVC_MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    /* The Map list: every target by name, the system actions (which have no part
     * on the picture but RESET) too. Items go in by the menu, one per target. */
    _mapList = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    {
        NSMenuItem *none = [[_mapList menu] addItemWithTitle:@"Choose a control..." action:nil keyEquivalent:@""];
        none.tag = -1;
        const int total = intvsession_target_count();
        for (int idx = 0; idx < total; idx++) {
            NSMenuItem *item = [[_mapList menu] addItemWithTitle:IntvCtlTargetName(intvsession_target_from_index(idx))
                                                          action:nil
                                                   keyEquivalent:@""];
            item.tag = idx;
        }
    }
    _mapList.target = self;
    _mapList.action = @selector(mapListChanged:);
    [_mapList setRefusesFirstResponder:YES];
    [_content addSubview:_mapList];
    _helpButton = IntvHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [self fillAssignPopups];
    [self setMapMode:IVC_MAP_IDLE];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout --------------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    _winW = ssize.width;
    const CGFloat inner = _winW - 2 * IVC_MARGIN;
    const CGFloat sysY = ssize.height + 8;
    const CGFloat padsY = sysY + IVC_SYS_ROW_H + 6;
    const CGFloat rowsY = padsY + IVC_TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * IVC_ROW_H + 6;
    const CGFloat hintY = mapY + IVC_BTN_H + 8;
    const CGFloat total = hintY + 34 + IVC_MARGIN;

    [self fitPanel:NSMakeSize(_winW, total)];
    [_content setFrameSize:NSMakeSize(_winW, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    [_resetConfigButton setFrame:NSMakeRect(IVC_MARGIN, sysY + 3, 124, IVC_BTN_H)];
    [_stopButton setFrame:NSMakeRect(IVC_MARGIN + 130, sysY + 3, 124, IVC_BTN_H)];
    [_crossedNote setFrame:NSMakeRect(IVC_MARGIN + 266, sysY, inner - 266, IVC_SYS_ROW_H)];
    [_padsTitle setFrame:NSMakeRect(IVC_MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(IVC_MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < IVC_MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * IVC_ROW_H;
        [_padName[row] setFrame:NSMakeRect(IVC_MARGIN, rowY + 4, inner - IVC_POPUP_W - IVC_EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(IVC_MARGIN + inner - IVC_POPUP_W - IVC_EFF_W - 6, rowY + 4, IVC_EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(IVC_MARGIN + inner - IVC_POPUP_W, rowY - 1, IVC_POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(IVC_MARGIN, mapY, 56, IVC_BTN_H)];
    [_clearButton setFrame:NSMakeRect(IVC_MARGIN + 62, mapY, 56, IVC_BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(IVC_MARGIN + 124, mapY, 72, IVC_BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(IVC_MARGIN + 206, mapY + 3, 122, 22)];
    [_mapList setFrame:NSMakeRect(IVC_MARGIN + 336, mapY + 1, 200, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(_winW - IVC_MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(IVC_MARGIN, hintY, inner, 34)];
    [_stage setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel (the pointer source only). */
- (void)letGo
{
    [_stage letGo];
    intvsession_release_pointer(_session);
}

/* The ECS option changed at a restart: the picture has other handsets, and the
 * pads have other sides to drive. */
- (void)syncLayout
{
    if (![_stage layoutStale]) return;
    [self letGo];
    [_stage configure];
    [self fillAssignPopups];
    [self refreshPads];
    [self relayout];
    [_stage poll];
}

/* ---- the system buttons -------------------------------------------------------------- */

/* Both go to the app's own queue, which fires them and reports a failure. */
- (void)resetToConfig:(id)sender
{
    (void)sender;
    intvsession_sysaction_post(_session, INTVSESSION_SYSACT_RESET_CONFIG);
}

- (void)stopInDebugger:(id)sender
{
    (void)sender;
    intvsession_sysaction_post(_session, INTVSESSION_SYSACT_DEBUG_STOP);
}

/* ---- Map mode ------------------------------------------------------------------------- */

- (NSString *)legendForTarget:(intvsession_key_mapping)target
{
    return IntvCtlKeyName(intvsession_target_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(intvsession_key_mapping)target
{
    const intvsession_binding bind = intvsession_target_binding_get(_session, target);
    NSString *key = bind.keysym ? IntvCtlKeyName(bind.keysym) : @"no key";
    if (bind.button != INTVSESSION_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, IntvCtlString(intvsession_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != IVC_MAP_IDLE;
    _stage.hasPick = _mapState == IVC_MAP_PICKED;
    _stage.pick = _pick;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapMode:(int)state
{
    _mapState = state;
    if (state != IVC_MAP_PICKED) _pick.kind = INTVSESSION_MAP_NONE;
    if (state == IVC_MAP_PICKED) {
        intvsession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             IntvCtlTargetName(_pick), [self bindingText:_pick]];
    } else {
        intvsession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == IVC_MAP_ARMED
            ? @"Click a key, a side tab, a part of the disc or RESET to remap it, or choose one from the list."
            : @"Hold a key or a tab, drag the disc, or press RESET. Keys and gamepads work here too.";
    }
    if (state != IVC_MAP_IDLE) {
        [self letGo];
        intvsession_release_all(_session);      /* Map's key-ups must not strand a held input */
        intvsession_ecs_keys_clear(_session);
    }
    [_mapButton setTitle:(state == IVC_MAP_IDLE ? @"Map" : @"Done")];
    IntvCtlButtonAccent(_mapButton, state != IVC_MAP_IDLE);
    _clearButton.enabled = state == IVC_MAP_PICKED;
    _mapList.enabled = state != IVC_MAP_IDLE;
    const int idx = state == IVC_MAP_PICKED ? IntvCtlTargetIndex(_pick) : -1;
    if (![_mapList selectItemWithTag:idx]) [_mapList selectItemAtIndex:0];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode, or chosen from the list. */
- (void)mapPick:(intvsession_key_mapping)target
{
    if (_mapState == IVC_MAP_IDLE || target.kind == INTVSESSION_MAP_NONE) return;
    _pick = target;
    [self setMapMode:IVC_MAP_PICKED];
}

- (void)mapListChanged:(NSPopUpButton *)sender
{
    const NSInteger tag = sender.selectedItem.tag;
    if (tag >= 0) [self mapPick:intvsession_target_from_index((int)tag)];
    else if (_mapState == IVC_MAP_PICKED) [self setMapMode:IVC_MAP_ARMED];
}

- (void)showBound:(NSString *)name target:(intvsession_key_mapping)target stolen:(const char *)stolen
{
    NSString *what = IntvCtlTargetName(target);
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, IntvCtlString(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const intvsession_key_mapping target = _pick;
    intvsession_target_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapMode:IVC_MAP_ARMED];     /* stay armed: remapping several in a row is normal */
    [self showBound:IntvCtlKeyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    intvsession_pad_button button = INTVSESSION_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState != IVC_MAP_PICKED || !intvsession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const intvsession_key_mapping target = _pick;
    intvsession_target_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapMode:IVC_MAP_ARMED];
    [self showBound:IntvCtlString(intvsession_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapMode:(_mapState == IVC_MAP_IDLE ? IVC_MAP_ARMED : IVC_MAP_IDLE)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState != IVC_MAP_PICKED) return;
    const intvsession_key_mapping target = _pick;
    intvsession_binding_clear(_session, target);
    [self setMapMode:IVC_MAP_ARMED];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", IntvCtlTargetName(target)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    intvsession_bindings_reset(_session);
    for (int pad = 0, count = intvsession_gamepad_count(_session); pad < count; pad++)
        intvsession_gamepad_assign(_session, pad, -1);
    [self refreshPads];
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    intvsession_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* ---- gamepads ------------------------------------------------------------------------- */

/* Automatic, then one item per side the session has: Left and Right, and with
 * the ECS on, ECS Left and ECS Right. */
- (void)fillAssignPopups
{
    const int ecs = intvsession_ecs_on(_session) ? 1 : 0;
    if (ecs == _assignEcs) return;
    _assignEcs = ecs;
    const int sides = ecs ? 4 : 2;
    for (int row = 0; row < IVC_MAX_PAD_ROWS; row++) {
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemWithTitle:@"Automatic"];
        for (int side = 0; side < sides; side++)
            [_padAssign[row] addItemWithTitle:IntvCtlString(intvsession_pad_side_name((intvsession_pad_side)side))];
    }
}

- (void)refreshPadSides
{
    for (int row = 0; row < _padRows; row++) {
        const int side = intvsession_gamepad_effective_side(_session, row);
        NSString *text = side >= 0
            ? [NSString stringWithFormat:@"drives %@", IntvCtlString(intvsession_pad_side_name((intvsession_pad_side)side))]
            : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

/* Rebuilt whenever the pads' generation changes (one added or removed). */
- (void)refreshPads
{
    const int count = intvsession_gamepad_count(_session);
    _padGeneration = intvsession_gamepad_generation(_session);
    _padRows = count < IVC_MAX_PAD_ROWS ? (count > 0 ? count : 0) : IVC_MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more pads
     * are connected); the panel changes size only then */
    const int rooms = _padRows > IVC_MIN_PAD_ROWS ? _padRows : IVC_MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > IVC_MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", IVC_MAX_PAD_ROWS, count]
        : @"Gamepads";
    const int sides = _assignEcs ? 4 : 2;
    for (int row = 0; row < IVC_MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        intvsession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = IntvCtlString(name);
        _padName[row].toolTip = IntvCtlString(name);
        /* the pad's own side, or Automatic when it has none */
        const int assign = intvsession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= sides ? 0 : assign + 1)];
    }
    [self refreshPadSides];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    intvsession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadSides];
}

/* ---- the poll ------------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the ECS option, the hot-plugged pads and what Auto resolved to. */
- (void)tick
{
    if (_mapState == IVC_MAP_PICKED) [self pollCapture];
    if (++_slow >= IVC_SLOW_TICKS) {
        _slow = 0;
        [self syncLayout];
        if (intvsession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadSides];
    }
    [_stage poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak IntvControllersWindow *weakSelf = self;
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

/* ---- the keyboard --------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = IntvKeysymForEvent(evt);
    if (!keysym) return;
    /* the keys the app owns: F9 toggles this panel, F10 the ECS keyboard, F11 is
     * full screen, F12 the debugger (the menus take them first; this is for when
     * they did not) */
    if (keysym == IVC_KEYSYM_F9 || keysym == IVC_KEYSYM_F10 || keysym == IVC_KEYSYM_F11 || keysym == IVC_KEYSYM_F12) {
        if ([evt isARepeat]) return;
        if (keysym == IVC_KEYSYM_F9) [self toggle];
        else if (keysym == IVC_KEYSYM_F10) [EcsKeyboardWindow toggleForSession:_session];
        else if (keysym == IVC_KEYSYM_F11) [[NSApp mainWindow] toggleFullScreen:nil];
        else [DebuggerWindow showForSession:_session];
        return;
    }
    if (_swallowKey && keysym == _swallowKey) return;
    if ([evt isARepeat]) return;
    if (_mapState == IVC_MAP_PICKED) {
        /* the next key is the binding; Esc backs out of the pick; a Ctrl chord
         * and the app's keys are not bindings */
        if ([evt modifierFlags] & NSEventModifierFlagControl) return;
        if (intvsession_key_reserved(keysym)) return;
        if (keysym == INTVSESSION_KEYSYM_ESCAPE) {
            [self setMapMode:IVC_MAP_ARMED];
            _swallowKey = keysym;
        } else {
            [self bindKey:keysym swallow:YES];
        }
        return;
    }
    if (_mapState == IVC_MAP_ARMED) return;     /* armed: typing does nothing */
    IntvForwardKeyEvent(_session, evt, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    const uint32_t keysym = IntvKeysymForEvent(evt);
    if (_swallowKey && keysym == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (_mapState != IVC_MAP_IDLE || !keysym) return;
    IntvForwardKeyEvent(_session, evt, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they are
 * the machine's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    if (!IntvModifierKeyState(evt, &down)) return;
    const uint32_t keysym = IntvKeysymForKeyCode(evt.keyCode);
    if (_mapState == IVC_MAP_PICKED) {
        /* a modifier pressed while a pick waits is the binding */
        if (down && keysym) [self bindKey:keysym swallow:NO];
        return;
    }
    if (_mapState == IVC_MAP_ARMED) return;
    IntvForwardKeyEvent(_session, evt, down);
}

/* ---- the window ------------------------------------------------------------------------ */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapMode:IVC_MAP_IDLE];
    [self letGo];
    intvsession_release_all(_session);
    intvsession_ecs_keys_clear(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [_stage configure];
    [self fillAssignPopups];
    [self refreshPads];
    [self setMapMode:IVC_MAP_IDLE];
    [self relayout];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (BOOL)isActive
{
    return [_panel isKeyWindow];
}

/* The bindings or the gamepads changed elsewhere (Settings, a hot-plug): read
 * again. */
- (void)refresh
{
    [self syncLayout];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    intvsession_release_all(_session);
    intvsession_ecs_keys_clear(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
