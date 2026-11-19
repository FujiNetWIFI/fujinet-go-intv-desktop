/*
 * The Win32 ECS Keyboard window (F10): the Intellivision Computer Keyboard as
 * the real one looks -- beige case, the INTELLIVISION label plate, ventilation
 * ridges, the red pinstripe, black keys with their shifted legends above them,
 * the white RTN and two SHIFT caps -- drawn from the session's layout
 * (intvsession_ecs_layout) in a fixed-size tool window.
 *
 * Clicking a key holds it for as long as the mouse button is down
 * (intvsession_ecs_key_set); SHIFT and CTL latch: a click flips them, and a
 * latched one stays lit until it is clicked again or the window is left for
 * another program. A cap is lit while the machine sees its key held
 * (intvsession_ecs_key_held), so typing on the physical keyboard lights it too.
 * The window is the ECS keyboard whether or not the "keyboard_mode" setting
 * routes the physical keyboard there, so typing in it always reaches the ECS
 * matrix (intv_forward_ecs_key); the hotkeys go to the main window.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE; the legends go on after at the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ecskbd_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../accent_button.h"
#include "../controllers/gdi_art.h"
#include "../help/help_window.h"
#include "../key_forward.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoIntvEcsKeyboard"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

#define UNIT96   22         /* pixels per layout unit at 96 dpi */
#define MARGIN   12
#define BAR_H    28
#define MAX_CAPS 64

#define IDT_HELD 1

static HWND g_panel, g_parent;
static intvsession *g_session;
static int g_dpi = 96;

static const intvsession_ecs_cap *g_caps;
static int g_numcaps;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;
static int g_total_w, g_total_h;
static RECT g_art_rc, g_notice_rc, g_help_rc;

static int g_mouse_cap = -1;                /* the cap the mouse holds down (not a latch) */
static unsigned char g_lit[MAX_CAPS];       /* what was last painted */
static int g_notice_shown, g_mode_shown = -1;
static HFONT g_font, g_key, g_small, g_label, g_script;

static int scaled(int val) { return intv_scale(val, g_dpi); }

#define HX(val) gdi_rgb(val)

#define COL_HELD HX(INTVSESSION_HELD_RGB)

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_key, &g_small, &g_label, &g_script };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = intv_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_key = gdi_face_font(g_dpi, 12, FW_BOLD, 0);       /* a key's main legend */
    g_small = gdi_face_font(g_dpi, 9, FW_BOLD, 0);      /* the shifted legend, SPACE */
    g_label = gdi_face_font(g_dpi, 12, FW_BOLD, 0);     /* INTELLIVISION on the plate */
    g_script = gdi_face_font(g_dpi, 10, FW_BOLD, 1);    /* the red MATTEL ELECTRONICS script */
}

/* ---- the art ------------------------------------------------------------------- */

static int cap_lit(const intvsession_ecs_cap *cap)
{
    return cap->kind == INTVSESSION_ECS_KEYCAP && intvsession_ecs_key_held(g_session, (intvsession_ecs_key)cap->key);
}

static void cap_box(const intvsession_ecs_cap *cap, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)cap->x * g_unit;
    *ypos = g_orgy + (double)cap->y * g_unit;
    *width = (double)cap->w * g_unit;
    *height = (double)cap->h * g_unit;
}

