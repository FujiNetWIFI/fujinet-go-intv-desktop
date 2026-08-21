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
 * The disc is a small custom window class ("IntvDiscWidget") offering all
 * 16 positions the hardware has, marking all 16 sectors, and lighting
 * exactly the one under the pointer. Hit testing is
 * intvsession_disc_from_point (core/src/disc_geom.c), shared with the
 * GNOME/KDE/macOS keypad windows, and the painting follows the same
 * step-by-step recipe they do. SetCapture/ReleaseCapture keep receiving
 * mouse-move messages while dragging outside the widget's own bounds.
 *
 * FOCUS: like the GNOME/KDE keypad windows, this one forwards keyboard
 * input to the session rather than letting it fall on the floor. Win32
 * child controls (BUTTON, the custom disc class) take keyboard focus on
 * click by default, so WM_KEYDOWN/WM_KEYUP arrive at whichever child last
 * took focus rather than at the top-level window -- and Win32 does not
 * bubble those messages back up to the parent on its own. Rather than
 * subclassing every child (which a control added later could easily
 * forget -- the Map/Reset buttons below did, until keyboard remapping
 * turned out to be unreachable through them), intv_keypad_pretranslate is
 * called from the frontend's own message pump (see key_forward.h and
 * main.c) for every message whose window belongs to this one, keyed off
 * GetAncestor(..., GA_ROOT) rather than which child currently has focus.
 *
 * MAP MODE: a bottom row (map_select_target and friends, near the end of
 * this file) lets a click on any of the 15 digit/action buttons above, any
 * of the 16 disc segments, OR either of the System row's two buttons
 * (Reset Game / Reset to CONFIG, build_system_row -- machine-global system
 * actions, not per-side), be rebound to a different keyboard key or gamepad
 * button, through core/src/bindings.c's remappable layer -- see
 * intvsession.h's own "remappable bindings" section for the contract. While
 * armed, map_intercept_key_msg (checked ahead of intv_forward_key_msg in
 * intv_keypad_pretranslate below) swallows the next keystroke for the
 * mapping instead of letting it reach the machine, and key_btn_proc's/
 * disc_proc's own WM_LBUTTONDOWN handling (or the System row's own
 * WM_COMMAND/BN_CLICKED, see keypad_wnd_proc) picks the map target instead
 * of injecting a pad key/disc direction/system action. Highlight is
 * BM_SETSTATE (the native "pressed" look, driven programmatically, rather
 * than an owner-draw custom style) on the Map button and whichever keypad
 * button is the current target, or an amber wedge on the disc when the
 * target is a disc segment. A gamepad button press is polled for on a
 * short WM_TIMER (IDT_GAMEPAD_POLL below) since core/src/gamepad_sdl.c's
 * capture result lands on its own background thread, not this window's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include "key_forward.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISC_SIZE 150
#define DISC_PI 3.14159265358979323846

/* Segments the highlighted sector's arc edge is drawn with -- see
 * disc_paint. */
#define DISC_WEDGE_STEPS 6

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
    int direction;   /* -1 = centered, else 0-15 (0 = East, clockwise) */
    int map_target;  /* -1 = not the map target, else 0-15: the sector Map
                      * mode has armed on THIS disc -- distinct from
                      * `direction`, which is the live position actually
                      * being sent to the machine. */
    int held;
    HWND hwnd;        /* this disc's own window, for map_highlight's
                       * InvalidateRect -- the state doesn't otherwise carry
                       * it (disc_proc looks itself up via GWLP_USERDATA). */
} disc_state;

static HWND g_win;

/* ---- Map mode (see this file's own header) --------------------------------
 * File-static, like g_win above: this window is a singleton, so there is
 * never more than one map sequence in flight. */
typedef enum {
    MAP_IDLE = 0,       /* normal play */
    MAP_PICK_TARGET,    /* Map armed, waiting for a keypad button click */
    MAP_WAIT_INPUT,     /* target picked, waiting for a key/pad press */
} map_state_t;

