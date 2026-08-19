/*
 * EcsKeyboardWindow (Win32) -- an on-screen ECS keyboard: 48 buttons
 * covering every key of the ECS's own 7x8 scan-matrix keyboard (see
 * core/jzintv/intv_host.h's intv_ecs_key), each driving
 * intvsession_ecs_key_set directly. Independent of the "keyboard_mode"
 * setting (main.c's on_key) -- these on-screen buttons work whether or not
 * the physical keyboard is currently routed to the ECS, so opening this
 * window never silently changes that setting.
 *
 * Ordinary keys are plain BUTTON controls subclassed to catch
 * WM_LBUTTONDOWN/WM_LBUTTONUP directly, exactly like keypad_window.c's own
 * key buttons: a real ECS key is held for as long as it's down.
 * Shift/Ctrl are BS_AUTOCHECKBOX|BS_PUSHLIKE buttons instead -- a mouse
 * can't hold two buttons down to chord a Shifted letter the way a hand
 * can, so they latch (toggled via BN_CLICKED, routed through this
 * window's own WM_COMMAND), and intv_host_ecs_key's OR-in-a-bit chording
 * (core/jzintv/intv_host.c) does the rest.
 *
 * FOCUS: like keypad_window.c, this window forwards keyboard input to the
 * session -- see that file's own FOCUS note. Here it goes through
 * intv_forward_ecs_key, which routes to the ECS matrix regardless of the
 * "keyboard_mode" setting, matching this window's on-screen buttons above
 * and the GNOME and KDE ports' equivalents.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ecskbd_window.h"

#include "key_forward.h"

#include <stdlib.h>
#include <string.h>

#define WIN_W 660
#define WIN_H 300

static HWND g_win;
static HWND g_notice;
static intvsession *g_session;

/* ---- ordinary keys (subclassed BUTTON, press/release) -------------------- */

typedef struct {
    intvsession *session;
    intvsession_ecs_key key;
} key_binding;

static WNDPROC g_btn_proc;

static LRESULT CALLBACK key_btn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    key_binding *kb = (key_binding *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    /* A clicked BUTTON keeps the focus, so keystrokes arrive here rather
     * than at the main window -- forward them instead of swallowing them
     * (see key_forward.h). Claiming the message also stops the BUTTON's own
     * default handling from treating Space/Enter as "press me". */
    if (intv_forward_ecs_key_msg(msg, wp, lp))
        return 0;

    if (kb) {
        if (msg == WM_LBUTTONDOWN) {
            SetCapture(hwnd);
            intvsession_ecs_key_set(kb->session, kb->key, 1);
        } else if (msg == WM_LBUTTONUP) {
            ReleaseCapture();
            intvsession_ecs_key_set(kb->session, kb->key, 0);
        } else if (msg == WM_DESTROY) {
            free(kb);
        }
    }
    return CallWindowProcA(g_btn_proc, hwnd, msg, wp, lp);
}

static HWND make_key(HWND parent, HINSTANCE inst, const char *text,
                     intvsession *session, intvsession_ecs_key key,
                     int x, int y, int w)
{
    HWND btn = CreateWindowExA(0, "BUTTON", text,
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w,
                               32, parent, NULL, inst, NULL);
    key_binding *kb = (key_binding *)calloc(1, sizeof(*kb));
    kb->session = session;
    kb->key = key;
    SetWindowLongPtrA(btn, GWLP_USERDATA, (LONG_PTR)kb);
    g_btn_proc = (WNDPROC)SetWindowLongPtrA(btn, GWLP_WNDPROC,
                                            (LONG_PTR)key_btn_proc);
    return btn;
}

/* ---- Shift/Ctrl (latching -- see file header) ---------------------------- */

#define IDC_ECSKBD_CTRL  1001
#define IDC_ECSKBD_SHIFT 1002

