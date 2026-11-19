/*
 * The Win32 Controllers window (F9): two Intellivision II handsets (four when
 * the ECS is on, the extra pair captioned ECS 1 and ECS 2) over the console's
 * front strip, drawn from the session's layout (intvsession_controls_layout)
 * so every frontend shows the same parts. Under them: the system buttons
 * (Reset to CONFIG, Stop in debugger), one row per gamepad with the controller
 * it drives, and the Map row.
 *
 * MOUSE: a press on a key or a side tab holds it for as long as the mouse
 * button is down; the disc is dragged, the pointer's sector being the
 * direction (intvsession_pointer_set); RESET fires on release. The mouse is
 * captured on press; letting go anywhere, losing the capture, hiding, Map
 * being armed or deactivating the window releases whatever the pointer holds
 * (intvsession_release_pointer) -- a pad's holds are the pad's own.
 *
 * What the machine sees held -- keyboard, gamepads and this window together --
 * is polled every 40 ms from the session's input sources and drawn in the held
 * colour, so the window doubles as an input tester. A keypad press also
 * grounds disc and button lines on the real hardware; the lights come from the
 * sources, never from the decoded pad byte, so they stay honest.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode nor a long gamepad name changes it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a key, a side tab, a disc
 * sector or a system button to pick the target, then press a host key OR a
 * gamepad button; Esc cancels. While armed the controls show what drives
 * them, the picked target is lit and the rest are drawn at 40%, and typing
 * does not reach the machine. Clear takes the picked target's key and button
 * away. "Show bindings" shows the same legends outside Map mode. F9-F12 are
 * the app's and cannot be bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

#include <math.h>

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../accent_button.h"
#include "../debugger/dbg_window.h"
#include "../help/help_window.h"
#include "../key_forward.h"
#include "../win_common.h"
#include "gdi_art.h"

#define PANEL_CLASS   "FujiNetGoIntvControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; scaled() maps them to the window's. */
#define UNIT96      17      /* pixels per layout unit */
#define MARGIN      12
#define CASE_PAD    10      /* the mat round the art */
#define NOTE_H      34
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define ROW_GAP     3
#define PAD_ROWS    3       /* gamepad rows the window has room for */
#define SEG_GAP     4
#define BTN_H       28
#define HINT_H      20
#define SIDE_COUNT  4

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };
enum { SYS_CONFIG, SYS_STOP, SYS_COUNT };
enum { MAP_IDLE, MAP_PICK, MAP_WAIT };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    unsigned buttons[SIDE_COUNT];   /* intvsession_buttons_held */
    int disc[SIDE_COUNT];           /* intvsession_disc_held, -1 for none */
    int flash;                      /* the console RESET is pressed with the mouse */
    int ecs_on;
} held_state;

static HWND g_panel, g_parent;
static intvsession *g_session;
static int g_dpi = 96;
static int g_ecs_on;

static int g_map_phase = MAP_IDLE;
static intvsession_key_mapping g_map_target;
static int g_show_bindings;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];
static char g_hover[220];
static int g_tracking;

/* The layout (static table owned boxy the core), and where it sits in the client. */
static const intvsession_control *g_layout;
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds: a pressed part (layout index) and the disc sector. */
static int g_mouse_part = -1;
static int g_pointer_disc = -2;

static held_state g_shown;
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_mat_rc, g_note_rc, g_sys_head_rc, g_sys_rc[SYS_COUNT];
static RECT g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_digit, g_word, g_tiny, g_caption, g_wordmark, g_script;

static void refresh_held(void);

static int scaled(int val) { return intv_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_digit, &g_word, &g_tiny, &g_caption, &g_wordmark, &g_script };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = intv_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = intv_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_digit = gdi_face_font(g_dpi, 14, FW_BOLD, 0);     /* the keypad's digits */
    g_word = gdi_face_font(g_dpi, 10, FW_BOLD, 0);      /* clear, enter */
    g_tiny = intv_make_font(g_dpi, 9, FW_NORMAL, NULL); /* binding legends */
    g_caption = gdi_face_font(g_dpi, 12, FW_BOLD, 0);   /* Player 1 on the mat */
    g_wordmark = gdi_face_font(g_dpi, 12, FW_BOLD, 1);  /* INTELLIVISION II */
    g_script = gdi_face_font(g_dpi, 10, FW_BOLD, 1);    /* MATTEL ELECTRONICS, the tab letters */
}

/* ---- colours ----------------------------------------------------------------- */

#define HX(val) gdi_rgb(val)

#define COL_HELD        HX(INTVSESSION_HELD_RGB)
#define COL_MAT_TOP     HX(0x8E897C)    /* the desk: light enough that black plastic reads */
#define COL_MAT_BOT     HX(0x6B675C)
#define COL_MAT_EDGE    HX(0x3D3A33)
#define COL_SHADOW      HX(0x59564B)
#define COL_CAPTION     HX(0xFBF8EC)
#define COL_DIM_TO      HX(0x7C776B)    /* unpicked parts fade toward the mat */

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? gdi_mix(col, COL_DIM_TO, 0.6) : col;
}

/* ---- parts ----------------------------------------------------------------- */

