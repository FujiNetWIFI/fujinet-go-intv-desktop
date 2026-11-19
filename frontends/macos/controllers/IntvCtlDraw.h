/*
 * IntvCtlDraw -- the few drawing helpers the Controllers panel and the ECS
 * keyboard panel share. Every name carries the IntvCtl prefix on purpose: a
 * generic file-static name (outline, polygon, rgb, point, rect, line, text,
 * box, color, draw, fill, stroke) can clash with a symbol of the macOS SDK,
 * which only the macOS CI build would show.
 *
 * Static inline, so a file that does not use one pays nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include <math.h>
#include <stdint.h>

static inline NSString *IntvCtlString(const char *chars)
{
    NSString *res = chars ? [NSString stringWithUTF8String:chars] : nil;
    return res ? res : @"";
}

static inline NSColor *IntvCtlColorAlpha(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

static inline NSColor *IntvCtlColor(uint32_t value)
{
    return IntvCtlColorAlpha(value, 1.0);
}

static inline uint32_t IntvCtlMix(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double chanA = (double)((from >> shift) & 0xff);
        const double chanB = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(chanA + (chanB - chanA) * frac) << shift;
    }
    return out;
}

/* A linear gradient clipped to a path, from one point to another. */
static inline void IntvCtlGradientBetween(NSBezierPath *path, NSPoint from, NSPoint to,
                                          uint32_t rgbFrom, uint32_t rgbTo)
{
    const CGFloat comps[8] = {
        ((rgbFrom >> 16) & 0xff) / 255.0, ((rgbFrom >> 8) & 0xff) / 255.0, (rgbFrom & 0xff) / 255.0, 1.0,
        ((rgbTo >> 16) & 0xff) / 255.0, ((rgbTo >> 8) & 0xff) / 255.0, (rgbTo & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static inline void IntvCtlGradientFill(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    IntvCtlGradientBetween(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)),
                           NSMakePoint(NSMinX(bnd), NSMaxY(bnd)), topRgb, botRgb);
}

static inline NSBezierPath *IntvCtlRoundRect(NSRect frame, CGFloat radius)
{
    return [NSBezierPath bezierPathWithRoundedRect:frame xRadius:radius yRadius:radius];
}

static inline NSRect IntvCtlCircleRect(NSPoint centre, CGFloat radius)
{
    return NSMakeRect(centre.x - radius, centre.y - radius, 2 * radius, 2 * radius);
}

static inline void IntvCtlFillOval(NSRect frame, NSColor *colour)
{
    [colour setFill];
    [[NSBezierPath bezierPathWithOvalInRect:frame] fill];
}

static inline void IntvCtlStrokeSegment(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *seg = [NSBezierPath bezierPath];
    [seg moveToPoint:from];
    [seg lineToPoint:to];
    seg.lineWidth = width;
    seg.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [seg stroke];
}

/* Text in frame with the given alignment, the font shrunk (to 5 pt) until it
 * fits, centred vertically. */
static inline void IntvCtlDrawText(NSString *label, NSRect frame, CGFloat size, BOOL bold,
                                   BOOL slanted, NSTextAlignment align, NSColor *ink)
{
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = align;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        NSMutableDictionary *dict = [NSMutableDictionary dictionaryWithDictionary:@{
            NSFontAttributeName : font,
            NSParagraphStyleAttributeName : para,
            NSForegroundColorAttributeName : ink }];
        if (slanted) dict[NSObliquenessAttributeName] = @(0.2);
        attrs = dict;
        const NSSize extent = [label sizeWithAttributes:attrs];
        lineHeight = extent.height;
        if ((extent.width <= frame.size.width && lineHeight <= frame.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    const CGFloat ypos = frame.origin.y + floor((frame.size.height - lineHeight) / 2);
    [label drawInRect:NSMakeRect(frame.origin.x, ypos, frame.size.width, lineHeight)
       withAttributes:attrs];
}

/* A soft shadow on the backdrop under a shape. */
static inline void IntvCtlDropShadow(NSBezierPath *path)
{
    NSBezierPath *shadow = [path copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:3];
    [shadow transformUsingAffineTransform:move];
    [IntvCtlColorAlpha(0x000000, 0.35) setFill];
    [shadow fill];
}