#define IDC_KEYPAD_MAP    2001
#define IDC_KEYPAD_RESET  2002
/* One id per intvsession_sysaction, IDC_KEYPAD_SYSACT_BASE + a -- see
 * build_system_row and keypad_wnd_proc's own WM_COMMAND handling. */
#define IDC_KEYPAD_SYSACT_BASE 2003
#define IDT_GAMEPAD_POLL 1

static map_state_t g_map_state = MAP_IDLE;
static intvsession_key_mapping g_map_target;
static intvsession *g_map_session;
static HWND g_map_btn;
static HWND g_status_label;
/* [side][key] -- only INTVSESSION_PAD_LEFT/_RIGHT are ever populated, the
 * two panels this window builds. */
static HWND g_key_buttons[2][INTVSESSION_KEY_COUNT];
/* Indexed by intvsession_sysaction -- see build_system_row. */
static HWND g_sysact_buttons[INTVSESSION_SYSACT_COUNT];
/* [side] -- same two sides, this window's own disc_state* for each panel's
 * disc, a direct pointer rather than another GWLP_USERDATA lookup, safe
 * because the disc windows live for the whole process (this window hides
 * rather than destroys itself). */
static disc_state *g_discs[2];

/* BM_SETSTATE forces a BUTTON control's native "pressed" rendering without
 * changing any check state or simulating a click -- exactly the
 * programmatic highlight this needs, with no owner-draw plumbing. */
static void set_highlighted(HWND btn, int on)
{
    if (!btn)
        return;
    SendMessageA(btn, BM_SETSTATE, on ? TRUE : FALSE, 0);
}

static void map_set_status(const char *text)
{
    if (g_status_label)
        SetWindowTextA(g_status_label, text ? text : "");
}

/* Highlights (or clears) whichever target g_map_target currently names,
 * MAP_KEY/MAP_DISC/MAP_SYSACT alike -- every set_highlighted(g_key_buttons
 * [...]) call site below that used to be scoped to a (side,key) pair now
 * goes through this instead, so Map mode doesn't need parallel code paths
 * per kind. */
static void map_highlight(int on)
{
    if (g_map_target.kind == INTVSESSION_MAP_KEY) {
        set_highlighted(g_key_buttons[g_map_target.side][g_map_target.key],
                        on);
    } else if (g_map_target.kind == INTVSESSION_MAP_DISC) {
        disc_state *d = g_discs[g_map_target.side];
        if (d) {
            d->map_target = on ? g_map_target.direction : -1;
            if (d->hwnd)
                InvalidateRect(d->hwnd, NULL, FALSE);
        }
    } else if (g_map_target.kind == INTVSESSION_MAP_SYSACT) {
        set_highlighted(g_sysact_buttons[g_map_target.sysact], on);
    }
}

/* Back to MAP_IDLE from any state, undoing whatever highlight is showing
 * and disarming gamepad capture -- shared by Map-to-abort, Reset, and the
 * window being hidden/closed mid-sequence. */
static void map_reset_ui(void)
{
    if (g_map_state == MAP_WAIT_INPUT)
        map_highlight(0);
    set_highlighted(g_map_btn, 0);
    g_map_state = MAP_IDLE;
    map_set_status("");
    if (g_map_session)
        intvsession_gamepad_capture_cancel(g_map_session);
    if (g_win)
        KillTimer(g_win, IDT_GAMEPAD_POLL);
}

static void map_select_target(intvsession_key_mapping target)
{
    char desc[128], status[256];
    intvsession_binding b = intvsession_target_binding_get(g_map_session,
                                                            target);

    g_map_target = target;
    g_map_state = MAP_WAIT_INPUT;
    map_highlight(1);
    intvsession_binding_describe(b, desc, sizeof(desc));
    snprintf(status, sizeof(status),
            "Currently mapped to %s. Press target key or gamepad button, "
            "or press MAP to abort.",
            desc);
    map_set_status(status);
    intvsession_gamepad_capture_begin(g_map_session);
    if (g_win)
        SetTimer(g_win, IDT_GAMEPAD_POLL, 66 /* ~15Hz */, NULL);
}

/* Completes the mapping, keyboard or gamepad half alike -- both land here
 * once bindings.c has already made the change, just with a different verb
 * for the status line. */