/* The pixel rectangle of a layout part. */
static void part_box(const intvsession_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const intvsession_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static int binding_mode(void)
{
    return g_map_phase != MAP_IDLE || g_show_bindings;
}

static int pressable(const intvsession_control *ctl)
{
    return ctl->kind == INTVSESSION_CTL_KEY || ctl->kind == INTVSESSION_CTL_ACTION ||
           ctl->kind == INTVSESSION_CTL_DISC || ctl->kind == INTVSESSION_CTL_BUTTON;
}

static int same_target(intvsession_key_mapping one, intvsession_key_mapping two)
{
    if (one.kind != two.kind) return 0;
    switch (one.kind) {
    case INTVSESSION_MAP_KEY: return one.side == two.side && one.key == two.key;
    case INTVSESSION_MAP_DISC: return one.side == two.side && one.direction == two.direction;
    case INTVSESSION_MAP_SYSACT: return one.sysact == two.sysact;
    default: return 0;
    }
}

/* The disc sector under a client point on a DISC part, -1 in its dead centre. */
static int disc_dir_at(const intvsession_control *ctl, POINT pnt)
{
    return intvsession_controls_disc_dir(ctl, (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* What Map would bind for a click on a part (the disc: the sector clicked). */
static intvsession_key_mapping target_at(const intvsession_control *ctl, POINT pnt)
{
    intvsession_key_mapping none;
    memset(&none, 0, sizeof none);
    if (ctl->kind == INTVSESSION_CTL_DISC) {
        const int dir = disc_dir_at(ctl, pnt);
        return dir >= 0 ? intvsession_target_disc((intvsession_pad_side)ctl->side, dir) : none;
    }
    return intvsession_control_target(ctl);
}

/* The disc sector shown lit on a side: held, or in Map mode the picked one. */
static int disc_lit(int side)
{
    if (g_map_phase == MAP_PICK) return -1;
    if (g_map_phase == MAP_WAIT)
        return g_map_target.kind == INTVSESSION_MAP_DISC && (int)g_map_target.side == side ? g_map_target.direction : -1;
    return g_shown.disc[side];
}

/* A part shown held: what the machine sees. In Map mode the picked target is
 * the one lit instead. */
static int part_lit(const intvsession_control *ctl)
{
    if (g_map_phase == MAP_PICK) return 0;
    if (g_map_phase == MAP_WAIT)
        return ctl->kind != INTVSESSION_CTL_DISC && pressable(ctl) &&
               same_target(intvsession_control_target(ctl), g_map_target);
    if (ctl->kind == INTVSESSION_CTL_BUTTON) return g_shown.flash;
    if (ctl->kind == INTVSESSION_CTL_KEY || ctl->kind == INTVSESSION_CTL_ACTION)
        return (g_shown.buttons[ctl->side] >> ctl->target) & 1u;
    return 0;
}

/* Map mode with a target picked: every other control is drawn at 40%. */
static int part_dim(const intvsession_control *ctl)
{
    if (g_map_phase != MAP_WAIT || !pressable(ctl)) return 0;
    if (ctl->kind == INTVSESSION_CTL_DISC)
        return !(g_map_target.kind == INTVSESSION_MAP_DISC && (int)g_map_target.side == ctl->side);
    return !same_target(intvsession_control_target(ctl), g_map_target);
}

/* A key's name made small enough for a key cap, a side tab or the disc:
 * "Numpad 5" reads "KP 5", "Up Arrow" "Up", "Right Shift" "RSh". The hover
 * line has the full name. */
static void shorten_name(const char *full, char *out, int outsize)
{
    static const struct { const char *from, *to; } rules[] = {
        { "Numpad ", "KP " }, { "Keypad ", "KP " }, { " Arrow", "" }, { "Left ", "L" }, { "Right ", "R" },
        { "Shift", "Sh" }, { "Control", "Ct" }, { "Ctrl", "Ct" }, { "Alt", "Al" },
    };
    char work[96];
    size_t rule;
    snprintf(work, sizeof work, "%s", full);
    for (rule = 0; rule < sizeof rules / sizeof rules[0]; rule++) {
        char *hit = strstr(work, rules[rule].from);
        if (hit) {
            const size_t from_len = strlen(rules[rule].from), to_len = strlen(rules[rule].to);
            memmove(hit + to_len, hit + from_len, strlen(hit + from_len) + 1);
            memcpy(hit, rules[rule].to, to_len);
        }
    }
    snprintf(out, (size_t)outsize, "%s", work);
}

/* What drives a target: its key, shortened. */
static void binding_label(intvsession_key_mapping target, char *out, int outsize)
{
    intvsession_binding bind;
    out[0] = '\0';
    if (target.kind == INTVSESSION_MAP_NONE) return;
    bind = intvsession_target_binding_get(g_session, target);
    if (bind.keysym) {
        char full[96];
        intvsession_keysym_name(bind.keysym, full, sizeof full);
        shorten_name(full, out, outsize);
    }
    if (!out[0]) snprintf(out, (size_t)outsize, "%s", "\xe2\x80\x94");
}

/* ---- the art ------------------------------------------------------------------- */

static void art_handset(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    if (!strcmp(ctl->label, "Cable")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, HX(INTVSESSION_CABLE_RGB), HX(0x2B2B2B), unit * 0.05);
    } else if (!strcmp(ctl->label, "Handset")) {
        gdi_rrect(hdc, boxx + unit * 0.15, boxy + unit * 0.2, boxw, boxh, unit * 0.8, COL_SHADOW, COL_SHADOW, 0);
        gdi_grad_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.8, HX(INTVSESSION_BEVEL_RGB), HX(INTVSESSION_HANDSET_RGB),
                       HX(INTVSESSION_SHADE_RGB), unit * 0.1);
        /* the raised bezel round the keypad */
        gdi_rrect(hdc, boxx + unit * 0.35, boxy + unit * 0.35, boxw - unit * 0.7, boxh - unit * 0.7, unit * 0.6,
                  GDI_NOFILL, HX(INTVSESSION_BEVEL_RGB), unit * 0.08);
    } else if (!strcmp(ctl->label, "Keypad")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.4, HX(INTVSESSION_KEYPAD_RGB), HX(0x3A3733), unit * 0.06);
    } else if (!strcmp(ctl->label, "Card")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, GDI_NOFILL, HX(INTVSESSION_CARD_EDGE_RGB), unit * 0.07);
    } else if (!strcmp(ctl->label, "Ring")) {
        gdi_ellipse(hdc, boxx + boxw / 2, boxy + boxh / 2, boxw / 2, boxh / 2, HX(INTVSESSION_RING_RGB),
                    HX(INTVSESSION_SHADE_RGB), unit * 0.08);
        gdi_ellipse(hdc, boxx + boxw / 2, boxy + boxh / 2, boxw / 2 - unit * 0.12, boxh / 2 - unit * 0.12, GDI_NOFILL,
                    HX(INTVSESSION_BEVEL_RGB), unit * 0.05);
    }
}

