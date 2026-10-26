/*
 * A toggle button in the app's accent colour: dark green (#1E4912) with
 * white text while it is on, the stock look while it is off. Done through the
 * button's own NM_CUSTOMDRAW (comctl32 v6, see app.manifest.in), so an off
 * button keeps the themed look and its focus, hover and press behaviour, and
 * a check state stays the button's own (BM_GETCHECK) -- only the "on" paint
 * is replaced.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_WIN_ACCENT_BUTTON_H
#define INTV_WIN_ACCENT_BUTTON_H

#include <windows.h>
#include <commctrl.h>

#include "intvsession.h"

#define INTV_ACCENT_COLORREF                                              \
    RGB((INTVSESSION_ACCENT_RGB >> 16) & 0xFF,                           \
        (INTVSESSION_ACCENT_RGB >> 8) & 0xFF, INTVSESSION_ACCENT_RGB & 0xFF)

/* Handles one NM_CUSTOMDRAW from a button. on = the button's toggle state;
 * the result is what the parent's WM_NOTIFY should return. */
static inline LRESULT intv_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    static HBRUSH accent;
    char text[64];
    RECT r;

    if (cd->dwDrawStage != CDDS_PREPAINT || !on)
        return CDRF_DODEFAULT;
    if (!accent)
        accent = CreateSolidBrush(INTV_ACCENT_COLORREF);
    r = cd->rc;
    FillRect(cd->hdc, &r, accent);
    FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, RGB(255, 255, 255));
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        /* the control's own font, not the DC's default */
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#endif