static void map_finish(const char *bound_to, const char *stolen)
{
    char target[128], status[256];

    intvsession_target_name(g_map_target, target, sizeof(target));
    if (stolen && stolen[0])
        snprintf(status, sizeof(status), "%s mapped to %s (taken from %s)",
                target, bound_to, stolen);
    else
        snprintf(status, sizeof(status), "%s mapped to %s", target, bound_to);

    map_highlight(0);
    set_highlighted(g_map_btn, 0);
    g_map_state = MAP_IDLE;
    if (g_win)
        KillTimer(g_win, IDT_GAMEPAD_POLL);
    map_set_status(status);
}

static void map_complete_key(uint32_t keysym)
{
    char stolen[128], namebuf[64];

    intvsession_target_set_key(g_map_session, g_map_target, keysym, stolen,
                               sizeof(stolen));
    intvsession_gamepad_capture_cancel(g_map_session);
    intvsession_keysym_name(keysym, namebuf, sizeof(namebuf));
    map_finish(namebuf, stolen);
}

static void map_complete_button(intvsession_pad_button button)
{
    char stolen[128], boundto[64];

    intvsession_target_set_button(g_map_session, g_map_target, button, stolen,
                                  sizeof(stolen));
    snprintf(boundto, sizeof(boundto), "Gamepad %s",
             intvsession_pad_button_name(button));
    map_finish(boundto, stolen);
}

/* Consumes WM_KEYDOWN/UP/SYSKEYDOWN/UP while Map mode wants the *next*
 * keyboard press for itself instead of letting it reach the machine -- same
 * contract as intv_forward_key_msg (see key_forward.h), so every proc below
 * just OR's the two checks together, this one first. */
static int map_intercept_key_msg(UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_map_state == MAP_IDLE)
        return 0;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_map_state == MAP_WAIT_INPUT) {
            uint32_t keysym = intv_keysym_from_msg(wp, lp);
            if (keysym)
                map_complete_key(keysym);
        }
        return 1;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return 1;
    default:
        return 0;
    }
}

/* ---- disc window class -------------------------------------------------
 * Angles below are ordinary compass/math degrees (0 = East, growing
 * counter-clockwise), converted to y-DOWN client coordinates at the point
 * of use: cos for x, MINUS sin for y. */
static POINT disc_point(int cx, int cy, double radius, double deg)
{
    double a = deg * DISC_PI / 180.0;
    POINT p;
    p.x = (LONG)floor(cx + radius * cos(a) + 0.5);
    p.y = (LONG)floor(cy - radius * sin(a) + 0.5);
    return p;
}