static HWND make_mod_key(HWND parent, HINSTANCE inst, const char *text,
                         int ctrl_id, int x, int y, int w)
{
    return CreateWindowExA(0, "BUTTON", text,
                           WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX |
                               BS_PUSHLIKE,
                           x, y, w, 32, parent, (HMENU)(INT_PTR)ctrl_id, inst,
                           NULL);
}

/* ---- layout ---------------------------------------------------------------
 * Four QWERTY-ish rows plus a function row, covering all 48 keys of
 * intv_ecs_key exactly once. See core/jzintv/intv_host.h for the row/mask
 * this ultimately maps to. */

typedef struct { const char *label; intvsession_ecs_key key; } ecs_key_label;

static const ecs_key_label kRowDigits[10] = {
    {"1", INTVSESSION_ECS_KEY_1}, {"2", INTVSESSION_ECS_KEY_2},
    {"3", INTVSESSION_ECS_KEY_3}, {"4", INTVSESSION_ECS_KEY_4},
    {"5", INTVSESSION_ECS_KEY_5}, {"6", INTVSESSION_ECS_KEY_6},
    {"7", INTVSESSION_ECS_KEY_7}, {"8", INTVSESSION_ECS_KEY_8},
    {"9", INTVSESSION_ECS_KEY_9}, {"0", INTVSESSION_ECS_KEY_0},
};
static const ecs_key_label kRowQwerty[10] = {
    {"Q", INTVSESSION_ECS_KEY_Q}, {"W", INTVSESSION_ECS_KEY_W},
    {"E", INTVSESSION_ECS_KEY_E}, {"R", INTVSESSION_ECS_KEY_R},
    {"T", INTVSESSION_ECS_KEY_T}, {"Y", INTVSESSION_ECS_KEY_Y},
    {"U", INTVSESSION_ECS_KEY_U}, {"I", INTVSESSION_ECS_KEY_I},
    {"O", INTVSESSION_ECS_KEY_O}, {"P", INTVSESSION_ECS_KEY_P},
};
static const ecs_key_label kRowAsdf[10] = {
    {"A", INTVSESSION_ECS_KEY_A}, {"S", INTVSESSION_ECS_KEY_S},
    {"D", INTVSESSION_ECS_KEY_D}, {"F", INTVSESSION_ECS_KEY_F},
    {"G", INTVSESSION_ECS_KEY_G}, {"H", INTVSESSION_ECS_KEY_H},
    {"J", INTVSESSION_ECS_KEY_J}, {"K", INTVSESSION_ECS_KEY_K},
    {"L", INTVSESSION_ECS_KEY_L}, {";", INTVSESSION_ECS_KEY_SEMI},
};
static const ecs_key_label kRowZxcv[10] = {
    {"Z", INTVSESSION_ECS_KEY_Z}, {"X", INTVSESSION_ECS_KEY_X},
    {"C", INTVSESSION_ECS_KEY_C}, {"V", INTVSESSION_ECS_KEY_V},
    {"B", INTVSESSION_ECS_KEY_B}, {"N", INTVSESSION_ECS_KEY_N},
    {"M", INTVSESSION_ECS_KEY_M}, {",", INTVSESSION_ECS_KEY_COMMA},
    {".", INTVSESSION_ECS_KEY_PERIOD}, {"<-", INTVSESSION_ECS_KEY_LEFT},
};

static void add_row(HWND parent, HINSTANCE inst, intvsession *session,
                    const ecs_key_label *row, int count, int y)
{
    const int key_w = 40, gap = 4;
    const int total = count * key_w + (count - 1) * gap;
    int x = (WIN_W - total) / 2;
    int i;
    for (i = 0; i < count; i++) {
        make_key(parent, inst, row[i].label, session, row[i].key, x, y,
                key_w);
        x += key_w + gap;
    }
}

