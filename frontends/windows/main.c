/*
 * FujiNet Go Intv -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE frontends over the shared intvsession API: a
 * GDI-blitted display letterboxed to 4:3, full keyboard mapping, a menu
 * bar, and the FujiNet configuration (default browser) and console-log
 * windows, plus clickable keypad and ECS keyboard windows.
 *
 * Unlike the CoCo/MSX Windows frontends, there is no vsync-feeding present
 * thread here: intvsession has no notify_vsync to feed (jzIntv's own paced
 * thread already governs itself to NTSC/PAL speed independent of any
 * frontend -- see frontends/gnome/display.c's own comment on this), so a
 * plain WM_TIMER drives the repaint poll instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h> /* timeBeginPeriod -- see WinMain's own comment */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvsession.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "ecskbd/ecskbd_window.h"
#include "keypad/keypad_window.h"
#include "resource.h"

static intvsession *g_session;
static HWND g_hwnd;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

static uint32_t g_frame[INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT];
static uint64_t g_serial;
static int g_have_frame;

/* Off-screen buffer the frame is composed into, blitted to the window in one
 * BitBlt -- see paint(). Sized to the client rect and rebuilt only when that
 * changes. */
static HDC     g_back_dc;
static HBITMAP g_back_bmp;
static HBITMAP g_back_old;
static int     g_back_w, g_back_h;
static int     g_back_dirty;   /* the picture in it is stale (new frame, or
                                * the buffer was just created/resized) */
static RECT    g_pic;          /* where the picture sits in the back buffer */

static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

#define TIMER_REPAINT 1
#define TIMER_SYSACT  3 /* 2 is LOG_TIMER_ID, on the separate log window */

/* ---- display ---------------------------------------------------------------
 * INTVSESSION_FB_WIDTH x _HEIGHT is fixed (160x200, the STIC's own raw
 * pixel geometry), but 4:3 is the shape to letterbox into regardless --
 * the STIC's pixels are not square, and real Intellivision video (like
 * jzIntv's own default display) fills a standard NTSC 4:3 picture. Matches
 * the reasoning in frontends/gnome/display.c exactly. */

static RECT dest_rect(int cw, int ch)
{
    const double aspect = 4.0 / 3.0;
    double dw, dh;
    RECT r;

    if ((double)cw / ch > aspect) {
        dh = ch;
        dw = ch * aspect;
    } else {
        dw = cw;
        dh = cw / aspect;
    }
    r.left = (LONG)((cw - dw) / 2);
    r.top = (LONG)((ch - dh) / 2);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    return r;
}

/* Describes g_frame to GDI. Constant for the life of the process --
 * INTVSESSION_FB_WIDTH/_HEIGHT are the STIC's fixed geometry -- so it is
 * built once rather than on the stack of every paint. */
static const BITMAPINFO *frame_bmi(void)
{
    static BITMAPINFO bmi;
    if (bmi.bmiHeader.biSize == 0) {
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = INTVSESSION_FB_WIDTH;
        bmi.bmiHeader.biHeight = -INTVSESSION_FB_HEIGHT; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
    }
    return &bmi;
}

static void free_backbuffer(void)
{
    if (g_back_dc) {
        SelectObject(g_back_dc, g_back_old);
        DeleteDC(g_back_dc);
        g_back_dc = NULL;
        g_back_old = NULL;
    }
    if (g_back_bmp) {
        DeleteObject(g_back_bmp);
        g_back_bmp = NULL;
    }
    g_back_w = g_back_h = 0;
}

/* Ensures the off-screen buffer matches the client size. Returns 0 if it
 * could not be created, in which case paint() falls back to drawing straight
 * to the window (correct, just flicker-prone -- better than a blank window). */
static int ensure_backbuffer(HDC ref, int w, int h)
{
    if (w <= 0 || h <= 0)
        return 0;
    if (g_back_dc && g_back_w == w && g_back_h == h)
        return 1;

    free_backbuffer();
    g_back_dc = CreateCompatibleDC(ref);
    if (!g_back_dc)
        return 0;
    g_back_bmp = CreateCompatibleBitmap(ref, w, h);
    if (!g_back_bmp) {
        DeleteDC(g_back_dc);
        g_back_dc = NULL;
        return 0;
    }
    g_back_old = (HBITMAP)SelectObject(g_back_dc, g_back_bmp);
    SetStretchBltMode(g_back_dc, COLORONCOLOR); /* once, not per paint */
    g_back_w = w;
    g_back_h = h;
    g_back_dirty = 1; /* nothing has been drawn into it yet */
    return 1;
}