static void disc_set_direction(HWND hwnd, disc_state *s, int dir)
{
    if (dir == s->direction)
        return;
    s->direction = dir;
    intvsession_pad_disc(s->session, s->side, dir);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Fills the 22.5-degree wedge centred on `dir` (0-15) into hdc, whatever
 * brush is currently selected -- shared by the live direction and the
 * Map-mode target, which disc_paint below selects a different brush for
 * before each call. Drawn as a filled Polygon sampled along the arc rather
 * than with Pie(): every toolkit in this project has its own answer to
 * which way an arc sweeps in a y-down surface, and GDI's is the murkiest of
 * the four -- its arc direction is documented as counter-clockwise, but
 * relative to a logical coordinate system whose y axis points the other
 * way from the screen's, so which of the two radials is the start is a
 * coin toss that decides between a 22.5-degree wedge and a 337.5-degree
 * one. A polygon has no such convention. At r ~= 71px a 3.75-degree chord
 * bulges 0.04px inside the true arc. */
static void disc_fill_wedge(HDC hdc, int cx, int cy, int r, int dir)
{
    double centre_deg = dir * INTVSESSION_DISC_SECTOR_DEG;
    POINT wedge[DISC_WEDGE_STEPS + 2];
    int i;

    wedge[0].x = cx;
    wedge[0].y = cy;
    for (i = 0; i <= DISC_WEDGE_STEPS; i++) {
        double a = centre_deg - INTVSESSION_DISC_SECTOR_DEG / 2.0 +
                   INTVSESSION_DISC_SECTOR_DEG * i / DISC_WEDGE_STEPS;
        wedge[i + 1] = disc_point(cx, cy, r, a);
    }
    Polygon(hdc, wedge, DISC_WEDGE_STEPS + 2);
}

static void disc_paint(HWND hwnd, disc_state *s)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT c;
    int cx, cy, r, hub;
    HBRUSH bg, hi, target, center;
    HPEN pen, oldPen;
    HBRUSH oldBrush;
    int i;

    GetClientRect(hwnd, &c);
    cx = c.right / 2;
    cy = c.bottom / 2;
    r = (c.right < c.bottom ? c.right : c.bottom) / 2 - 4;
    hub = (int)(r * INTVSESSION_DISC_DEADZONE_FRAC);

    bg = CreateSolidBrush(RGB(38, 38, 43));
    hi = CreateSolidBrush(RGB(77, 140, 230));
    target = CreateSolidBrush(RGB(242, 166, 38));
    center = CreateSolidBrush(RGB(64, 64, 71));
    pen = CreatePen(PS_SOLID, 1, RGB(115, 115, 128));

    /* The corners outside the disc take the window's own face colour, as
     * they do on GNOME/KDE/macOS (this used to paint them black, which is
     * why the Windows keypad had a black square behind each disc). */
    FillRect(hdc, &c, (HBRUSH)(COLOR_BTNFACE + 1));

    oldBrush = (HBRUSH)SelectObject(hdc, bg);
    oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

    /* The Map-mode target sector, if this disc has one armed -- drawn
     * before the live wedge below so the live wedge (which Map mode's own
     * press guard prevents from ever landing on the same sector at the
     * same time, but the ordering is cheap insurance either way) stays on
     * top. */
    if (s->map_target >= 0) {
        SelectObject(hdc, target);
        disc_fill_wedge(hdc, cx, cy, r, s->map_target);
    }

    /* The held sector: exactly the 22.5 degrees centred on the direction
     * intvsession_disc_from_point returned, so the lit wedge is always the
     * one bounded by the two spokes the pointer is between. */
    if (s->direction >= 0) {
        SelectObject(hdc, hi);
        disc_fill_wedge(hdc, cx, cy, r, s->direction);
    }

    /* One spoke per sector boundary -- 16 of them, at the half-way angles
     * BETWEEN the 16 positions, so each position gets a visible sector of
     * its own to aim at. */
    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    SelectObject(hdc, pen);
    for (i = 0; i < INTVSESSION_DISC_POSITIONS; i++) {
        double a = i * INTVSESSION_DISC_SECTOR_DEG -
                   INTVSESSION_DISC_SECTOR_DEG / 2.0;
        POINT inner = disc_point(cx, cy, hub, a);
        POINT outer = disc_point(cx, cy, r, a);
        MoveToEx(hdc, inner.x, inner.y, NULL);
        LineTo(hdc, outer.x, outer.y);
    }
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

    SelectObject(hdc, center);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, cx - hub, cy - hub, cx + hub, cy + hub);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bg);
    DeleteObject(hi);
    DeleteObject(target);
    DeleteObject(center);
    DeleteObject(pen);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK disc_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    disc_state *s = (disc_state *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT:
        if (s) disc_paint(hwnd, s);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (s) {
            /* Map mode's disc half: the same PICK_TARGET/WAIT_INPUT guard
             * key_btn_proc's own WM_LBUTTONDOWN has (see this file's own
             * header). A click in the dead hub (intvsession_disc_from_point
             * returning -1) picks nothing and leaves Map mode armed, the
             * same as clicking the gap between two keypad buttons would;
             * SetCapture/s->held are never reached on that path. */
            double r = DISC_SIZE / 2.0;
            if (g_map_state == MAP_PICK_TARGET) {
                int dir = intvsession_disc_from_point(
                    (double)(short)LOWORD(lp) - r,
                    (double)(short)HIWORD(lp) - r, r);
                if (dir >= 0)
                    map_select_target(intvsession_target_disc(s->side, dir));
                return 0;
            }
            if (g_map_state == MAP_WAIT_INPUT)
                return 0;
            s->held = 1;
            SetCapture(hwnd);
            disc_set_direction(hwnd, s,
                               intvsession_disc_from_point(
                                   (double)(short)LOWORD(lp) - r,
                                   (double)(short)HIWORD(lp) - r, r));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s && s->held && g_map_state == MAP_IDLE) {
            double r = DISC_SIZE / 2.0;
            disc_set_direction(hwnd, s,
                               intvsession_disc_from_point(
                                   (double)(short)LOWORD(lp) - r,
                                   (double)(short)HIWORD(lp) - r, r));
        }
        return 0;
    case WM_LBUTTONUP:
        if (s && g_map_state == MAP_IDLE) {
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

/* WM_LBUTTONDOWN/UP are intercepted for Map mode too (see this file's own
 * header): PICK_TARGET turns a press into "select this button as the map
 * target" instead of an injection (and returns without reaching the
 * default BUTTON proc at all, so it never shows the normal pressed-look --
 * only map_select_target's own set_highlighted should), and WAIT_INPUT
 * swallows clicks outright -- at that point the window wants a *keyboard
 * or gamepad* press, not another mouse click reinterpreted as one. */
static LRESULT CALLBACK key_btn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    key_binding *kb = (key_binding *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    if (kb) {
        if (msg == WM_LBUTTONDOWN) {
            if (g_map_state == MAP_PICK_TARGET) {
                map_select_target(intvsession_target_key(kb->side, kb->key));
                return 0;
            }
            if (g_map_state == MAP_WAIT_INPUT)
                return 0;
            SetCapture(hwnd);
            intvsession_pad_key(kb->session, kb->side, kb->key, 1);
        } else if (msg == WM_LBUTTONUP) {
            if (g_map_state != MAP_IDLE)
                return 0;
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
    g_key_buttons[side][key] = btn;
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
    s->map_target = -1;
    s->hwnd = disc;
    SetWindowLongPtrA(disc, GWLP_USERDATA, (LONG_PTR)s);
    /* Only LEFT/RIGHT panels are ever built (see g_discs' own comment). */
    if (side == INTVSESSION_PAD_LEFT || side == INTVSESSION_PAD_RIGHT)
        g_discs[side] = s;
}

/* The System row beneath both controller panels, which end at y=372 (see
 * build_controller's own arithmetic: oy=8, plus 26 title + 4*36 digit rows
 * + 8 + 28 action row + 44 + DISC_SIZE(150) disc = 8+26+144+8+28+44+150).
 * Reset Game / Reset to CONFIG -- machine-global system actions, distinct
 * from the Map row's own "Reset Bindings" below, and themselves Map-mode
 * targets like every keypad digit/action button and disc segment. Plain
 * BS_PUSHBUTTON controls routed through keypad_wnd_proc's own WM_COMMAND
 * (IDC_KEYPAD_SYSACT_BASE + a) -- NOT make_key, which subclasses with
 * key_btn_proc for a held press/release pair a sysaction doesn't want, and
 * would also clobber the file-static g_btn_proc it shares with every keypad
 * digit button. */
static void build_system_row(HWND parent, HINSTANCE inst)
{
    int a;
    int x = 170 - (INTVSESSION_SYSACT_COUNT - 1) * 20; /* roughly centered */

    for (a = 0; a < INTVSESSION_SYSACT_COUNT; a++) {
        HWND btn = CreateWindowExA(
            0, "BUTTON", intvsession_sysaction_name((intvsession_sysaction)a),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, 380, 120, 28, parent,
            (HMENU)(INT_PTR)(IDC_KEYPAD_SYSACT_BASE + a), inst, NULL);
        g_sysact_buttons[a] = btn;
        x += 128;
    }
}

/* The Map/Reset row beneath the System row (y=416, see build_system_row's
 * own arithmetic: 380 + 28 button height + 8 gap). See this file's own
 * header for the state machine map_select_target/map_complete_* and
 * keypad_wnd_proc's WM_COMMAND handling implement together. */
static void build_map_row(HWND parent, HINSTANCE inst)
{
    g_map_btn = CreateWindowExA(0, "BUTTON", "Map",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170,
                                416, 80, 28, parent,
                                (HMENU)(INT_PTR)IDC_KEYPAD_MAP, inst, NULL);
    CreateWindowExA(0, "BUTTON", "Reset Bindings",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 258, 416, 140, 28,
                    parent, (HMENU)(INT_PTR)IDC_KEYPAD_RESET, inst, NULL);
    g_status_label =
        CreateWindowExA(0, "STATIC", "",
                        WS_CHILD | WS_VISIBLE | SS_CENTER, 8, 452, 464, 48,
                        parent, NULL, inst, NULL);
}

/* ---- top-level window -------------------------------------------------------- */

static LRESULT CALLBACK keypad_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            int id = LOWORD(wp);
            if (id == IDC_KEYPAD_MAP) {
                if (g_map_state != MAP_IDLE) {
                    map_reset_ui();
                } else {
                    g_map_state = MAP_PICK_TARGET;
                    set_highlighted(g_map_btn, 1);
                    map_set_status("Press button to map");
                }
            } else if (id == IDC_KEYPAD_RESET) {
                map_reset_ui();
                if (g_map_session) {
                    intvsession_bindings_reset(g_map_session);
                    map_set_status("Bindings reset to default");
                }
            } else if (id >= IDC_KEYPAD_SYSACT_BASE &&
                      id < IDC_KEYPAD_SYSACT_BASE + INTVSESSION_SYSACT_COUNT) {
                /* Reset Game / Reset to CONFIG -- see build_system_row's own
                 * header. Three Map states, same as every keypad digit/
                 * disc segment (key_btn_proc/disc_proc): PICK_TARGET picks
                 * this as the map target, WAIT_INPUT swallows the click
                 * (waiting for a key/pad press, not another click), IDLE
                 * fires it. */
                intvsession_sysaction a =
                    (intvsession_sysaction)(id - IDC_KEYPAD_SYSACT_BASE);
                if (g_map_state == MAP_PICK_TARGET) {
                    map_select_target(intvsession_target_sysaction(a));
                } else if (g_map_state == MAP_IDLE && g_map_session) {
                    if (intvsession_sysaction_fire(g_map_session, a) != 0)
                        map_set_status(intvsession_last_error(g_map_session));
                }
            }
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_GAMEPAD_POLL) {
            intvsession_pad_button button;
            if (g_map_state == MAP_WAIT_INPUT && g_map_session &&
                intvsession_gamepad_capture_poll(g_map_session, &button))
                map_complete_button(button);
        }
        return 0;
    case WM_CLOSE:
        /* A Map sequence left armed behind a hidden window would still be
         * capturing gamepad input on the next show -- abort it, same as
         * clicking Map again to cancel. */
        map_reset_ui();
        ShowWindow(hwnd, SW_HIDE);
        return 0; /* singleton: hide and reuse, matching the GNOME port */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int intv_keypad_pretranslate(MSG *msg)
{
    if (!g_win || GetAncestor(msg->hwnd, GA_ROOT) != g_win)
        return 0;
    if (map_intercept_key_msg(msg->message, msg->wParam, msg->lParam))
        return 1;
    return intv_forward_key_msg(msg->message, msg->wParam, msg->lParam);
}

void intv_keypad_window_toggle(HWND parent, intvsession *session)
{
    static int registered;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    if (g_win) {
        if (IsWindowVisible(g_win)) {
            map_reset_ui(); /* see keypad_wnd_proc's WM_CLOSE comment */
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

    g_map_session = session;

    g_win = CreateWindowExA(0, "IntvKeypadWindow", "Keypad", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 480, 600, parent,
                            NULL, inst, NULL);

    build_controller(g_win, inst, session, INTVSESSION_PAD_LEFT, 8, 8,
                     "Left Controller");
    build_controller(g_win, inst, session, INTVSESSION_PAD_RIGHT, 240, 8,
                     "Right Controller");
    build_system_row(g_win, inst);
    build_map_row(g_win, inst);

    ShowWindow(g_win, SW_SHOW);
}
