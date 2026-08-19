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
 * Keyboard: the translation and dispatch both live in IntvKeyForward.m,
 * shared with the keypad and ECS keyboard windows -- see that file's
 * header for the keyCode-versus-character reasoning, and the GNOME port's
 * keysym_map.h for why intvsession.h's private INTVSESSION_KEYSYM_*
 * numbering is not interchangeable with a toolkit's own key values.
 *
 * NOT BUILT OR RUN-VERIFIED -- see main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import "IntvKeyForward.h"

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

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    IntvForwardKeyEvent(_session, event, down);
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

/* Losing first-responder status shouldn't leave a key stuck down in
 * whichever matrix was active -- see intvsession_ecs_keys_clear's own
 * comment. */
- (BOOL)resignFirstResponder
{
    intvsession_ecs_keys_clear(_session);
    return [super resignFirstResponder];
}

- (void)flagsChanged:(NSEvent *)event
{
    int down;
    if (IntvModifierKeyState(event, &down))
        [self forwardKeyEvent:event down:down];
}

@end