/* Redraws the picture and its letterbox bars into the back buffer. Only
 * called when something actually changed -- a repaint caused by the window
 * being uncovered or moved reuses what is already there, which is the whole
 * point of keeping a back buffer rather than a bare double-buffer swap. */
static void compose(int w, int h)
{
    RECT bar;

    g_pic = dest_rect(w, h);

    /* Only the bars, not the whole client area: the picture region is about
     * to be overwritten anyway, and on a 4:3 window there are no bars at all.
     * At most one pair is non-empty (dest_rect pillarboxes or letterboxes,
     * never both), and FillRect ignores an empty rect. */
    bar.left = 0; bar.top = 0; bar.right = w; bar.bottom = g_pic.top;
    FillRect(g_back_dc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
    bar.top = g_pic.bottom; bar.bottom = h;
    FillRect(g_back_dc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
    bar.top = 0; bar.bottom = h; bar.left = 0; bar.right = g_pic.left;
    FillRect(g_back_dc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
    bar.left = g_pic.right; bar.right = w;
    FillRect(g_back_dc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (g_have_frame)
        StretchDIBits(g_back_dc, g_pic.left, g_pic.top,
                      g_pic.right - g_pic.left, g_pic.bottom - g_pic.top,
                      0, 0, INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT,
                      g_frame, frame_bmi(), DIB_RGB_COLORS, SRCCOPY);
    else
        FillRect(g_back_dc, &g_pic, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

/* WHY THE BACK BUFFER: this used to FillRect the whole client area black and
 * then StretchDIBits the picture over it, both straight onto the window DC --
 * i.e. straight into the surface DWM composites from. GDI batches, and DWM
 * samples that surface on its own vsync, independently of this thread. Sample
 * between the fill and the blit and the composited frame is entirely black:
 * rare per frame, near-certain over a few minutes of play, and reported as
 * "the screen occasionally goes black for a frame or two". Composing
 * off-screen and blitting once leaves no intermediate state for DWM to catch.
 *
 * It is also much less work. A repaint with no new frame (uncover, move,
 * another window dragged across) now costs one BitBlt instead of a fresh
 * 160x200 -> client-size software stretch. */
static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    int w, h;

    GetClientRect(hwnd, &client);
    w = client.right;
    h = client.bottom;

    if (!ensure_backbuffer(hdc, w, h)) {
        /* No back buffer to be had. Draw directly; flicker beats blank. */
        FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_have_frame) {
            RECT d = dest_rect(w, h);
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, d.left, d.top, d.right - d.left,
                          d.bottom - d.top, 0, 0, INTVSESSION_FB_WIDTH,
                          INTVSESSION_FB_HEIGHT, g_frame, frame_bmi(),
                          DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return;
    }

    if (g_back_dirty) {
        compose(w, h);
        g_back_dirty = 0;
    }

    /* Only the invalid region -- BeginPaint has already clipped to it, but
     * blitting just those pixels keeps a partial repaint cheap. */
    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
           ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top,
           g_back_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

    EndPaint(hwnd, &ps);
}

/* The VK -> keysym translation and the reserved-hotkey table live in
 * key_translate.c, so they can be unit-tested (keytranslate_test.c) -- this
 * file cannot be linked against. Declared in key_forward.h. */

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);
static void open_keypad(void);
static void open_ecskbd(void);
static void reset_to_config(void);


static void on_key(HWND hwnd, WPARAM vk, LPARAM lp, int down)
{
    /* Which keys are hotkeys is intv_key_is_reserved's answer, not a second
     * list here: a keypad window's Map mode asks the same function so it can
     * refuse to bind one (key_translate.c), and two lists that had to agree
     * would eventually stop agreeing -- leaving Map mode either refusing a
     * key nothing claims, or accepting one this function then shadows. The
     * switch below only says what each reserved key DOES.
     *
     * The release is swallowed too: these never reach the machine, so
     * forwarding a lone key-up would look like a release of a key that was
     * never pressed. */
    if (!intv_key_is_reserved(vk, NULL)) {
        intv_forward_key(vk, lp, down);
        return;
    }
    if (!down)
        return;

    switch (vk) {
    case VK_F9:
        open_keypad();
        break;
    case VK_F10:
        /* Claimed here like F9/F11/F12 rather than left to Win32's own
         * "F10 activates the menu bar" default -- see wnd_proc's own
         * WM_KEYDOWN/WM_SYSKEYDOWN handling, which no longer lets F10
         * fall through to DefWindowProc for this reason. */
        open_ecskbd();
        break;
    case VK_F11:
        toggle_fullscreen(hwnd);
        break;
    case VK_F12:
        open_debugger();
        break;
    case 'R': /* only reserved while Ctrl is held -- see intv_key_is_reserved */
        reset_to_config();
        break;
    default:
        break;
    }
}


/* The translate-and-dispatch half of on_key, without any of its hotkey
 * handling -- shared with the keypad window, whose child controls take
 * focus on click and so must forward keystrokes here rather than letting
 * them fall on the floor. Declared in key_forward.h. */
void intv_forward_key(WPARAM vk, LPARAM lp, int down)
{
    const uint32_t keysym = intv_keysym_from_msg(vk, lp);
    intvsession_key_mapping m;

    if (!keysym)
        return;

    /* _bound, not the pure table: a keypad window "Map" remap has to reach
     * every keyboard-driven window, not just the one the remap happened
     * in -- see intvsession.h's own comment on why. Resolved once, up
     * front, so a system-action target can be checked ahead of ECS
     * keyboard mode below. */
    m = intvsession_key_from_keysym_bound(g_session, keysym);

    /* System actions outrank ECS keyboard mode: Escape/Backspace default to
     * Reset to CONFIG/Reset Game, but both are also real ECS keys
     * (intvsession_ecs_key_from_keysym) -- ESC has to always be able to get
     * back to CONFIG regardless of which mode has the keyboard. Cost:
     * ESC/Backspace stop reaching the emulated ECS matrix from the HOST
     * keyboard while bound this way; the standalone ECS keyboard window is
     * unaffected (intv_forward_ecs_key below calls intvsession_ecs_key_set
     * directly, ignoring keyboard_mode). */
    if (m.kind == INTVSESSION_MAP_SYSACT) {
        /* Win32 auto-repeats WM_KEYDOWN/WM_SYSKEYDOWN for as long as a key
         * is held -- lp bit 30 ("previous key state") is 1 on every repeat
         * and 0 only on the genuine first press, so gate on that to fire
         * once, not ~30 times/sec for as long as the key is held. Bit 30 is
         * always 1 on WM_KEYUP/WM_SYSKEYUP (a release is never a repeat),
         * so this only needs checking on the down edge; the release itself
         * is a no-op for a sysaction either way. */
        if (down && !(lp & (1L << 30)))
            intvsession_sysaction_fire(g_session, m.sysact);
        return;
    }

    /* "ECS Keyboard" input mode (Settings, or toggled live from there)
     * steals the keyboard for the ECS's own keyboard instead of the hand
     * controllers -- see intvsession_ecs_key_from_keysym's own comment on
     * why the two can't both claim it at once. */
    if (intvsession_get_int(g_session, "keyboard_mode", 0)) {
        intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
        if (key != INTVSESSION_ECS_KEY_NONE)
            intvsession_ecs_key_set(g_session, key, down);
        return;
    }

    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(g_session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(g_session, m.side, down ? m.direction : -1);
}

/* Unconditionally routes to the ECS keyboard matrix, ignoring
 * "keyboard_mode" -- for the ECS keyboard window, which IS the ECS
 * keyboard and so means the same thing whether or not the setting happens
 * to be on. Matches that window's on-screen buttons, and the GNOME and KDE
 * ports' equivalents. Declared in key_forward.h. */
void intv_forward_ecs_key(WPARAM vk, LPARAM lp, int down)
{
    const uint32_t keysym = intv_keysym_from_msg(vk, lp);
    intvsession_ecs_key key;

    if (!keysym)
        return;

    key = intvsession_ecs_key_from_keysym(keysym);
    if (key != INTVSESSION_ECS_KEY_NONE)
        intvsession_ecs_key_set(g_session, key, down);
}

/* ---- menu actions ------------------------------------------------------------ */

static void show_fujinet_config(void)
{
    ShellExecuteA(NULL, "open", intvsession_fujinet_webui_url(g_session), NULL,
                 NULL, SW_SHOWNORMAL);
}

#define LOG_TIMER_ID 2

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = intvsession_fujinet_copy_log(g_session, buf, sizeof(buf));
        SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, LOG_TIMER_ID);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    static int registered;
    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "IntvLogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA(
        "IntvLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, NULL, NULL, inst, NULL);
    g_log_edit = CreateWindowA(
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_log_window, NULL, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT),
                TRUE);
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    ShowWindow(g_log_window, SW_SHOW);
}

static void open_debugger(void)
{
    intv_debugger_show(g_hwnd, g_session);
}

static void open_keypad(void)
{
    intv_keypad_window_toggle(g_hwnd, g_session);
}

static void open_ecskbd(void)
{
    intv_ecskbd_window_toggle(g_hwnd, g_session);
}

/* Stops and restarts the session with intvsession_default_opts() re-read
 * from the settings store -- the "apply" side of a Settings dialog change
 * to ECS/Intellivoice/video standard, matching the GNOME/KDE frontends'
 * own restart helpers. */
static void restart_session(void)
{
    intvsession_start_opts opts;
    intvsession_stop(g_session);
    intvsession_default_opts(g_session, &opts);
    if (intvsession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, intvsession_last_error(g_session),
                    "FujiNet Go Intv", MB_ICONWARNING);
}

static void reset_to_config(void)
{
    if (intvsession_reset_to_config(g_session) != 0)
        MessageBoxA(g_hwnd, intvsession_last_error(g_session),
                    "FujiNet Go Intv", MB_ICONWARNING);
}

static void reset_game(void)
{
    if (intvsession_reset_game(g_session) != 0)
        MessageBoxA(g_hwnd, intvsession_last_error(g_session),
                    "FujiNet Go Intv", MB_ICONWARNING);
}

/* Drains intvsession_sysaction_take -- the one caller that can't fire a
 * sysaction directly is gamepad_sdl.c's SDL thread (queuing RESET_CONFIG so
 * it never joins itself); this, the UI thread, fires each on TIMER_SYSACT's
 * tick (see WinMain's own SetTimer). Lives on the main window rather than
 * the keypad window: that window hides rather than destroys itself (see
 * intv_keypad_pretranslate's own header) but is hidden far more often than
 * shown, and a gamepad-bound sysaction has to keep working regardless. */
static void drain_sysactions(void)
{
    intvsession_sysaction a;
    while (intvsession_sysaction_take(g_session, &a)) {
        if (intvsession_sysaction_fire(g_session, a) != 0)
            MessageBoxA(g_hwnd, intvsession_last_error(g_session),
                        "FujiNet Go Intv", MB_ICONWARNING);
    }
}

/* ---- settings --------------------------------------------------------------
 * A registered WNDCLASSA + programmatically created child controls (combo
 * boxes for the tri-state ECS/Intellivoice options and the video standard,
 * a checkbox for the ECS keyboard input mode) -- not a DIALOGEX resource
 * template, matching every other secondary window in this frontend (the
 * keypad and debugger windows). Key/default list matches the GNOME/KDE
 * frontends' own settings dialogs (frontends/gnome/prefs.c's header is the
 * canonical reference):
 *   "ecs"/"ivoice"    INTVSESSION_HW_* (default AUTO)     -- restart to apply
 *   "video_standard"  INTVSESSION_VIDEO_* (default NTSC)  -- restart to apply
 *   "keyboard_mode"   0 = hand controllers, 1 = ECS keyboard -- applies live
 */

static HWND g_settings_window;
static int g_settings_dirty;

static void fill_combo(HWND combo, const char *(*name_fn)(int), int cur)
{
    int i;
    for (i = 0; name_fn(i); i++)
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)name_fn(i));
    SendMessageA(combo, CB_SETCURSEL, (WPARAM)cur, 0);
}

