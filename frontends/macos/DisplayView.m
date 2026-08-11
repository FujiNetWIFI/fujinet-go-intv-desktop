/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to a fixed 4:3 (the STIC's pixels are not square, and the
 * frame buffer's geometry never varies at runtime -- unlike the CoCo/ADAM
 * ports, there is no aspect-mode setting or CVDisplayLink-fed phase-lock
 * here: intvsession has no notify_vsync call, since jzIntv's own paced
 * thread (core/jzintv/intv_host.c) already governs itself to NTSC/PAL
 * speed independent of any frontend. A plain NSTimer drives the repaint
 * poll instead, matching the reasoning in frontends/gnome/display.c and
 * frontends/windows/main.c's own file comments.
 *
 * Keyboard: intvsession_key_from_keysym wants intvsession.h's OWN private
 * numbering for non-printable keys (INTVSESSION_KEYSYM_* starting at
 * 0x1000), not a raw NSEvent keyCode or character -- see the GNOME port's
 * keysym_map.h for why that distinction matters (passing a toolkit's
 * native key value straight through silently drops every arrow/numpad/
 * modifier key while ASCII letters keep working by coincidence). Virtual
 * keyCodes (layout-independent, unlike charactersIgnoringModifiers, which
 * cannot tell a numpad digit from a top-row one) are used for arrows/
 * numpad/modifiers, matching the Windows port's own special_keysym()/
 * base_char() split with VK_* codes.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#define TICK_INTERVAL (1.0 / 60.0)

@implementation DisplayView {
    intvsession *_session;
    NSTimer *_timer;
    uint32_t *_buf; /* INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT, XRGB8888 */
    uint64_t _serial;
    CGImageRef _image;
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 800, 650)];
    if (!self)
        return nil;
    _session = session;
    _buf = calloc((size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT,
                 sizeof(uint32_t));
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_buf);
}

- (void)start
{
    if (_timer)
        return;
    __weak DisplayView *weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:TICK_INTERVAL
                                             repeats:YES
                                               block:^(NSTimer *t) {
                                                 (void)t;
                                                 [weakSelf pullFrame];
                                               }];
}

- (void)stop
{
    [_timer invalidate];
    _timer = nil;
}

