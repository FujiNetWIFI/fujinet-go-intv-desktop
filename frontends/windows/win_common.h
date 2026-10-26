/*
 * Helpers every window of the Win32 frontend shares: DPI, the UI and
 * monospace fonts at a window's DPI, UTF-8 text into wide controls, and a
 * UTF-8 message box.
 *
 * The manifest makes the process per-monitor DPI aware (v2), so nothing is
 * scaled for us: CreateFont(-13) is 13 physical pixels, tiny at 200 %.
 * Every window lays itself out in 96-dpi units through intv_scale() and
 * remakes its fonts on WM_DPICHANGED.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_COMMON_H
#define INTV_WIN_COMMON_H

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* The fixed-pitch face the views use (every Windows since Vista has it). */
#define INTV_MONO_FACE "Consolas"

static inline int intv_system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

/* GetDpiForWindow is Windows 10 1607 and later: looked up, not imported. */
static inline int intv_window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return (int)d;
    }
    return intv_system_dpi();
}

static inline int intv_scale(int v, int dpi)
{
    return MulDiv(v, dpi, 96);
}

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face. */
static inline HFONT intv_make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0)
        lf.lfHeight = -MulDiv(px, dpi, 96);
    else
        lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, intv_system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

/* A font on a window and every child of it. */
static BOOL CALLBACK intv_set_font_proc(HWND h, LPARAM font)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static inline void intv_set_font_all(HWND parent, HFONT font)
{
    EnumChildWindows(parent, intv_set_font_proc, (LPARAM)font);
}

/* The size of a font's cell: the width of one "0", the line height. */
static inline SIZE intv_font_cell(HWND hwnd, HFONT font)
{
    SIZE sz = { 8, 16 };
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICA tm;
    if (GetTextMetricsA(dc, &tm)) {
        sz.cx = tm.tmAveCharWidth;
        sz.cy = tm.tmHeight;
    }
    SIZE z;
    if (GetTextExtentPoint32A(dc, "0", 1, &z) && z.cx > 0)
        sz.cx = z.cx;
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return sz;
}

/* UTF-8 (the session's text) into a malloc'd wide string; NULL on failure. */
static inline wchar_t *intv_wide_dup(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, n);
    return w;
}

static inline void intv_set_text_utf8(HWND h, const char *utf8)
{
    wchar_t *w = intv_wide_dup(utf8);
    if (w) { SetWindowTextW(h, w); free(w); }
}

static inline int intv_message(HWND owner, const char *text, const char *title, UINT flags)
{
    wchar_t *wt = intv_wide_dup(text), *wc = intv_wide_dup(title);
    int rc = MessageBoxW(owner, wt ? wt : L"", wc ? wc : L"", flags);
    free(wt);
    free(wc);
    return rc;
}

/* A window's outer size for a client size, at its own style and DPI (the
 * frame and caption grow with the DPI too: AdjustWindowRectExForDpi, Windows
 * 10 1607 and later, looked up). */
static inline SIZE intv_outer_size(HWND hwnd, int cw, int ch, BOOL menu)
{
    typedef BOOL (WINAPI *adjust_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static adjust_fn fn;
    static int looked;
    RECT r = { 0, 0, cw, ch };
    const DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    const DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (adjust_fn)(void (*)(void))GetProcAddress(user, "AdjustWindowRectExForDpi") : NULL;
        looked = 1;
    }
    if (!fn || !fn(&r, style, menu, ex, (UINT)intv_window_dpi(hwnd)))
        AdjustWindowRectEx(&r, style, menu, ex);
    SIZE s = { r.right - r.left, r.bottom - r.top };
    return s;
}

/* A tab control used as a strip alone (the pages sit on the window's own
 * background below it): placed at x,y,w, exactly as tall as its tabs, so no
 * stray body shows. Returns its height. */
static inline int intv_place_tab_strip(HWND tabs, int x, int y, int w)
{
    RECT item;
    int h = 24;
    if (SendMessageW(tabs, 0x130A /* TCM_GETITEMRECT */, 0, (LPARAM)&item))
        h = item.bottom + 1;
    MoveWindow(tabs, x, y, w, h, TRUE);
    return h;
}

#endif /* INTV_WIN_COMMON_H */