static void settings_hw_combo_changed(int ctrl_id, const char *key)
{
    int sel = (int)SendMessageA(GetDlgItem(g_settings_window, ctrl_id),
                                CB_GETCURSEL, 0, 0);
    if (intvsession_get_int(g_session, key, INTVSESSION_HW_AUTO) == sel)
        return;
    intvsession_set_int(g_session, key, sel);
    g_settings_dirty = 1;
}

/* ---- controller list ----------------------------------------------------
 * intvsession_gamepad_count/_name/_assign had no caller in any frontend, so
 * there was no way to tell whether a controller had been detected at all,
 * let alone which side it was driving. That is a poor experience for any
 * pad and a blocking one for an Intellivision-to-USB adapter, which
 * enumerates as one plain HID joystick per controller: "is this one the left
 * or the right?" is exactly the question the user has to answer to map it.
 *
 * Repopulated on a timer rather than on a Refresh button, because a device
 * can be plugged in while this window is open and a stale list is worse than
 * none. Rebuilt only when something actually changed, so the selection is
 * not yanked out from under a click. */
#define IDT_SETTINGS_PADS 1

static int pad_list_signature(void)
{
    /* Cheap "has anything changed" stamp: count, plus each pad's effective
     * side. Enough to catch connect/disconnect and reassignment, which is
     * everything the list shows. */
    int sig = intvsession_gamepad_count(g_session) * 31;
    for (int i = 0; i < intvsession_gamepad_count(g_session); i++)
        sig = sig * 7 + intvsession_gamepad_effective_side(g_session, i) + 2;
    return sig;
}