- (void)pullFrame
{
    if (!intvsession_copy_frame(_session, _buf, &_serial))
        return;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _buf,
        (size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT * 4, NULL);
    /* The core's XRGB8888 is 0x00RRGGBB, so in memory on a little-endian
     * host the bytes run B,G,R,X -- kCGBitmapByteOrder32Little with
     * kCGImageAlphaNoneSkipFirst reads exactly that, same conclusion the
     * GNOME port's own display.c comment draws for GDK_MEMORY_B8G8R8X8. */
    CGImageRef image = CGImageCreate(
        INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT, 8, 32,
        INTVSESSION_FB_WIDTH * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    const CGFloat aspect = 4.0 / 3.0;
    CGFloat dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

/* ---- keyboard -------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* macOS virtual keyCodes for arrows/numpad/modifiers -- layout-independent,
 * unlike characters. */
enum {
    kVK_ANSI_KeypadDecimal = 0x41,
    kVK_ANSI_KeypadMultiply = 0x43,
    kVK_ANSI_KeypadPlus = 0x45,
    kVK_ANSI_KeypadClear = 0x47,
    kVK_ANSI_KeypadDivide = 0x4B,
    kVK_ANSI_KeypadEnter = 0x4C,
    kVK_ANSI_KeypadMinus = 0x4E,
    kVK_ANSI_KeypadEquals = 0x51,
    kVK_ANSI_Keypad0 = 0x52,
    kVK_ANSI_Keypad1 = 0x53,
    kVK_ANSI_Keypad2 = 0x54,
    kVK_ANSI_Keypad3 = 0x55,
    kVK_ANSI_Keypad4 = 0x56,
    kVK_ANSI_Keypad5 = 0x57,
    kVK_ANSI_Keypad6 = 0x58,
    kVK_ANSI_Keypad7 = 0x59,
    kVK_ANSI_Keypad8 = 0x5B,
    kVK_ANSI_Keypad9 = 0x5C,
    kVK_Shift = 0x38,
    kVK_RightShift = 0x3C,
    kVK_Control = 0x3B,
    kVK_RightControl = 0x3E,
    kVK_Option = 0x3A,
    kVK_RightOption = 0x3D,
    kVK_LeftArrow = 0x7B,
    kVK_RightArrow = 0x7C,
    kVK_DownArrow = 0x7D,
    kVK_UpArrow = 0x7E,
};

static uint32_t special_keysym(unsigned short keyCode)
{
    switch (keyCode) {
    case kVK_UpArrow:    return INTVSESSION_KEYSYM_UP;
    case kVK_DownArrow:  return INTVSESSION_KEYSYM_DOWN;
    case kVK_LeftArrow:  return INTVSESSION_KEYSYM_LEFT;
    case kVK_RightArrow: return INTVSESSION_KEYSYM_RIGHT;
    case kVK_ANSI_Keypad0: return INTVSESSION_KEYSYM_KP_0;
    case kVK_ANSI_Keypad1: return INTVSESSION_KEYSYM_KP_1;
    case kVK_ANSI_Keypad2: return INTVSESSION_KEYSYM_KP_2;
    case kVK_ANSI_Keypad3: return INTVSESSION_KEYSYM_KP_3;
    case kVK_ANSI_Keypad4: return INTVSESSION_KEYSYM_KP_4;
    case kVK_ANSI_Keypad5: return INTVSESSION_KEYSYM_KP_5;
    case kVK_ANSI_Keypad6: return INTVSESSION_KEYSYM_KP_6;
    case kVK_ANSI_Keypad7: return INTVSESSION_KEYSYM_KP_7;
    case kVK_ANSI_Keypad8: return INTVSESSION_KEYSYM_KP_8;
    case kVK_ANSI_Keypad9: return INTVSESSION_KEYSYM_KP_9;
    case kVK_ANSI_KeypadDecimal: return INTVSESSION_KEYSYM_KP_PERIOD;
    case kVK_ANSI_KeypadEnter:   return INTVSESSION_KEYSYM_KP_ENTER;
    case kVK_Shift:        return INTVSESSION_KEYSYM_LSHIFT;
    case kVK_RightShift:   return INTVSESSION_KEYSYM_RSHIFT;
    case kVK_Control:      return INTVSESSION_KEYSYM_LCTRL;
    case kVK_RightControl: return INTVSESSION_KEYSYM_RCTRL;
    case kVK_Option:       return INTVSESSION_KEYSYM_LALT;
    case kVK_RightOption:  return INTVSESSION_KEYSYM_RALT;
    default:
        return 0;
    }
}

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    uint32_t keysym = special_keysym(event.keyCode);
    intvsession_key_mapping m;

    if (!keysym) {
        NSString *chars = event.charactersIgnoringModifiers;
        unichar c = chars.length ? [chars characterAtIndex:0] : 0;
        if (c >= 0x20 && c < 0x7F)
            keysym = c;
        else
            return; /* not a key the emulated controller has */
    }

    m = intvsession_key_from_keysym(keysym);
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(_session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(_session, m.side, down ? m.direction : -1);
}

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat)
        return; /* pad_t tracks a held key by level, not by repeat count */
    [self forwardKeyEvent:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKeyEvent:event down:0];
}

- (void)flagsChanged:(NSEvent *)event
{
    /* NSEventTypeFlagsChanged carries no down/up of its own -- infer it
     * from whether the bit for this specific key's side is now set, the
     * standard AppKit idiom (matches the CoCo/ADAM ports' own
     * flagsChanged: handling). */
    NSEventModifierFlags f = event.modifierFlags;
    int down;
    switch (event.keyCode) {
    case kVK_Shift:        down = (f & NSEventModifierFlagShift) != 0; break;
    case kVK_RightShift:   down = (f & NSEventModifierFlagShift) != 0; break;
    case kVK_Control:      down = (f & NSEventModifierFlagControl) != 0; break;
    case kVK_RightControl: down = (f & NSEventModifierFlagControl) != 0; break;
    case kVK_Option:       down = (f & NSEventModifierFlagOption) != 0; break;
    case kVK_RightOption:  down = (f & NSEventModifierFlagOption) != 0; break;
    default:
        return;
    }
    [self forwardKeyEvent:event down:down];
}

@end