static void cap_rect(const intvsession_ecs_cap *cap, RECT *rect)
{
    double xpos, ypos, width, height;
    cap_box(cap, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static void art_body(HDC hdc, const intvsession_ecs_cap *cap)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    int ridge;
    cap_box(cap, &boxx, &boxy, &boxw, &boxh);
    if (!strcmp(cap->label, "Case")) {
        gdi_rrect(hdc, boxx + unit * 0.12, boxy + unit * 0.18, boxw, boxh, unit * 1.0, HX(0x59564B), HX(0x59564B), 0);
        gdi_grad_rrect(hdc, boxx, boxy, boxw, boxh, unit * 1.0, gdi_mix(HX(INTVSESSION_ECS_CASE_RGB), HX(0xFFFFFF), 0.25),
                       HX(INTVSESSION_ECS_CASE_RGB), HX(INTVSESSION_SHADE_RGB), unit * 0.08);
    } else if (!strcmp(cap->label, "Plate")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.6, HX(INTVSESSION_ECS_PLATE_RGB), HX(INTVSESSION_SHADE_RGB),
                  unit * 0.06);
    } else if (!strcmp(cap->label, "Label")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, HX(INTVSESSION_ECS_LABEL_RGB), HX(0x1F181D), unit * 0.05);
    } else if (!strcmp(cap->label, "Vents")) {
        for (ridge = 0; ridge < 6; ridge++) {
            const double row = boxy + (ridge + 0.5) * boxh / 6.0;
            gdi_line(hdc, boxx, row, boxx + boxw, row, HX(INTVSESSION_ECS_RIDGE_RGB), unit * 0.16);
            gdi_line(hdc, boxx, row + unit * 0.14, boxx + boxw, row + unit * 0.14,
                     gdi_mix(HX(INTVSESSION_ECS_CASE_RGB), HX(0xFFFFFF), 0.5), unit * 0.05);
        }
    } else if (!strcmp(cap->label, "Pinstripe")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.1, HX(INTVSESSION_ECS_PINSTRIPE_RGB), HX(INTVSESSION_ECS_PINSTRIPE_RGB),
                  0);
    } else if (!strcmp(cap->label, "Cable")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.2, HX(INTVSESSION_CABLE_RGB), HX(0x2B2B2B), unit * 0.05);
    }
}

static void art_cap(HDC hdc, const intvsession_ecs_cap *cap)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    const int lit = cap_lit(cap), white = (cap->flags & INTVSESSION_ECSCAP_WHITE) != 0;
    const COLORREF face = white ? HX(INTVSESSION_ECS_RTN_RGB) : HX(INTVSESSION_ECS_KEY_RGB);
    cap_box(cap, &boxx, &boxy, &boxw, &boxh);
    gdi_rrect(hdc, boxx + unit * 0.06, boxy + unit * 0.12, boxw, boxh, unit * 0.3, HX(0x59564B), HX(0x59564B), 0);
    gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, lit ? COL_HELD : face, lit ? COL_HELD : gdi_mix(face, HX(0), 0.4),
              unit * 0.07);
    if (!lit)
        gdi_line(hdc, boxx + unit * 0.3, boxy + unit * 0.14, boxx + boxw - unit * 0.3, boxy + unit * 0.14,
                 gdi_mix(face, HX(0xFFFFFF), white ? 0.0 : 0.22), unit * 0.06);
}

static void art_all(HDC hdc)
{
    int idx;
    for (idx = 0; idx < g_numcaps; idx++) {
        if (g_caps[idx].kind == INTVSESSION_ECS_BODY) art_body(hdc, &g_caps[idx]);
        else art_cap(hdc, &g_caps[idx]);
    }
}

static void text_all(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numcaps; idx++) {
        const intvsession_ecs_cap *cap = &g_caps[idx];
        RECT rect;
        cap_rect(cap, &rect);
        if (cap->kind == INTVSESSION_ECS_BODY) {
            if (!strcmp(cap->label, "Label") && cap->shifted) {
                /* the two white lines on the plate */
                char first[48];
                const char *bar = strchr(cap->shifted, '|');
                RECT top = rect, bottom = rect;
                size_t len = bar ? (size_t)(bar - cap->shifted) : strlen(cap->shifted);
                if (len >= sizeof first) len = sizeof first - 1;
                memcpy(first, cap->shifted, len);
                first[len] = '\0';
                top.left += scaled(8);
                top.bottom = rect.top + (rect.bottom - rect.top) * 11 / 20;
                bottom.left += scaled(8);
                bottom.top = top.bottom;
                SelectObject(hdc, g_label);
                SetTextColor(hdc, HX(INTVSESSION_LEGEND_RGB));
                gdi_text_line(hdc, first, &top, DT_LEFT);
                if (bar) {
                    SelectObject(hdc, g_small);
                    gdi_text_line(hdc, bar + 1, &bottom, DT_LEFT);
                }
            } else if (!strcmp(cap->label, "Mattel") && cap->shifted) {
                /* on the plate's first line, to the right of INTELLIVISION */
                OffsetRect(&rect, scaled(4), -(rect.bottom - rect.top));
                SelectObject(hdc, g_script);
                SetTextColor(hdc, HX(INTVSESSION_MATTEL_RED_RGB));
                gdi_text_line(hdc, cap->shifted, &rect, DT_RIGHT);
            }
            continue;
        }
        {
            const int lit = cap_lit(cap), white = (cap->flags & INTVSESSION_ECSCAP_WHITE) != 0;
            const COLORREF ink = lit ? RGB(0, 0, 0)
                                     : (white ? HX(INTVSESSION_ECS_RTN_INK_RGB) : HX(INTVSESSION_LEGEND_RGB));
            RECT main = rect, upper = rect;
            if (cap->shifted) {
                const int mid = rect.top + (rect.bottom - rect.top) * 9 / 20;
                upper.bottom = mid;
                main.top = mid;
                SelectObject(hdc, g_small);
                SetTextColor(hdc, ink);
                gdi_text_line(hdc, cap->shifted, &upper, DT_CENTER);
            }
            SelectObject(hdc, cap->label[1] != '\0' && (unsigned char)cap->label[0] < 0x80 ? g_small : g_key);
            SetTextColor(hdc, ink);
            gdi_text_line(hdc, cap->label, &main, DT_CENTER);
        }
    }
}

