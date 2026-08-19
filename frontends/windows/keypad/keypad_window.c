/*
 * KeypadWindow (Win32) -- both hand controllers side by side: a 3x4 keypad
 * grid (1-9, Clear/0/Enter), three action buttons (top, lower-left,
 * lower-right), and a 16-position direction disc, each driving
 * intvsession_pad_key/intvsession_pad_disc directly.
 *
 * Digit/action buttons are plain BUTTON controls subclassed to catch
 * WM_LBUTTONDOWN/WM_LBUTTONUP directly (a keypad digit is a real button on
 * real hardware, held for as long as the finger is down -- BN_CLICKED
 * alone only fires once, on release, which is the wrong shape for a
 * press/release pair) -- the GNOME/KDE ports need the analogous plumbing
 * for the same reason, just through GtkGestureClick/QAbstractButton's own
 * pressed()/released() signals instead.
 *
 * The disc is a small custom window class ("IntvDiscWidget") snapping to 8
 * positions, matching the fix already made to the GNOME port's own disc
 * and to intv_disc_from_stick (core/src/gamepad_sdl.c): dy is negated
 * before computing the angle because Win32 client coordinates put y
 * increasing DOWNWARD, while intv_host.h's disc_codes numbering (0=E,
 * 4=N, 8=W, 12=S) is defined in ordinary compass/math terms where "up" is
 * a lower y. SetCapture/ReleaseCapture keep receiving mouse-move messages
 * while dragging outside the widget's own bounds.
 *
 * FOCUS: like the GNOME/KDE keypad windows, this one forwards keyboard
 * input to the session rather than letting it fall on the floor. Win32
 * child controls (BUTTON, the custom disc class) take keyboard focus on
 * click by default, so WM_KEYDOWN/WM_KEYUP arrive at the child rather than
 * at the main window -- but each of those child procs is already
 * subclassed here for its own mouse handling, so they forward the key
 * messages from the same place (see key_forward.h) and no session-wide
 * keyboard hook is needed after all.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include "key_forward.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DISC_SIZE 150
#define DISC_DEADZONE_FRAC 0.22

/* Column width every row in build_controller() is centered within (the
 * title label and the disc already span it) -- matches the GNOME port's
 * own gtk_widget_set_halign(..., GTK_ALIGN_CENTER) on each row. */
#define PANEL_W 220
#define DIGITS_W (2 * 52 + 48)
#define ACTIONS_W (2 * 72 + 68)

typedef struct {
    intvsession *session;
    intvsession_pad_side side;
    intvsession_key key;
} key_binding;

typedef struct {
    intvsession *session;
    intvsession_pad_side side;
    int direction; /* -1 = centered, else 0-15 (0 = East, clockwise) */
    int held;
} disc_state;

static HWND g_win;

/* See this file's header for the dy-negation and 8-way-snap reasoning --
 * identical to frontends/gnome/keypad/keypad_window.c's own
 * direction_from_point after its own fix. */
static int direction_from_point(double dx, double dy, double radius)
{
    double dist = sqrt(dx * dx + dy * dy);
    double angle_deg;
    int dir;

    if (dist < radius * DISC_DEADZONE_FRAC)
        return -1;

    angle_deg = atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
    if (angle_deg < 0)
        angle_deg += 360.0;
    dir = (int)floor(angle_deg / 45.0 + 0.5) % 8;
    if (dir < 0)
        dir += 8;
    return dir * 2;
}

/* ---- disc window class ------------------------------------------------- */

