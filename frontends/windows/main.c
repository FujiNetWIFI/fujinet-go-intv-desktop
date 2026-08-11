/*
 * FujiNet Go Intv -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE frontends over the shared intvsession API: a
 * GDI-blitted display letterboxed to 4:3, full keyboard mapping, a menu
 * bar, and the FujiNet configuration (default browser) and console-log
 * windows, plus a clickable keypad window.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvsession.h"
#include "debugger/dbg_window.h"
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

static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

#define TIMER_REPAINT 1

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

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    BITMAPINFO bmi;
    RECT d;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (g_have_frame) {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = INTVSESSION_FB_WIDTH;
        bmi.bmiHeader.biHeight = -INTVSESSION_FB_HEIGHT; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        d = dest_rect(client.right, client.bottom);
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                      0, 0, INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT,
                      g_frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    EndPaint(hwnd, &ps);
}

/* ---- keyboard ----------------------------------------------------------------
 * intvsession_key_from_keysym wants intvsession.h's OWN private numbering
 * for non-printable keys (INTVSESSION_KEYSYM_* starting at 0x1000) -- NOT
 * an X11 keysym the way the CoCo/MSX ports' own special_keysym tables
 * produce. See frontends/gnome/keysym_map.h's header comment for why that
 * distinction matters (a real bug was caught there: passing a toolkit's
 * native key value straight through silently drops every arrow/numpad/
 * modifier key while ASCII letters keep working by coincidence).
 *
 * Win32's VK_* codes carry no shift state of their own, so non-special keys
 * resolve straight to their base US-layout character, stable across
 * down/up by construction -- same reasoning the CoCo/MSX ports' own
 * base_char() documents. */

static uint32_t special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_UP:       return INTVSESSION_KEYSYM_UP;
    case VK_DOWN:     return INTVSESSION_KEYSYM_DOWN;
    case VK_LEFT:     return INTVSESSION_KEYSYM_LEFT;
    case VK_RIGHT:    return INTVSESSION_KEYSYM_RIGHT;
    case VK_NUMPAD0:  return INTVSESSION_KEYSYM_KP_0;
    case VK_NUMPAD1:  return INTVSESSION_KEYSYM_KP_1;
    case VK_NUMPAD2:  return INTVSESSION_KEYSYM_KP_2;
    case VK_NUMPAD3:  return INTVSESSION_KEYSYM_KP_3;
    case VK_NUMPAD4:  return INTVSESSION_KEYSYM_KP_4;
    case VK_NUMPAD5:  return INTVSESSION_KEYSYM_KP_5;
    case VK_NUMPAD6:  return INTVSESSION_KEYSYM_KP_6;
    case VK_NUMPAD7:  return INTVSESSION_KEYSYM_KP_7;
    case VK_NUMPAD8:  return INTVSESSION_KEYSYM_KP_8;
    case VK_NUMPAD9:  return INTVSESSION_KEYSYM_KP_9;
    case VK_DECIMAL:  return INTVSESSION_KEYSYM_KP_PERIOD;
    case VK_LSHIFT:   return INTVSESSION_KEYSYM_LSHIFT;
    case VK_RSHIFT:   return INTVSESSION_KEYSYM_RSHIFT;
    case VK_LCONTROL: return INTVSESSION_KEYSYM_LCTRL;
    case VK_RCONTROL: return INTVSESSION_KEYSYM_RCTRL;
    case VK_LMENU:    return INTVSESSION_KEYSYM_LALT;
    case VK_RMENU:    return INTVSESSION_KEYSYM_RALT;
    /* VK_RETURN doubles as the numpad Enter key when the extended-key bit
     * is set (checked by the caller via lParam), otherwise it is the main
     * Enter and falls through to base_char's own '\r'-less printable path
     * -- so it is deliberately NOT resolved here; see on_key. */
    default:
        return 0;
    }
}

static uint32_t base_char(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PLUS:   return '=';
    case VK_OEM_COMMA:  return ',';
    case VK_OEM_PERIOD: return '.';
    default:            return 0;
    }
}

/* WM_(SYS)KEYDOWN/UP only report the generic VK_SHIFT/CONTROL/MENU; the
 * left/right pair is recovered from the scancode (Shift) or the
 * extended-key bit (Control/Alt), the standard Win32 idiom for this. */
static WPARAM resolve_side(WPARAM vk, LPARAM lp)
{
    UINT scancode;
    int extended;

    switch (vk) {
    case VK_SHIFT:
        scancode = (UINT)((lp >> 16) & 0xFF);
        return MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);
static void open_keypad(void);

static void on_key(HWND hwnd, WPARAM vk, LPARAM lp, int down)
{
    WPARAM rvk;
    uint32_t keysym;
    intvsession_key_mapping m;

    if (down) {
        switch (vk) {
        case VK_F9:
            open_keypad();
            return;
        case VK_F11:
            toggle_fullscreen(hwnd);
            return;
        case VK_F12:
            open_debugger();
            return;
        default:
            break;
        }
    } else if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) {
        return;
    }

    rvk = resolve_side(vk, lp);
    /* Numpad Enter reports as VK_RETURN with the extended-key bit set;
     * plain Enter does not carry a keypad binding in intv_keymap.c at all
     * (jzIntv's own mapping.c has no ENTER-key binding outside the
     * numpad), so only the extended form maps to anything. */
    if (rvk == VK_RETURN && (lp & 0x01000000))
        keysym = INTVSESSION_KEYSYM_KP_ENTER;
    else
        keysym = special_keysym(rvk);
    if (!keysym) {
        keysym = base_char(rvk);
        if (!keysym)
            return; /* not a key the emulated controller has */
    }

    m = intvsession_key_from_keysym(keysym);
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(g_session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(g_session, m.side, down ? m.direction : -1);
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

/* ---- fullscreen ---------------------------------------------------------------- */

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
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
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
    HMENU help = CreatePopupMenu();

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "Keypad\tF9");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

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
        return 1; /* paint() clears; avoid flicker */
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REPAINT) {
            if (intvsession_copy_frame(g_session, g_frame, &g_serial)) {
                g_have_frame = 1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        on_key(hwnd, wp, lp, 1);
        if (wp == VK_F10)
            break; /* let the system open the menu */
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key(hwnd, wp, lp, 0);
        if (wp == VK_F10)
            break;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_KEYPAD:         open_keypad(); break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
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
    if (intvsession_start(g_session) != 0)
        MessageBoxA(NULL, intvsession_last_error(g_session), "FujiNet Go Intv",
                    MB_ICONWARNING);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

    SetTimer(g_hwnd, TIMER_REPAINT, 16, NULL);

    /* Developer affordances, matching the GNOME frontend's own
     * INTV_OPEN_DEBUGGER/INTV_OPEN_KEYPAD env vars. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        open_debugger();
    if (getenv("INTV_OPEN_KEYPAD"))
        open_keypad();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (intv_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    KillTimer(g_hwnd, TIMER_REPAINT);
    intvsession_free(g_session);
    return (int)msg.wParam;
}
