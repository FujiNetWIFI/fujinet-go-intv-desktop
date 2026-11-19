/*
 * GDI drawing helpers shared by the Controllers window and the ECS Keyboard
 * window: the art is drawn at twice the window's size into a memory bitmap
 * (GDI has no anti-aliasing of its own) and shrunk with HALFTONE, so these
 * take window pixels and scale them by gdi_art_scale. Each window that
 * includes this header gets its own copy of the state below.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GDI_NOFILL ((COLORREF)0xFFFFFFFFu)
#define GDI_PI 3.14159265358979323846

/* The supersampling factor, and the window pixel the bitmap's top-left is. */
static int gdi_art_scale = 1, gdi_art_left, gdi_art_top, gdi_art_rows;

static inline COLORREF gdi_rgb(unsigned val)
{
    return RGB((val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
}

static inline COLORREF gdi_mix(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

static inline int gdi_xpos(double val) { return (int)floor((val - gdi_art_left) * gdi_art_scale + 0.5); }
static inline int gdi_ypos(double val) { return (int)floor((val - gdi_art_top) * gdi_art_scale + 0.5); }
static inline int gdi_len(double val) { return (int)floor(val * gdi_art_scale + 0.5); }

static inline void gdi_ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill,
                               COLORREF line, double width)
{
    HBRUSH brush = fill == GDI_NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? gdi_len(width) : 0, line);
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, gdi_xpos(ctrx - radx), gdi_ypos(ctry - rady), gdi_xpos(ctrx + radx), gdi_ypos(ctry + rady));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
    if (fill != GDI_NOFILL) DeleteObject(brush);
}

static inline void gdi_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                             COLORREF fill, COLORREF line, double linew)
{
    HBRUSH brush = fill == GDI_NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? gdi_len(linew) : 0, line);
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, gdi_xpos(xpos), gdi_ypos(ypos), gdi_xpos(xpos + width), gdi_ypos(ypos + height),
              gdi_len(radius * 2), gdi_len(radius * 2));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
    if (fill != GDI_NOFILL) DeleteObject(brush);
}

static inline void gdi_line(HDC hdc, double xfrom, double yfrom, double xto, double yto, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, gdi_len(width) > 0 ? gdi_len(width) : 1, col);
    HGDIOBJ oldpen = SelectObject(hdc, pen);
    MoveToEx(hdc, gdi_xpos(xfrom), gdi_ypos(yfrom), NULL);
    LineTo(hdc, gdi_xpos(xto), gdi_ypos(yto));
    SelectObject(hdc, oldpen);
    DeleteObject(pen);
}

/* A pie slice of a circle, counter-clockwise on the screen from the angle
 * `first` to `last` (degrees, 0 = East, 90 = North: the disc's own numbering). */
static inline void gdi_pie(HDC hdc, double ctrx, double ctry, double rad, double first, double last, COLORREF fill)
{
    const double from = first * GDI_PI / 180.0, to = last * GDI_PI / 180.0;
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Pie(hdc, gdi_xpos(ctrx - rad), gdi_ypos(ctry - rad), gdi_xpos(ctrx + rad), gdi_ypos(ctry + rad),
        gdi_xpos(ctrx + rad * 2 * cos(from)), gdi_ypos(ctry - rad * 2 * sin(from)),
        gdi_xpos(ctrx + rad * 2 * cos(to)), gdi_ypos(ctry - rad * 2 * sin(to)));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(brush);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static inline void gdi_grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                                  COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = gdi_xpos(xpos), right = gdi_xpos(xpos + width);
    const int first = gdi_ypos(ypos), last = gdi_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, gdi_len(radius * 2), gdi_len(radius * 2));
    int row;
    HGDIOBJ oldbrush;
    SelectClipRgn(hdc, clip);
    oldbrush = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < gdi_art_rows; row++) {
        SetDCBrushColor(hdc, gdi_mix(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldbrush);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) gdi_rrect(hdc, xpos, ypos, width, height, radius, GDI_NOFILL, edge, edgew);
}

/* UTF-8 through DrawTextW. */
static inline int gdi_text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static inline void gdi_text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    gdi_text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Draws `paint` (which draws the art with the gdi_* calls above) at twice the
 * size into a bitmap covering `area` of the window and shrinks it onto hdc. */
static inline void gdi_paint_art(HDC hdc, const RECT *area, COLORREF backdrop, void (*paint)(HDC))
{
    const int scale = 2;
    const int width = area->right - area->left, height = area->bottom - area->top;
    BITMAPINFO info;
    void *bits = NULL;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = width * scale;
    info.bmiHeader.biHeight = -height * scale;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bitmap) {
        HBRUSH face = CreateSolidBrush(backdrop);
        RECT all = { 0, 0, width * scale, height * scale };
        HGDIOBJ old = SelectObject(mem, bitmap);
        FillRect(mem, &all, face);
        DeleteObject(face);
        SetBkMode(mem, TRANSPARENT);
        gdi_art_scale = scale;
        gdi_art_left = area->left;
        gdi_art_top = area->top;
        gdi_art_rows = height * scale;
        paint(mem);
        gdi_art_scale = 1;
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, area->left, area->top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bitmap);
    }
    DeleteDC(mem);
}

/* A push button face: the system's, or the app's accent green when `accent`. */
static inline void gdi_button_face(HDC hdc, const RECT *bounds, int accent, int inactive, COLORREF accent_fill,
                                   COLORREF accent_ink)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(accent_fill);
        FillRect(hdc, &rect, back);
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        DeleteObject(back);
        SetTextColor(hdc, accent_ink);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON, DFCS_BUTTONPUSH | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static inline HFONT gdi_face_font(int dpi, int pixels, int weight, int italic)
{
    return CreateFontW(-MulDiv(pixels, dpi, 96), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}