/* ---- layout -------------------------------------------------------------------- */

static void layout_static(void)
{
    const int left = scaled(MARGIN);
    int ypos = scaled(MARGIN), art_w;

    g_caps = intvsession_ecs_layout(&g_numcaps, &g_layw, &g_layh);
    if (g_numcaps > MAX_CAPS) g_numcaps = MAX_CAPS;
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    art_w = (int)ceil((double)g_layw * g_unit);
    g_orgx = left;
    g_orgy = ypos;
    SetRect(&g_art_rc, left, ypos, left + art_w, ypos + (int)ceil((double)g_layh * g_unit) + scaled(3));
    g_total_w = 2 * left + art_w;
    ypos = g_art_rc.bottom + scaled(8);
    SetRect(&g_help_rc, left + art_w - scaled(56), ypos, left + art_w, ypos + scaled(BAR_H));
    SetRect(&g_notice_rc, left, ypos, g_help_rc.left - scaled(8), ypos + scaled(BAR_H));
    g_total_h = ypos + scaled(BAR_H) + scaled(MARGIN);
}

static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

/* ---- state ---------------------------------------------------------------------- */

/* The ECS is off (or has no ecs.bin): the keyboard does nothing until Settings turns it on. */
static int ecs_missing(void)
{
    return !intvsession_ecs_on(g_session);
}

static void update_title(void)
{
    const int mode = intvsession_get_int(g_session, "keyboard_mode", 0) != 0;
    if (!g_panel || mode == g_mode_shown) return;
    g_mode_shown = mode;
    SetWindowTextA(g_panel, mode ? "ECS Keyboard (F10) - the keyboard types here" : "ECS Keyboard (F10)");
}

/* Lights follow what the machine sees held; redrawn when any changed. */
static void refresh_held(void)
{
    unsigned char now[MAX_CAPS];
    int idx;
    if (!g_panel || !g_session) return;
    memset(now, 0, sizeof now);
    for (idx = 0; idx < g_numcaps; idx++) now[idx] = (unsigned char)cap_lit(&g_caps[idx]);
    if (memcmp(now, g_lit, sizeof now) != 0) {
        memcpy(g_lit, now, sizeof now);
        InvalidateRect(g_panel, &g_art_rc, FALSE);
    }
    if (ecs_missing() != g_notice_shown) {
        g_notice_shown = ecs_missing();
        InvalidateRect(g_panel, &g_notice_rc, FALSE);
    }
    update_title();
}

static void release_mouse(void)
{
    if (g_mouse_cap >= 0 && g_session) {
        const intvsession_ecs_cap *cap = &g_caps[g_mouse_cap];
        intvsession_ecs_key_set(g_session, (intvsession_ecs_key)cap->key, 0);
    }
    g_mouse_cap = -1;
    if (GetCapture() == g_panel) ReleaseCapture();
}

static void hide_panel(void)
{
    release_mouse();
    ShowWindow(g_panel, SW_HIDE);
}