static void art_key(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    const int lit = part_lit(ctl), dim = part_dim(ctl);
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.4, lit ? COL_HELD : tone(HX(INTVSESSION_KEYFACE_RGB), dim),
              lit ? COL_HELD : tone(HX(INTVSESSION_KEYRED_RGB), dim), unit * 0.14);
}

static void art_tab(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    const int lit = part_lit(ctl), dim = part_dim(ctl);
    const COLORREF face = tone(HX(INTVSESSION_TAB_RGB), dim);
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, lit ? gdi_mix(face, COL_HELD, 0.45) : face,
              lit ? COL_HELD : tone(HX(INTVSESSION_TAB_EDGE_RGB), dim), lit ? unit * 0.18 : unit * 0.08);
}

/* The black pad with its 16 sectors; a held sector is a wedge from 22% of the
 * radius out (the dead centre) to the rim. Position 0 is East, counter-clockwise. */
static void art_disc(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh, ctrx, ctry, rad;
    const double unit = g_unit;
    const int dim = part_dim(ctl), lit = disc_lit(ctl->side);
    const COLORREF line = tone(HX(INTVSESSION_DISC_RING_RGB), dim);
    int pos;
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    ctrx = boxx + boxw / 2;
    ctry = boxy + boxh / 2;
    rad = boxw / 2;
    gdi_ellipse(hdc, ctrx, ctry, rad, rad, tone(HX(INTVSESSION_DISC_RGB), dim), line, unit * 0.06);
    gdi_ellipse(hdc, ctrx, ctry, rad * 0.8, rad * 0.8, GDI_NOFILL, line, unit * 0.05);
    gdi_ellipse(hdc, ctrx, ctry, rad * 0.55, rad * 0.55, GDI_NOFILL, line, unit * 0.05);
    if (lit >= 0)
        gdi_pie(hdc, ctrx, ctry, rad, lit * 22.5 - 11.25, lit * 22.5 + 11.25, COL_HELD);
    for (pos = 0; pos < 16; pos++) {
        const double ang = (pos * 22.5 + 11.25) * GDI_PI / 180.0;
        gdi_line(hdc, ctrx + rad * 0.22 * cos(ang), ctry - rad * 0.22 * sin(ang), ctrx + rad * cos(ang),
                 ctry - rad * sin(ang), lit >= 0 ? gdi_mix(line, COL_HELD, 0.3) : line, unit * 0.04);
    }
    gdi_ellipse(hdc, ctrx, ctry, rad * 0.22, rad * 0.22, tone(HX(INTVSESSION_DISC_RGB), dim), line, unit * 0.05);
}

static void art_console(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    int slat;
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    if (!strcmp(ctl->label, "Console")) {
        gdi_rrect(hdc, boxx + unit * 0.1, boxy + unit * 0.15, boxw, boxh, unit * 0.5, COL_SHADOW, COL_SHADOW, 0);
        gdi_grad_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.5, HX(INTVSESSION_BEVEL_RGB), HX(INTVSESSION_CONSOLE_RGB),
                       HX(INTVSESSION_SHADE_RGB), unit * 0.08);
    } else if (!strcmp(ctl->label, "Band")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.15, HX(INTVSESSION_BAND_RGB), HX(INTVSESSION_BAND_RGB), 0);
    } else if (!strcmp(ctl->label, "RedLine")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, 0, HX(INTVSESSION_MATTEL_RED_RGB), HX(INTVSESSION_MATTEL_RED_RGB), 0);
    } else if (!strcmp(ctl->label, "Grille")) {
        for (slat = 0; slat < 5; slat++) {
            const double row = boxy + (slat + 0.5) * boxh / 5.0;
            gdi_line(hdc, boxx + unit * 0.1, row, boxx + boxw - unit * 0.1, row, HX(INTVSESSION_GRILLE_RGB), unit * 0.14);
        }
    } else if (!strcmp(ctl->label, "Block")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.25, HX(INTVSESSION_BLOCK_RGB), HX(0x1B1D1F), unit * 0.06);
    } else if (!strcmp(ctl->label, "OnOff")) {
        gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.2, gdi_mix(HX(INTVSESSION_BLOCK_RGB), HX(0), 0.3), GDI_NOFILL, 0);
    } else if (!strcmp(ctl->label, "Led")) {
        gdi_ellipse(hdc, boxx + boxw / 2, boxy + boxh / 2, boxw / 2, boxh / 2, HX(INTVSESSION_LED_RGB), HX(0x3A0A07),
                    unit * 0.05);
    }
}