static void fill_pad_list(HWND hwnd, int keep_sel)
{
    HWND list = GetDlgItem(hwnd, IDC_SET_PAD_LIST);
    HWND side = GetDlgItem(hwnd, IDC_SET_PAD_SIDE);
    const int n = intvsession_gamepad_count(g_session);
    int sel = keep_sel;

    if (!list)
        return;
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        char name[128], row[192];
        const int eff = intvsession_gamepad_effective_side(g_session, i);
        name[0] = '\0';
        intvsession_gamepad_name(g_session, i, name, sizeof(name));
        snprintf(row, sizeof(row), "%s  \xE2\x80\x94  %s", name,
                eff >= 0 ? intvsession_pad_side_name(
                               (intvsession_pad_side)eff)
                         : "not driving a controller");
        SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)row);
    }
    if (n == 0)
        SendMessageA(list, LB_ADDSTRING, 0,
                    (LPARAM)"No controllers detected");
    if (sel < 0 || sel >= n)
        sel = n > 0 ? 0 : -1;
    if (sel >= 0)
        SendMessageA(list, LB_SETCURSEL, (WPARAM)sel, 0);

    if (side) {
        EnableWindow(side, n > 0);
        if (sel >= 0) {
            const int assigned =
                intvsession_gamepad_assignment(g_session, sel);
            SendMessageA(side, CB_SETCURSEL,
                        (WPARAM)(assigned < 0 ? 0 : assigned + 1), 0);
        }
    }
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_ECS:
            if (HIWORD(wp) == CBN_SELCHANGE)
                settings_hw_combo_changed(IDC_SET_ECS, "ecs");
            return 0;
        case IDC_SET_IVOICE:
            if (HIWORD(wp) == CBN_SELCHANGE)
                settings_hw_combo_changed(IDC_SET_IVOICE, "ivoice");
            return 0;
        case IDC_SET_VIDEO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_VIDEO),
                                            CB_GETCURSEL, 0, 0);
                if (intvsession_get_int(g_session, "video_standard",
                                        INTVSESSION_VIDEO_NTSC) != sel) {
                    intvsession_set_int(g_session, "video_standard", sel);
                    g_settings_dirty = 1;
                }
            }
            return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                HWND side = GetDlgItem(hwnd, IDC_SET_PAD_SIDE);
                const int sel = (int)SendMessageA(
                    GetDlgItem(hwnd, IDC_SET_PAD_LIST), LB_GETCURSEL, 0, 0);
                if (side && sel >= 0) {
                    const int assigned =
                        intvsession_gamepad_assignment(g_session, sel);
                    SendMessageA(side, CB_SETCURSEL,
                                (WPARAM)(assigned < 0 ? 0 : assigned + 1), 0);
                }
            }
            return 0;
        case IDC_SET_PAD_SIDE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(
                    GetDlgItem(hwnd, IDC_SET_PAD_LIST), LB_GETCURSEL, 0, 0);
                const int choice = (int)SendMessageA(
                    GetDlgItem(hwnd, IDC_SET_PAD_SIDE), CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < intvsession_gamepad_count(g_session) &&
                    choice >= 0) {
                    /* Entry 0 is "Automatic" (-1); the rest are the four
                     * sides in intvsession_pad_side order. */
                    intvsession_gamepad_assign(g_session, sel, choice - 1);
                    /* A reassignment can move some OTHER pad too -- the
                     * automatic slots renumber around an explicit one (see
                     * intv_pad_for_port) -- so redraw the whole list, not
                     * just this row, and keep the selection. */
                    fill_pad_list(hwnd, sel);
                }
            }
            return 0;
        case IDC_SET_ECS_KEYBOARD: {
            int on = SendMessageA(GetDlgItem(hwnd, IDC_SET_ECS_KEYBOARD),
                                  BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (intvsession_get_int(g_session, "keyboard_mode", 0) !=
                (on ? 1 : 0)) {
                intvsession_set_int(g_session, "keyboard_mode", on ? 1 : 0);
                if (!on)
                    intvsession_ecs_keys_clear(g_session);
            }
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            /* Only when something actually changed, so the timer cannot pull
             * the selection out from under a click. */
            static int last_sig;
            const int sig = pad_list_signature();
            if (sig != last_sig) {
                last_sig = sig;
                fill_pad_list(hwnd,
                             (int)SendMessageA(
                                 GetDlgItem(hwnd, IDC_SET_PAD_LIST),
                                 LB_GETCURSEL, 0, 0));
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_settings(HINSTANCE inst)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND combo, check, note, list;
    int y = 16;
    int have_ecs_rom;

    if (g_settings_window) {
        SetForegroundWindow(g_settings_window);
        return;
    }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "IntvSettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA(
        "IntvSettingsWindow", "Settings",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 560, NULL, NULL, inst, NULL);

    have_ecs_rom = intvsession_has_ecs_rom(g_session);
    CreateWindowExA(0, "STATIC", "ECS:", WS_CHILD | WS_VISIBLE, 16, y, 120,
                    20, g_settings_window, NULL, inst, NULL);
    combo = CreateWindowExA(
        0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                              WS_VSCROLL | (have_ecs_rom ? 0 : WS_DISABLED),
        140, y - 2, 220, 200, g_settings_window,
        (HMENU)(INT_PTR)IDC_SET_ECS, inst, NULL);
    fill_combo(combo, intvsession_hw_mode_name,
              intvsession_get_int(g_session, "ecs", INTVSESSION_HW_AUTO));
    SendMessageA(combo, WM_SETFONT, (WPARAM)font, TRUE);
    y += 28;
    if (!have_ecs_rom) {
        CreateWindowExA(0, "STATIC",
                        "ecs.bin not found in the ROM directory",
                        WS_CHILD | WS_VISIBLE, 140, y, 240, 18,
                        g_settings_window, NULL, inst, NULL);
        y += 20;
    }

    CreateWindowExA(0, "STATIC", "Intellivoice:", WS_CHILD | WS_VISIBLE, 16,
                    y, 120, 20, g_settings_window, NULL, inst, NULL);
    combo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 140, y - 2,
        220, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_IVOICE, inst,
        NULL);
    fill_combo(combo, intvsession_hw_mode_name,
              intvsession_get_int(g_session, "ivoice", INTVSESSION_HW_AUTO));
    SendMessageA(combo, WM_SETFONT, (WPARAM)font, TRUE);
    y += 32;

    CreateWindowExA(0, "STATIC", "Video Standard:", WS_CHILD | WS_VISIBLE,
                    16, y, 120, 20, g_settings_window, NULL, inst, NULL);
    combo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 140, y - 2,
        220, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VIDEO, inst,
        NULL);
    fill_combo(combo, intvsession_video_name,
              intvsession_get_int(g_session, "video_standard",
                                  INTVSESSION_VIDEO_NTSC));
    SendMessageA(combo, WM_SETFONT, (WPARAM)font, TRUE);
    y += 36;

    check = CreateWindowExA(
        0, "BUTTON", "ECS Keyboard (types on the ECS instead of the hand controllers)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, y, 420, 22,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_ECS_KEYBOARD, inst, NULL);
    SendMessageA(check, BM_SETCHECK,
                intvsession_get_int(g_session, "keyboard_mode", 0)
                    ? BST_CHECKED
                    : BST_UNCHECKED,
                0);
    SendMessageA(check, WM_SETFONT, (WPARAM)font, TRUE);
    y += 32;

    note = CreateWindowExA(
        0, "STATIC",
        "ECS/Intellivoice/Video Standard apply when this window is closed "
        "(the session restarts). ECS Keyboard applies immediately.",
        WS_CHILD | WS_VISIBLE, 16, y, 420, 80, g_settings_window, NULL, inst,
        NULL);
    SendMessageA(note, WM_SETFONT, (WPARAM)font, TRUE);
    /* Generous: this wraps to three lines at Windows' own metrics and four
     * under Wine's chunkier fonts, and a clipped explanation is worse than a
     * little whitespace. */
    y += 88;

    CreateWindowExA(0, "STATIC", "Controllers:", WS_CHILD | WS_VISIBLE, 16, y,
                    120, 20, g_settings_window, NULL, inst, NULL);
    y += 20;
    list = CreateWindowExA(
        WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 420, 76,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(list, WM_SETFONT, (WPARAM)font, TRUE);
    y += 84;

    CreateWindowExA(0, "STATIC", "Drives:", WS_CHILD | WS_VISIBLE, 16, y, 120,
                    20, g_settings_window, NULL, inst, NULL);
    combo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 140, y - 2,
        296, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_SIDE, inst,
        NULL);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    for (int side = 0; side < 4; side++)
        SendMessageA(combo, CB_ADDSTRING, 0,
                    (LPARAM)intvsession_pad_side_name(
                        (intvsession_pad_side)side));
    SendMessageA(combo, WM_SETFONT, (WPARAM)font, TRUE);

    fill_pad_list(g_settings_window, -1);
    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 1000, NULL);

    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- fullscreen ---------------------------------------------------------------- */