/* ---- window ---------------------------------------------------------------------- */

static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);
    IntersectRect(&art, inval, &g_art_rc);
    if (!IsRectEmpty(&art)) {
        gdi_paint_art(hdc, &art, GetSysColor(COLOR_BTNFACE), art_all);
        SelectObject(hdc, g_font);
        text_all(hdc);
    }
    SelectObject(hdc, g_font);
    if (ecs_missing()) {
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        gdi_text_line(hdc, "The ECS is not enabled: turn it on in Settings (it applies on restart).", &g_notice_rc,
                      DT_LEFT | DT_END_ELLIPSIS);
    } else {
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        gdi_text_line(hdc, "SHIFT and CTL latch: click again to let go.", &g_notice_rc, DT_LEFT | DT_END_ELLIPSIS);
    }
    gdi_button_face(hdc, &g_help_rc, 0, 0, INTV_ACCENT_COLORREF, INTV_ACCENT_INK_COLORREF);
    gdi_text_line(hdc, "Help", &g_help_rc, DT_CENTER);
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int cap;
    SetFocus(hwnd);
    if (PtInRect(&g_help_rc, pnt)) {
        intv_help_show(hwnd, "ecskbd");
        return;
    }
    cap = intvsession_ecs_hit((float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
    if (cap < 0 || cap >= g_numcaps) return;
    if (g_caps[cap].flags & INTVSESSION_ECSCAP_LATCH) {
        /* a click flips a latch; both SHIFT caps are the same key */
        const intvsession_ecs_key key = (intvsession_ecs_key)g_caps[cap].key;
        intvsession_ecs_key_set(g_session, key, !intvsession_ecs_key_held(g_session, key));
    } else {
        release_mouse();
        g_mouse_cap = cap;
        intvsession_ecs_key_set(g_session, (intvsession_ecs_key)g_caps[cap].key, 1);
        SetCapture(hwnd);
    }
    refresh_held();
}

/* A key with the window focused: the hotkeys belong to the main window, the
 * rest types on the ECS matrix. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (vk == VK_F10) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (intv_key_is_reserved(vk, NULL)) {
        if (g_parent) SendMessageA(g_parent, msg, vk, lparam);
        return 1;
    }
    intv_forward_ecs_key(vk, lparam, down);
    return 1;
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_LBUTTONUP:
        release_mouse();
        refresh_held();
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd && g_mouse_cap >= 0) {
            release_mouse();
            refresh_held();
        }
        return 0;
    case WM_TIMER:
        if (wparam == IDT_HELD) refresh_held();
        return 0;

    /* WM_SYSKEY* too: Alt is a key here as in the main window */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_ENTERMENULOOP:
        intvsession_ecs_keys_clear(g_session);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            release_mouse();
            /* Leaving the app: nothing typed here, a latch included, stays
             * held (within it, the click that latches goes on working). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                intvsession_ecs_keys_clear(g_session);
            refresh_held();
        }
        break;
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            release_mouse();
        }
        break;
    case WM_GETMINMAXINFO:
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        g_dpi = HIWORD(wparam);
        make_fonts();
        layout_static();
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy; the keys let go with it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        release_mouse();
        g_panel = NULL;
        g_numcaps = 0;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = intv_window_dpi(g_parent);
    make_fonts();
    layout_static();
    SetRect(&want, 0, 0, g_total_w, g_total_h);
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "ECS Keyboard (F10)", PANEL_STYLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, want.right - want.left, want.bottom - want.top, g_parent, NULL, inst,
                              NULL);
    if (!g_panel) return 0;
    intv_help_set_context(g_panel, "ecskbd", 0);
    g_mode_shown = -1;
    g_notice_shown = -1;
    memset(g_lit, 0xFF, sizeof g_lit);
    {
        const int dpi = intv_window_dpi(g_panel);   /* placed on another monitor than the main window */
        if (dpi != g_dpi) {
            g_dpi = dpi;
            make_fonts();
            layout_static();
        }
    }
    fit_client();
    return 1;
}

void intv_ecskbd_window_toggle(HWND parent, intvsession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) {
        hide_panel();
        return;
    }
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

int intv_ecskbd_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}