static void disc_set_direction(HWND hwnd, disc_state *s, int dir)
{
    if (dir == s->direction)
        return;
    s->direction = dir;
    intvsession_pad_disc(s->session, s->side, dir);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void disc_paint(HWND hwnd, disc_state *s)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT c;
    int cx, cy, r;
    HBRUSH bg, hi, center;
    HPEN pen, oldPen;
    HBRUSH oldBrush;
    int i;

    GetClientRect(hwnd, &c);
    cx = c.right / 2;
    cy = c.bottom / 2;
    r = (c.right < c.bottom ? c.right : c.bottom) / 2 - 4;

    bg = CreateSolidBrush(RGB(38, 38, 43));
    hi = CreateSolidBrush(RGB(77, 140, 230));
    center = CreateSolidBrush(RGB(64, 64, 71));
    pen = CreatePen(PS_SOLID, 1, RGB(115, 115, 128));

    FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));

    oldBrush = (HBRUSH)SelectObject(hdc, bg);
    oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

    if (s->direction >= 0) {
        /* GDI's Pie() sweeps counter-clockwise from the (xr1,yr1) radial
         * to (xr2,yr2), in the same y-down client coordinate system this
         * window already uses -- so the two radial points are computed
         * with a negated sin term to match the disc's own East-clockwise
         * compass convention (see direction_from_point's own comment). */
        double a0 = (s->direction * 22.5 - 22.5) * 3.14159265358979323846 /
                    180.0;
        double a1 = (s->direction * 22.5 + 22.5) * 3.14159265358979323846 /
                    180.0;
        SelectObject(hdc, hi);
        Pie(hdc, cx - r, cy - r, cx + r, cy + r,
           (int)(cx + r * cos(a1)), (int)(cy - r * sin(a1)),
           (int)(cx + r * cos(a0)), (int)(cy - r * sin(a0)));
    }

    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    SelectObject(hdc, pen);
    for (i = 0; i < 8; i++) {
        double a = (i * 45.0 - 22.5) * 3.14159265358979323846 / 180.0;
        MoveToEx(hdc, cx, cy, NULL);
        LineTo(hdc, (int)(cx + r * cos(a)), (int)(cy - r * sin(a)));
    }
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

    SelectObject(hdc, center);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, (int)(cx - r * DISC_DEADZONE_FRAC),
           (int)(cy - r * DISC_DEADZONE_FRAC),
           (int)(cx + r * DISC_DEADZONE_FRAC),
           (int)(cy + r * DISC_DEADZONE_FRAC));

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bg);
    DeleteObject(hi);
    DeleteObject(center);
    DeleteObject(pen);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK disc_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    disc_state *s = (disc_state *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    /* Same reason as key_btn_proc: this custom control takes focus when
     * clicked, so keystrokes land here (see key_forward.h). */
    if (intv_forward_key_msg(msg, wp, lp))
        return 0;

    switch (msg) {
    case WM_PAINT:
        if (s) disc_paint(hwnd, s);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (s) {
            double r = DISC_SIZE / 2.0;
            s->held = 1;
            SetCapture(hwnd);
            disc_set_direction(hwnd, s,
                               direction_from_point(
                                   (double)(short)LOWORD(lp) - r,
                                   (double)(short)HIWORD(lp) - r, r));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s && s->held) {
            double r = DISC_SIZE / 2.0;
            disc_set_direction(hwnd, s,
                               direction_from_point(
                                   (double)(short)LOWORD(lp) - r,
                                   (double)(short)HIWORD(lp) - r, r));
        }
        return 0;
    case WM_LBUTTONUP:
        if (s) {
            s->held = 0;
            ReleaseCapture();
            disc_set_direction(hwnd, s, -1);
        }
        return 0;
    case WM_DESTROY:
        free(s);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- key buttons --------------------------------------------------------- */

static WNDPROC g_btn_proc;

static LRESULT CALLBACK key_btn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    key_binding *kb = (key_binding *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    /* A clicked BUTTON keeps the focus, so keystrokes arrive here rather
     * than at the main window -- forward them instead of swallowing them
     * (see key_forward.h). Claiming the message also stops the BUTTON's own
     * default handling from treating Space/Enter as "press me". */
    if (intv_forward_key_msg(msg, wp, lp))
        return 0;

    if (kb) {
        if (msg == WM_LBUTTONDOWN) {
            SetCapture(hwnd);
            intvsession_pad_key(kb->session, kb->side, kb->key, 1);
        } else if (msg == WM_LBUTTONUP) {
            ReleaseCapture();
            intvsession_pad_key(kb->session, kb->side, kb->key, 0);
        } else if (msg == WM_DESTROY) {
            free(kb);
        }
    }
    return CallWindowProcA(g_btn_proc, hwnd, msg, wp, lp);
}

static HWND make_key(HWND parent, HINSTANCE inst, const char *text,
                     intvsession *session, intvsession_pad_side side,
                     intvsession_key key)
{
    HWND btn = CreateWindowExA(0, "BUTTON", text,
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
                               48, 32, parent, NULL, inst, NULL);
    key_binding *kb = (key_binding *)calloc(1, sizeof(*kb));
    kb->session = session;
    kb->side = side;
    kb->key = key;
    SetWindowLongPtrA(btn, GWLP_USERDATA, (LONG_PTR)kb);
    g_btn_proc = (WNDPROC)SetWindowLongPtrA(btn, GWLP_WNDPROC,
                                            (LONG_PTR)key_btn_proc);
    return btn;
}

/* ---- controller panel ------------------------------------------------------ */

static const struct { const char *label; intvsession_key key; } kDigits[12] =
{
    {"1", INTVSESSION_KEY_1}, {"2", INTVSESSION_KEY_2},
    {"3", INTVSESSION_KEY_3}, {"4", INTVSESSION_KEY_4},
    {"5", INTVSESSION_KEY_5}, {"6", INTVSESSION_KEY_6},
    {"7", INTVSESSION_KEY_7}, {"8", INTVSESSION_KEY_8},
    {"9", INTVSESSION_KEY_9}, {"Clear", INTVSESSION_KEY_CLEAR},
    {"0", INTVSESSION_KEY_0}, {"Enter", INTVSESSION_KEY_ENTER},
};

static void build_controller(HWND parent, HINSTANCE inst, intvsession *session,
                             intvsession_pad_side side, int ox, int oy,
                             const char *title)
{
    HWND label;
    HWND disc;
    disc_state *s;
    int i;

    const int digits_x = ox + (PANEL_W - DIGITS_W) / 2;
    const int actions_x = ox + (PANEL_W - ACTIONS_W) / 2;

    label = CreateWindowExA(0, "STATIC", title, WS_CHILD | WS_VISIBLE | SS_CENTER,
                            ox, oy, PANEL_W, 20, parent, NULL, inst, NULL);
    (void)label;

    for (i = 0; i < 12; i++) {
        HWND k = make_key(parent, inst, kDigits[i].label, session, side,
                          kDigits[i].key);
        MoveWindow(k, digits_x + (i % 3) * 52, oy + 26 + (i / 3) * 36, 48, 32,
                  TRUE);
    }

    MoveWindow(make_key(parent, inst, "Top", session, side,
                        INTVSESSION_ACTION_TOP),
              actions_x, oy + 26 + 4 * 36 + 8, 68, 28, TRUE);
    MoveWindow(make_key(parent, inst, "L-Lower", session, side,
                        INTVSESSION_ACTION_LOWER_LEFT),
              actions_x + 72, oy + 26 + 4 * 36 + 8, 68, 28, TRUE);
    MoveWindow(make_key(parent, inst, "R-Lower", session, side,
                        INTVSESSION_ACTION_LOWER_RIGHT),
              actions_x + 144, oy + 26 + 4 * 36 + 8, 68, 28, TRUE);

    disc = CreateWindowExA(0, "IntvDiscWidget", "", WS_CHILD | WS_VISIBLE,
                           ox + (PANEL_W - DISC_SIZE) / 2,
                           oy + 26 + 4 * 36 + 44, DISC_SIZE, DISC_SIZE, parent,
                           NULL, inst, NULL);
    s = (disc_state *)calloc(1, sizeof(*s));
    s->session = session;
    s->side = side;
    s->direction = -1;
    SetWindowLongPtrA(disc, GWLP_USERDATA, (LONG_PTR)s);
}

/* ---- top-level window -------------------------------------------------------- */

static LRESULT CALLBACK keypad_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
    if (intv_forward_key_msg(msg, wp, lp))
        return 0;

    switch (msg) {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0; /* singleton: hide and reuse, matching the GNOME port */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void intv_keypad_window_toggle(HWND parent, intvsession *session)
{
    static int registered;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    if (g_win) {
        if (IsWindowVisible(g_win)) {
            ShowWindow(g_win, SW_HIDE);
        } else {
            ShowWindow(g_win, SW_SHOW);
            SetForegroundWindow(g_win);
        }
        return;
    }

    if (!registered) {
        WNDCLASSA wc, dc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = keypad_wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "IntvKeypadWindow";
        RegisterClassA(&wc);

        memset(&dc, 0, sizeof(dc));
        dc.lpfnWndProc = disc_proc;
        dc.hInstance = inst;
        dc.hCursor = LoadCursor(NULL, IDC_ARROW);
        dc.lpszClassName = "IntvDiscWidget";
        RegisterClassA(&dc);

        registered = 1;
    }

    g_win = CreateWindowExA(0, "IntvKeypadWindow", "Keypad", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 480, 420, parent,
                            NULL, inst, NULL);

    build_controller(g_win, inst, session, INTVSESSION_PAD_LEFT, 8, 8,
                     "Left Controller");
    build_controller(g_win, inst, session, INTVSESSION_PAD_RIGHT, 240, 8,
                     "Right Controller");

    ShowWindow(g_win, SW_SHOW);
}