static void art_reset(HDC hdc, const intvsession_control *ctl)
{
    double boxx, boxy, boxw, boxh;
    const double unit = g_unit;
    const int lit = part_lit(ctl), dim = part_dim(ctl);
    const COLORREF face = gdi_mix(HX(INTVSESSION_BLOCK_RGB), HX(0xFFFFFF), 0.12);
    part_box(ctl, &boxx, &boxy, &boxw, &boxh);
    gdi_rrect(hdc, boxx, boxy, boxw, boxh, unit * 0.3, lit ? gdi_mix(face, COL_HELD, 0.7) : tone(face, dim),
              lit ? COL_HELD : tone(HX(INTVSESSION_BLOCK_LEGEND_RGB), dim), unit * 0.07);
}

/* The mat and every part in the layout, in layout order (so a body is drawn
 * before the parts on it). */
static void art_all(HDC hdc)
{
    int idx;
    gdi_grad_rrect(hdc, g_mat_rc.left, g_mat_rc.top, g_mat_rc.right - g_mat_rc.left, g_mat_rc.bottom - g_mat_rc.top,
                   scaled(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    for (idx = 0; idx < g_numlayout; idx++) {
        const intvsession_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case INTVSESSION_CTL_BODY:
            if (ctl->side >= 0) art_handset(hdc, ctl);
            else art_console(hdc, ctl);
            break;
        case INTVSESSION_CTL_KEY: art_key(hdc, ctl); break;
        case INTVSESSION_CTL_ACTION: art_tab(hdc, ctl); break;
        case INTVSESSION_CTL_DISC: art_disc(hdc, ctl); break;
        case INTVSESSION_CTL_BUTTON: art_reset(hdc, ctl); break;
        default: break;
        }
    }
}

/* ---- the text over the art ------------------------------------------------------------- */

/* The binding legends sit on the part when the bindings are shown. */
static COLORREF legend_color(intvsession_key_mapping target, int dim, COLORREF normal)
{
    return same_target(target, g_map_target) && g_map_phase == MAP_WAIT ? COL_HELD : tone(normal, dim);
}

static void text_key(HDC hdc, const intvsession_control *ctl, const RECT *rect)
{
    const int lit = part_lit(ctl), dim = part_dim(ctl);
    const int word = ctl->label[1] != '\0';     /* clear, enter */
    COLORREF ink = lit ? RGB(0, 0, 0) : tone(HX(INTVSESSION_LEGEND_RGB), dim);
    RECT top = *rect, bottom = *rect;
    SetTextColor(hdc, ink);
    SelectObject(hdc, word ? g_word : g_digit);
    if (!binding_mode()) {
        gdi_text_line(hdc, ctl->label, rect, DT_CENTER);
        return;
    }
    {
        char legend[96];
        const int mid = rect->top + (rect->bottom - rect->top) * 11 / 20;
        top.bottom = mid;
        bottom.top = mid;
        gdi_text_line(hdc, ctl->label, &top, DT_CENTER);
        binding_label(intvsession_control_target(ctl), legend, sizeof legend);
        SelectObject(hdc, g_tiny);
        SetTextColor(hdc, lit ? RGB(0, 0, 0) : legend_color(intvsession_control_target(ctl), dim, HX(0xFFD2C8)));
        gdi_text_line(hdc, legend, &bottom, DT_CENTER | DT_END_ELLIPSIS);
    }
}

static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const intvsession_control *ctl = &g_layout[idx];
        RECT rect;
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case INTVSESSION_CTL_CAPTION:
            if (ctl->side >= 0) {
                SelectObject(hdc, g_caption);
                SetTextColor(hdc, COL_CAPTION);
                gdi_text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else if (!strcmp(ctl->label, "INTELLIVISION II")) {
                SelectObject(hdc, g_wordmark);
                SetTextColor(hdc, HX(INTVSESSION_LEGEND_RGB));
                gdi_text_line(hdc, ctl->label, &rect, DT_LEFT);
            } else {
                SelectObject(hdc, g_script);
                SetTextColor(hdc, HX(INTVSESSION_MATTEL_RED_RGB));
                gdi_text_line(hdc, ctl->label, &rect, DT_RIGHT);
            }
            break;
        case INTVSESSION_CTL_BODY:
            if (!strcmp(ctl->label, "OnOff")) {
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, gdi_mix(HX(INTVSESSION_BLOCK_LEGEND_RGB), HX(0), 0.35));
                gdi_text_line(hdc, "ON/OFF", &rect, DT_CENTER);
            }
            break;
        case INTVSESSION_CTL_KEY:
            text_key(hdc, ctl, &rect);
            break;
        case INTVSESSION_CTL_ACTION: {
            const intvsession_key_mapping target = intvsession_control_target(ctl);
            const int dim = part_dim(ctl);
            RECT face = rect;
            SelectObject(hdc, g_tiny);
            if (binding_mode()) {
                char legend[96];
                binding_label(target, legend, sizeof legend);
                SetTextColor(hdc, legend_color(target, dim, HX(INTVSESSION_TAB_LEGEND_RGB)));
                gdi_text_line(hdc, legend, &face, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                SelectObject(hdc, g_script);
                SetTextColor(hdc, tone(HX(INTVSESSION_TAB_LEGEND_RGB), dim));
                gdi_text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        case INTVSESSION_CTL_DISC:
            if (binding_mode()) {
                /* the four compass points' keys, on the pad */
                static const int dirs[4] = { 8, 12, 4, 0 };    /* W S N E */
                const int midx = (rect.left + rect.right) / 2, midy = (rect.top + rect.bottom) / 2;
                const double reach = (rect.right - rect.left) / 2 * 0.64;
                int slot;
                SelectObject(hdc, g_tiny);
                for (slot = 0; slot < 4; slot++) {
                    const intvsession_key_mapping target =
                        intvsession_target_disc((intvsession_pad_side)ctl->side, dirs[slot]);
                    const double ang = dirs[slot] * 22.5 * GDI_PI / 180.0;
                    const int half = scaled(14), postx = midx + (int)(reach * cos(ang)),
                              posty = midy - (int)(reach * sin(ang));
                    RECT box;
                    char legend[96];
                    SetRect(&box, postx - half, posty - scaled(7), postx + half, posty + scaled(7));
                    binding_label(target, legend, sizeof legend);
                    SetTextColor(hdc, legend_color(target, part_dim(ctl), RGB(0xFF, 0xFF, 0xFF)));
                    gdi_text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
                }
            }
            break;
        case INTVSESSION_CTL_BUTTON: {
            const intvsession_key_mapping target = intvsession_control_target(ctl);
            const int dim = part_dim(ctl);
            RECT face = rect;
            SelectObject(hdc, g_tiny);
            if (binding_mode()) {
                char legend[96];
                binding_label(target, legend, sizeof legend);
                SetTextColor(hdc, legend_color(target, dim, RGB(0xFF, 0xFF, 0xFF)));
                gdi_text_line(hdc, legend, &face, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                SetTextColor(hdc, part_lit(ctl) ? RGB(0, 0, 0) : tone(HX(INTVSESSION_LEGEND_RGB), dim));
                gdi_text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the strips: note, system buttons, gamepads, Map, hint ----------------------------- */

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_phase == MAP_IDLE ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

static intvsession_key_mapping sys_target(int btn)
{
    return intvsession_target_sysaction(btn == SYS_CONFIG ? INTVSESSION_SYSACT_RESET_CONFIG
                                                           : INTVSESSION_SYSACT_DEBUG_STOP);
}

static const char *sys_label(int btn)
{
    return intvsession_sysaction_name(btn == SYS_CONFIG ? INTVSESSION_SYSACT_RESET_CONFIG
                                                         : INTVSESSION_SYSACT_DEBUG_STOP);
}

static void paint_system(HDC hdc)
{
    int btn;
    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    SetBkMode(hdc, TRANSPARENT);
    gdi_text_line(hdc, "System buttons", &g_sys_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SYS_COUNT; btn++) {
        const intvsession_key_mapping target = sys_target(btn);
        const int picked = g_map_phase == MAP_WAIT && same_target(target, g_map_target);
        const int dim = g_map_phase == MAP_WAIT && !picked;
        RECT rect = g_sys_rc[btn];
        gdi_button_face(hdc, &rect, picked, dim, INTV_ACCENT_COLORREF, INTV_ACCENT_INK_COLORREF);
        if (binding_mode()) {
            char legend[96];
            RECT upper = rect, lower = rect;
            upper.bottom = lower.top = (rect.top + rect.bottom) / 2;
            binding_label(target, legend, sizeof legend);
            SelectObject(hdc, g_tiny);
            gdi_text_line(hdc, sys_label(btn), &upper, DT_CENTER);
            gdi_text_line(hdc, legend, &lower, DT_CENTER | DT_END_ELLIPSIS);
            SelectObject(hdc, g_font);
        } else {
            gdi_text_line(hdc, sys_label(btn), &rect, DT_CENTER);
        }
    }
}

/* The choice at the right of a gamepad's row: segment 0 is Automatic, n the
 * controller n - 1 (Left, Right, then ECS Left and ECS Right with the ECS on). */
static int segment_count(void)
{
    return g_ecs_on ? 5 : 3;
}

static int segment_width(int seg)
{
    static const int widths[5] = { 76, 52, 56, 76, 84 };
    return scaled(widths[seg]);
}

static const char *segment_label(int seg)
{
    static const char *const labels[5] = { "Automatic", "Left", "Right", "ECS Left", "ECS Right" };
    return labels[seg];
}

static void segment_rect(const RECT *row, int seg, RECT *rect)
{
    const int gap = scaled(SEG_GAP);
    int total = 0, left, idx;
    for (idx = 0; idx < segment_count(); idx++) total += segment_width(idx) + (idx ? gap : 0);
    left = row->right - total;
    for (idx = 0; idx < seg; idx++) left += segment_width(idx) + gap;
    SetRect(rect, left, row->top, left + segment_width(seg), row->bottom - 1);
}

/* One line per gamepad: its name and the controller it drives now, then the
 * Automatic / Left / Right (/ ECS Left / ECS Right) choice, the current one in
 * the accent. */
static void paint_gamepads(HDC hdc)
{
    const int num = intvsession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    gdi_text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        gdi_text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        gdi_text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = intvsession_gamepad_assignment(g_session, idx);
        const int effective = intvsession_gamepad_effective_side(g_session, idx);
        const int chosen = assign < 0 || assign >= segment_count() - 1 ? 0 : assign + 1;
        char name[128], line[220];
        RECT name_rc, first;

        segment_rect(&g_gp_row_rc[idx], 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - scaled(6);
        intvsession_gamepad_name(g_session, idx, name, sizeof name);
        if (effective >= 0)
            snprintf(line, sizeof line, "%s  (%s)", name, intvsession_pad_side_name((intvsession_pad_side)effective));
        else
            snprintf(line, sizeof line, "%s  (no controller)", name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        gdi_text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        for (seg = 0; seg < segment_count(); seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[idx], seg, &rect);
            gdi_button_face(hdc, &rect, seg == chosen, 0, INTV_ACCENT_COLORREF, INTV_ACCENT_INK_COLORREF);
            gdi_text_line(hdc, segment_label(seg), &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(POINT pnt, char *out, int outsize)
{
    const intvsession_control *ctl;
    intvsession_key_mapping target;
    intvsession_binding bind;
    char key[64], name[96];
    int part;
    out[0] = '\0';
    if (!g_numlayout) return;
    part = intvsession_controls_hit(g_ecs_on, (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
    if (part < 0) return;
    ctl = &g_layout[part];
    target = target_at(ctl, pnt);
    if (target.kind == INTVSESSION_MAP_NONE) {
        if (ctl->kind == INTVSESSION_CTL_DISC)
            snprintf(out, (size_t)outsize, "%s disc: press and drag to push it",
                     intvsession_pad_side_name((intvsession_pad_side)ctl->side));
        return;
    }
    bind = intvsession_target_binding_get(g_session, target);
    key[0] = '\0';
    if (bind.keysym) intvsession_keysym_name(bind.keysym, key, sizeof key);
    intvsession_target_name(target, name, sizeof name);
    snprintf(out, (size_t)outsize, "%s   Key: %s   Gamepad: %s", name, key[0] ? key : "none",
             bind.button != INTVSESSION_PAD_BTN_NONE ? intvsession_pad_button_name(bind.button) : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    SetBkMode(hdc, TRANSPARENT);
    {
        RECT note = g_note_rc;
        gdi_text_out(hdc, "The action buttons are crossed boxy design: the right-hand modifier keys drive the LEFT "
                          "controller.", &note, DT_LEFT | DT_WORDBREAK | DT_TOP);
    }
    paint_system(hdc);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_phase != MAP_IDLE) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_phase != MAP_WAIT;
        RECT rect = g_btn_rc[btn];
        SelectObject(hdc, g_font);
        gdi_button_face(hdc, &rect, armed, inactive, INTV_ACCENT_COLORREF, INTV_ACCENT_INK_COLORREF);
        gdi_text_line(hdc, strip_label(btn), &rect, DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    gdi_text_line(hdc, g_map_phase == MAP_IDLE && g_hover[0] ? g_hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the art supersampled, its text, the strips. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_mat_rc);
    if (!IsRectEmpty(&art)) {
        gdi_paint_art(hdc, &art, GetSysColor(COLOR_BTNFACE), art_all);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ------------------------------------------------------------------- */

static void invalidate_art(void)
{
    if (g_panel) InvalidateRect(g_panel, &g_mat_rc, FALSE);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). */
static void mouse_release(void)
{
    g_mouse_part = -1;
    g_pointer_disc = -2;
    if (g_session) intvsession_release_pointer(g_session);
    if (g_panel) {
        refresh_held();
        invalidate_art();
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover[0] = '\0';
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a key or a side button to hold it; drag the disc. Map rebinds."

/* Idle, armed (waiting for a target) or waiting for a key or pad button for
 * `target`. */
static void set_map(int phase, intvsession_key_mapping target)
{
    g_map_phase = phase;
    g_map_target = target;
    if (phase != MAP_IDLE) {
        mouse_release();                        /* a held part must not outlive Map being armed */
        intvsession_release_all(g_session);     /* Map swallows key-ups, so drop keyboard holds too */
    }
    if (phase != MAP_WAIT) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        intvsession_gamepad_capture_cancel(g_session);
    }
    if (phase == MAP_IDLE) {
        set_hint(IDLE_HINT);
    } else if (phase == MAP_PICK) {
        set_hint("Click a key, a side button, a disc direction or a system button to remap it. Esc finishes.");
    } else {
        char msg[200], name[96];
        intvsession_target_name(target, name, sizeof name);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", name);
        set_hint(msg);
        intvsession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void map_idle(void)
{
    intvsession_key_mapping none;
    memset(&none, 0, sizeof none);
    set_map(MAP_IDLE, none);
}

static void map_pick(void)
{
    intvsession_key_mapping none;
    memset(&none, 0, sizeof none);
    set_map(MAP_PICK, none);
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(intvsession_key_mapping target, const char *what, const char *was)
{
    char msg[300], name[96];
    intvsession_target_name(target, name, sizeof name);
    if (was && was[0]) snprintf(msg, sizeof msg, "Bound %s to %s (was %s)", name, what, was);
    else snprintf(msg, sizeof msg, "Bound %s to %s", name, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static void fetch_layout(void)
{
    g_ecs_on = intvsession_ecs_on(g_session) != 0;
    g_layout = intvsession_controls_layout(g_ecs_on, &g_numlayout, &g_layw, &g_layh);
}

/* The mat with the handsets and the console in it, the note, the system
 * buttons, the gamepad rows and the Map row: the window is this size at this
 * DPI, always (for this ECS setting). */
static void layout_static(void)
{
    static const int widths[SB_COUNT] = { 56, 56, 76, 108, 56 };
    static const int sys_widths[SYS_COUNT] = { 124, 134 };
    const int left = scaled(MARGIN);
    int ypos = scaled(MARGIN), idx, xpos, content_w;

    fetch_layout();
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * scaled(CASE_PAD);
    g_total_w = 2 * left + content_w;

    g_orgx = left + scaled(CASE_PAD);
    g_orgy = ypos + scaled(CASE_PAD);
    SetRect(&g_mat_rc, left, ypos, left + content_w, (int)ceil(g_orgy + (double)g_layh * g_unit) + scaled(CASE_PAD));
    ypos = g_mat_rc.bottom + scaled(4);

    SetRect(&g_note_rc, left, ypos, left + content_w, ypos + scaled(NOTE_H));
    ypos += scaled(NOTE_H) + scaled(4);

    SetRect(&g_sys_head_rc, left, ypos, left + scaled(104), ypos + scaled(BTN_H));
    xpos = left + scaled(108);
    for (idx = 0; idx < SYS_COUNT; idx++) {
        SetRect(&g_sys_rc[idx], xpos, ypos, xpos + scaled(sys_widths[idx]), ypos + scaled(BTN_H));
        xpos += scaled(sys_widths[idx]) + scaled(6);
    }
    ypos += scaled(BTN_H) + scaled(10);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + scaled(HEAD_H));
    ypos += scaled(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + scaled(ROW_H));
        ypos += scaled(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += scaled(6);

    xpos = left;
    for (idx = 0; idx < SB_HELP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + scaled(widths[idx]), ypos + scaled(BTN_H));
        xpos += scaled(widths[idx]) + scaled(6);
    }
    SetRect(&g_btn_rc[SB_HELP], left + content_w - scaled(widths[SB_HELP]), ypos, left + content_w,
            ypos + scaled(BTN_H));
    ypos += scaled(BTN_H) + scaled(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + scaled(HINT_H));
    g_total_h = ypos + scaled(HINT_H) + scaled(MARGIN);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    memset(&g_shown, 0, sizeof g_shown);
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    if (g_panel) {
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* The window's outer size for the laid-out client. */
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

/* ---- polling -------------------------------------------------------------------- */

/* What the machine sees held on each side: redrawn when any of it changed. The
 * ECS option only changes with a restart; the layout follows it here. */
static void refresh_held(void)
{
    held_state now;
    int side;
    if (!g_panel || !g_session) return;
    if ((intvsession_ecs_on(g_session) != 0) != g_ecs_on) {
        rebuild();
        return;
    }
    memset(&now, 0, sizeof now);
    for (side = 0; side < SIDE_COUNT; side++) {
        now.buttons[side] = intvsession_buttons_held(g_session, (intvsession_pad_side)side);
        now.disc[side] = intvsession_disc_held(g_session, (intvsession_pad_side)side);
    }
    now.flash = g_mouse_part >= 0 && g_mouse_part < g_numlayout && g_layout[g_mouse_part].kind == INTVSESSION_CTL_BUTTON;
    now.ecs_on = g_ecs_on;
    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        g_shown = now;
        invalidate_art();
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    if (!g_session) return;
    memset(&now, 0, sizeof now);
    now.generation = intvsession_gamepad_generation(g_session);
    now.count = intvsession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = intvsession_gamepad_assignment(g_session, idx);
        now.effective[idx] = intvsession_gamepad_effective_side(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        if (g_panel) InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ----------------------------------------------------------------------- */

static void hide_panel(void)
{
    map_idle();
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* The app's own keys, never the machine's or Map's. */
static int app_key(WPARAM vk)
{
    return vk == VK_F9 || vk == VK_F10 || vk == VK_F11 || vk == VK_F12;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9-F12,
 * which are the app's). */
static int map_key(UINT msg, WPARAM vk, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_phase == MAP_IDLE) return 0;
    if (app_key(vk)) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 0;       /* Ctrl chords are the app's */
    if (!down || (lparam & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        if (g_map_phase == MAP_WAIT) map_pick();
        else map_idle();
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_phase == MAP_WAIT) {
        const intvsession_key_mapping target = g_map_target;
        const uint32_t keysym = intv_keysym_from_msg(vk, lparam);
        char was[128], name[48];
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (intvsession_key_reserved(keysym)) {
            set_hint("F9, F10, F11 and F12 are the app's and cannot be bound.");
            g_swallow_vk = vk;
            return 1;
        }
        was[0] = '\0';
        intvsession_target_set_key(g_session, target, keysym, was, sizeof was);
        intvsession_keysym_name(keysym, name, sizeof name);
        g_swallow_vk = vk;
        map_pick();                     /* stay armed: remapping several in a row is normal */
        bound_hint(target, name, was);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. Anything Map does not take goes to
 * the main window's key path, so a key plays exactly as typed there (and
 * honours keyboard_mode). */
static int panel_key(UINT msg, WPARAM vk, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (map_key(msg, vk, lparam)) return 1;
    if (vk == VK_F9) {
        if (down && !(lparam & (1 << 30))) hide_panel();
        return 1;
    }
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (g_parent) {
        SendMessageA(g_parent, msg, vk, lparam);
        return 1;
    }
    return 0;
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_numlayout) return -1;
    return intvsession_controls_hit(g_ecs_on, (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

static void fire_system(intvsession_sysaction action)
{
    if (intvsession_sysaction_fire(g_session, action) != 0)
        intv_message(g_panel, intvsession_last_error(g_session), "FujiNet Go Intv", MB_ICONWARNING);
    else if (action == INTVSESSION_SYSACT_DEBUG_STOP)
        intv_debugger_show(g_parent, g_session);
}

/* Where the pointer is on the disc, as the direction the machine is given. */
static void disc_follow(int part, POINT pnt)
{
    const intvsession_control *ctl = &g_layout[part];
    const int dir = disc_dir_at(ctl, pnt);
    if (dir != g_pointer_disc) {
        g_pointer_disc = dir;
        intvsession_pointer_set(g_session, (intvsession_pad_side)ctl->side, -1, dir);
    }
    refresh_held();
    invalidate_art();
}

static void strip_click(HWND hwnd, int btn)
{
    switch (btn) {
    case SB_MAP:
        if (g_map_phase == MAP_IDLE) map_pick();
        else map_idle();
        break;
    case SB_CLEAR:
        if (g_map_phase == MAP_WAIT) {
            intvsession_binding_clear(g_session, g_map_target);
            set_hint("Cleared. Press a key or a gamepad button to bind it again");
            intvsession_gamepad_capture_begin(g_session);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case SB_DEFAULTS: {
        const int count = intvsession_gamepad_count(g_session);
        int pad;
        intvsession_bindings_reset(g_session);
        for (pad = 0; pad < count; pad++) intvsession_gamepad_assign(g_session, pad, -1);
        refresh_pads();
        set_hint("Every key and button is back to its default");
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case SB_SHOW:
        g_show_bindings = !g_show_bindings;
        intvsession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    default: intv_help_show(hwnd, "controllers"); break;
    }
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover[0]) {
        g_hover[0] = '\0';
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++)
        if (PtInRect(&g_btn_rc[btn], pnt)) { strip_click(hwnd, btn); return; }
    for (btn = 0; btn < SYS_COUNT; btn++)
        if (PtInRect(&g_sys_rc[btn], pnt)) {
            if (g_map_phase != MAP_IDLE) set_map(MAP_WAIT, sys_target(btn));
            else fire_system(btn == SYS_CONFIG ? INTVSESSION_SYSACT_RESET_CONFIG : INTVSESSION_SYSACT_DEBUG_STOP);
            return;
        }
    for (row = 0; row < PAD_ROWS && row < intvsession_gamepad_count(g_session); row++)
        for (seg = 0; seg < segment_count(); seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[row], seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), then the controller in side order */
                intvsession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const intvsession_control *ctl = &g_layout[part];
        if (g_map_phase != MAP_IDLE) {
            /* armed: picks (or changes) the target; on the disc, the sector clicked */
            const intvsession_key_mapping target = target_at(ctl, pnt);
            if (target.kind != INTVSESSION_MAP_NONE) set_map(MAP_WAIT, target);
            return;
        }
        mouse_release();
        g_mouse_part = part;
        if (ctl->kind == INTVSESSION_CTL_DISC)
            disc_follow(part, pnt);
        else if (ctl->kind != INTVSESSION_CTL_BUTTON)
            intvsession_pointer_set(g_session, (intvsession_pad_side)ctl->side, ctl->target, -1);
        SetCapture(hwnd);
        refresh_held();
        invalidate_art();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_part >= 0) {
        if (g_layout[g_mouse_part].kind == INTVSESSION_CTL_DISC) {
            disc_follow(g_mouse_part, pnt);         /* a disc is dragged, never "dragged off" */
            return;
        }
        /* dragged off what it holds: let go, as a button does */
        if (part_at(pnt) != g_mouse_part) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        char text[sizeof g_hover];
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        hover_text(pnt, text, sizeof text);
        if (strcmp(text, g_hover) != 0) {
            snprintf(g_hover, sizeof g_hover, "%s", text);
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd, POINT pnt)
{
    if (g_mouse_part >= 0) {
        const intvsession_control *ctl = &g_layout[g_mouse_part];
        const int fires = ctl->kind == INTVSESSION_CTL_BUTTON && part_at(pnt) == g_mouse_part;
        const intvsession_sysaction action = (intvsession_sysaction)ctl->target;
        mouse_release();
        if (GetCapture() == hwnd) ReleaseCapture();
        if (fires) fire_system(action);
        return;
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
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
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover[0]) {
            g_hover[0] = '\0';
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_up(hwnd, pnt);
        return 0;
    }
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd && g_mouse_part >= 0) mouse_release();
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            intvsession_pad_button button;
            if (g_map_phase != MAP_WAIT) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (intvsession_gamepad_capture_poll(g_session, &button)) {
                char was[128];
                const intvsession_key_mapping target = g_map_target;
                was[0] = '\0';
                intvsession_target_set_button(g_session, target, button, was, sizeof was);
                map_pick();                 /* stay armed: remapping several in a row is normal */
                bound_hint(target, intvsession_pad_button_name(button), was);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the machine's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_ENTERMENULOOP:
        intvsession_release_all(g_session);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release();                /* the pointer's holds go */
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;               /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                intvsession_release_all(g_session);
        }
        break;                              /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
            mouse_release();
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
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
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        map_idle();
        g_panel = NULL;
        g_numlayout = 0;
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

    g_show_bindings = intvsession_get_int(g_session, "controllers_show_bindings", 0) > 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
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
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers (F9)", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    intv_help_set_context(g_panel, "controllers", 0);    /* F1 may be a bound key */
    rescale(intv_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    g_hover[0] = '\0';
    map_idle();
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void intv_controller_window_toggle(HWND parent, intvsession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) hide_panel();
    else show_panel();
}

void intv_controller_window_show(HWND parent, intvsession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (!IsWindowVisible(g_panel)) show_panel();
    else SetForegroundWindow(g_panel);
}

int intv_controller_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void intv_controller_window_gamepads_changed(void)
{
    if (!g_panel) return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