static void build_keyboard(HWND parent, HINSTANCE inst, intvsession *session)
{
    add_row(parent, inst, session, kRowDigits, 10, 30);
    add_row(parent, inst, session, kRowQwerty, 10, 66);
    add_row(parent, inst, session, kRowAsdf, 10, 102);
    add_row(parent, inst, session, kRowZxcv, 10, 138);

    {
        /* Esc(56) Ctrl(64) Shift(64) Space(160) Up(40) Down(40) Right(40)
         * Enter(56), 4px gaps between the 8 -- see file header. */
        int x = (WIN_W - (56 + 64 + 64 + 160 + 40 + 40 + 40 + 56 + 7 * 4)) / 2;
        const int y = 182;

        make_key(parent, inst, "Esc", session, INTVSESSION_ECS_KEY_ESC, x, y,
                56);
        x += 56 + 4;
        make_mod_key(parent, inst, "Ctrl", IDC_ECSKBD_CTRL, x, y, 64);
        x += 64 + 4;
        make_mod_key(parent, inst, "Shift", IDC_ECSKBD_SHIFT, x, y, 64);
        x += 64 + 4;
        make_key(parent, inst, "Space", session, INTVSESSION_ECS_KEY_SPACE, x,
                y, 160);
        x += 160 + 4;
        make_key(parent, inst, "Up", session, INTVSESSION_ECS_KEY_UP, x, y,
                40);
        x += 40 + 4;
        make_key(parent, inst, "Down", session, INTVSESSION_ECS_KEY_DOWN, x,
                y, 40);
        x += 40 + 4;
        make_key(parent, inst, "Right", session, INTVSESSION_ECS_KEY_RIGHT, x,
                y, 40);
        x += 40 + 4;
        make_key(parent, inst, "Enter", session, INTVSESSION_ECS_KEY_ENTER, x,
                y, 56);
    }
}

/* ---- top-level window ------------------------------------------------------ */

static void release_all(void)
{
    if (g_session)
        intvsession_ecs_keys_clear(g_session);
}

static void update_notice(intvsession *session)
{
    int show = !intvsession_has_ecs_rom(session) ||
              intvsession_get_int(session, "ecs", INTVSESSION_HW_AUTO) ==
                  INTVSESSION_HW_OFF;
    ShowWindow(g_notice, show ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK ecskbd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
    if (intv_forward_ecs_key_msg(msg, wp, lp))
        return 0;

    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            int id = LOWORD(wp);
            if (id == IDC_ECSKBD_CTRL || id == IDC_ECSKBD_SHIFT) {
                HWND ctl = (HWND)lp;
                int checked =
                    SendMessageA(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
                intvsession_ecs_key_set(
                    g_session,
                    id == IDC_ECSKBD_CTRL ? INTVSESSION_ECS_KEY_CTRL
                                          : INTVSESSION_ECS_KEY_SHIFT,
                    checked);
            }
        }
        return 0;
    case WM_CLOSE:
        release_all();
        ShowWindow(hwnd, SW_HIDE);
        return 0; /* singleton: hide and reuse, matching the GNOME port */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void intv_ecskbd_window_toggle(HWND parent, intvsession *session)
{
    static int registered;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    g_session = session;

    if (g_win) {
        if (IsWindowVisible(g_win)) {
            ShowWindow(g_win, SW_HIDE);
            release_all();
        } else {
            update_notice(session);
            ShowWindow(g_win, SW_SHOW);
            SetForegroundWindow(g_win);
        }
        return;
    }

    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = ecskbd_wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "IntvEcsKeyboardWindow";
        RegisterClassA(&wc);
        registered = 1;
    }

    g_win = CreateWindowExA(0, "IntvEcsKeyboardWindow", "ECS Keyboard",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            WIN_W, WIN_H, parent, NULL, inst, NULL);

    g_notice = CreateWindowExA(
        0, "STATIC", "ECS is not enabled -- see Settings to turn it on.",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 8, 4, WIN_W - 16, 20, g_win, NULL,
        inst, NULL);

    build_keyboard(g_win, inst, session);
    update_notice(session);

    ShowWindow(g_win, SW_SHOW);
}