/* SWP_FRAMECHANGED rebuilds the window frame, and the client area is
 * undefined until something paints it -- with the repaint timer up to 8 ms
 * away, that gap is visible as a black flash on the way into fullscreen and
 * again on the way out. Repaint synchronously instead of waiting for the
 * tick. */
static void fullscreen_repaint(HWND hwnd)
{
    g_back_dirty = 1; /* the client size changed, so the letterbox did too */
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
}

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetWindowPlacement(hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                           &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
        fullscreen_repaint(hwnd);
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
        fullscreen_repaint(hwnd);
    }
}

/* ---- menu -----------------------------------------------------------------------
 * No machine/TV-input/CPU radio groups: the Intellivision has one
 * configuration, matching the GNOME/KDE frontends' own menus. */

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU settings = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(fujinet, MF_STRING, IDM_RESET_GAME, "Reset Game\tBackspace");
    AppendMenuA(fujinet, MF_STRING, IDM_RESET_CONFIG, "Reset to CONFIG\tCtrl+R");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "Keypad\tF9");
    AppendMenuA(view, MF_STRING, IDM_ECS_KEYBOARD, "ECS Keyboard\tF10");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

    AppendMenuA(settings, MF_STRING, IDM_SETTINGS, "Settings...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)settings, "&Settings");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "About FujiNet Go Intv");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* paint() covers every pixel; never erase underneath it */
    case WM_SIZE:
        /* The letterbox geometry moved, so the back buffer's contents are
         * stale as well as the wrong size (ensure_backbuffer would set
         * g_back_dirty itself on a resize, but not for a restore to the same
         * size after a minimize). */
        g_back_dirty = 1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DPICHANGED:
        /* PerMonitorV2 is declared in app.manifest.in, which obliges the app
         * to take the suggested rect when the window moves to a monitor at a
         * different scale. Unhandled, the window kept its old size while
         * Windows rebuilt the redirection surface underneath it -- a
         * reliable way to get a black or garbled frame mid-drag. */
        {
            const RECT *sug = (const RECT *)lp;
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                        sug->right - sug->left, sug->bottom - sug->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
            g_back_dirty = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REPAINT) {
            if (intvsession_copy_frame(g_session, g_frame, &g_serial)) {
                g_have_frame = 1;
                g_back_dirty = 1;
                /* Only the picture: the bars are unchanged, and repainting
                 * them every frame is work the compositor has to carry. */
                InvalidateRect(hwnd, g_back_dc ? &g_pic : NULL, FALSE);
            }
            return 0;
        }
        if (wp == TIMER_SYSACT) {
            drain_sysactions();
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        on_key(hwnd, wp, lp, 1);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key(hwnd, wp, lp, 0);
        return 0;
    case WM_KILLFOCUS:
        /* Losing keyboard focus shouldn't leave a key stuck down in
         * whichever matrix was active -- see intvsession_ecs_keys_clear's
         * own comment. Both matrices: the hand controllers were missing
         * here, so alt-tabbing mid-press left that key held for good, and
         * widening the bindable key range only makes that easier to hit. */
        intvsession_ecs_keys_clear(g_session);
        intvsession_pads_clear(g_session);
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_KEYPAD:         open_keypad(); break;
        case IDM_ECS_KEYBOARD:   open_ecskbd(); break;
        case IDM_RESET_GAME:     reset_game(); break;
        case IDM_RESET_CONFIG:   reset_to_config(); break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_SETTINGS:
            show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                        "FujiNet Go Intv\n"
                        "Self-contained Mattel Intellivision with built-in "
                        "FujiNet.\n"
                        "Copyright (C) 2026 Thomas Cherryhomes\n"
                        "GPL-3.0-or-later",
                        "About FujiNet Go Intv", MB_ICONINFORMATION);
            break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        free_backbuffer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();

    g_session = intvsession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", "FujiNet Go Intv",
                    MB_ICONERROR);
        return 1;
    }
    {
        intvsession_start_opts opts;
        intvsession_default_opts(g_session, &opts);
        if (intvsession_start(g_session, &opts) != 0)
            MessageBoxA(NULL, intvsession_last_error(g_session),
                       "FujiNet Go Intv", MB_ICONWARNING);
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* NULL, not BLACK_BRUSH: paint() covers every pixel of the client area
     * from its back buffer, so the only thing a class brush can still do is
     * flash black in the gap Windows erases on its own (a resize exposing new
     * area, SWP_FRAMECHANGED, a monitor change) before WM_PAINT lands. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = "IntvMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    g_hwnd = CreateWindowExA(0, "IntvMainWindow", "FujiNet Go Intv",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 650, NULL, g_menu, inst, NULL);
    if (!g_hwnd)
        return 1;
    ShowWindow(g_hwnd, show);

    /* Windows' default timer granularity is ~15.6 ms, so a 16 ms repaint
     * timer beat against the emulator's own 60 Hz publish rate and dropped or
     * doubled frames on an irregular cycle. It also clamped every sub-frame
     * sleep jzIntv's rate control asks for (its plat_delay uses a plain
     * CreateWaitableTimer), so the emulator itself ran in ~15.6 ms steps and
     * declared dropped frames. One call fixes both, and since Windows 10 2004
     * it affects only this process. Paired with a poll at half a frame, so a
     * published frame is never waiting more than ~8 ms -- copy_frame is a
     * mutex and a serial compare when nothing changed, so the extra ticks are
     * close to free. */
    timeBeginPeriod(1);
    SetTimer(g_hwnd, TIMER_REPAINT, 8, NULL);
    SetTimer(g_hwnd, TIMER_SYSACT, 100, NULL);

    /* Developer affordances, matching the GNOME frontend's own
     * INTV_OPEN_DEBUGGER/INTV_OPEN_KEYPAD env vars. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        open_debugger();
    if (getenv("INTV_OPEN_KEYPAD"))
        open_keypad();
    /* Same affordance for Settings. It earns its place here more than the
     * other two: the maintainer has no Windows machine, so this window is
     * only ever seen by cross-building and running under Wine, where there
     * is no way to drive a menu. */
    if (getenv("INTV_OPEN_SETTINGS"))
        show_settings(inst);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (intv_debugger_pretranslate(&msg))
            continue;
        if (intv_keypad_pretranslate(&msg))
            continue;
        if (intv_ecskbd_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    KillTimer(g_hwnd, TIMER_REPAINT);
    KillTimer(g_hwnd, TIMER_SYSACT);
    timeEndPeriod(1);
    intvsession_free(g_session);
    return (int)msg.wParam;
}
