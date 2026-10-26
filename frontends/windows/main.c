/*
 * FujiNet Go Intv -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE frontends over the shared intvsession API: a
 * Direct3D 11 display (display.c; GDI fallback) letterboxed to 4:3, full
 * keyboard mapping, a menu bar, importing of the BIOS and of cartridges for
 * the SD host, and the FujiNet configuration (default browser) and
 * console-log windows, plus clickable keypad and ECS keyboard windows.
 *
 * There is no vsync-feeding present thread to drive: intvsession has no
 * notify_vsync (jzIntv's own paced thread already governs itself to
 * NTSC/PAL speed independent of any frontend -- see frontends/gnome/
 * display.c's own comment on this), so display.c's render thread simply
 * shows the newest frame at every refresh.
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

#include <commctrl.h>

#include "intvsession.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "import.h"
#include "key_forward.h"
#include "ecskbd/ecskbd_window.h"
#include "keypad/keypad_window.h"
#include "resource.h"
#include "roms.h"
#include "win_common.h"

static intvsession *g_session;
static HWND g_hwnd;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

static HWND g_display;
static HWND g_bios_files_btn, g_bios_folder_btn;
static HFONT g_ui_font, g_title_font;
static HICON g_big_icon;
static int g_dpi = 96;

#define S(v) intv_scale((v), g_dpi)

static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

#define TIMER_SYSACT  3 /* 2 is LOG_TIMER_ID, on the separate log window */

static void start_machine(void);
static void refresh_settings_ecs(void);
static void show_roms(void);

/* ---- the No-BIOS page ------------------------------------------------------
 * The Executive and Graphics ROMs are Mattel's and are not part of the app.
 * Until they are imported the display child is hidden and the main window
 * shows this page instead: an icon, a title, a line of explanation and the
 * two Import buttons. The picture itself is display.c (Direct3D 11, 4:3). */

typedef struct {
    RECT icon, title, desc, files, folder;
} nobios_geom;

static const char k_nobios_title[] = "No Intellivision BIOS";
static const char k_nobios_text[] =
    "The Intellivision Executive and Graphics ROMs are not part of the app. "
    "Import the ones you have \xe2\x80\x94 exec.bin and grom.bin, a zip such as "
    "MAME's intv set, or a folder \xe2\x80\x94 and the Intellivision starts.";

static int text_height(HFONT font, const char *utf8, int width)
{
    wchar_t *w = intv_wide_dup(utf8);
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r;
    SetRect(&r, 0, 0, width, 0);
    if (w)
        DrawTextW(dc, w, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    free(w);
    return r.bottom - r.top;
}

static int button_width(const char *text)
{
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_ui_font);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return sz.cx + S(28) < S(80) ? S(80) : sz.cx + S(28);
}

static void nobios_layout(const RECT *area, nobios_geom *g)
{
    const int aw = area->right - area->left, ah = area->bottom - area->top;
    const int width = aw - S(48) < S(560) ? aw - S(48) : S(560);
    const int icon = S(64), title_h = text_height(g_title_font, k_nobios_title, width);
    const int desc_h = text_height(g_ui_font, k_nobios_text, width);
    const int bw1 = button_width("Import BIOS..."), bw2 = button_width("Import a Folder...");
    const int row_w = bw1 + S(12) + bw2, bh = S(32);
    const int total = icon + S(16) + title_h + S(10) + desc_h + S(24) + bh;
    const int x0 = area->left + (aw - width) / 2, cx = area->left + aw / 2;
    int y = area->top + (ah - total) / 2;

    if (y < area->top + S(8))
        y = area->top + S(8);
    SetRect(&g->icon, cx - icon / 2, y, cx + icon / 2, y + icon);
    y += icon + S(16);
    SetRect(&g->title, x0, y, x0 + width, y + title_h);
    y += title_h + S(10);
    SetRect(&g->desc, x0, y, x0 + width, y + desc_h);
    y += desc_h + S(24);
    SetRect(&g->files, cx - row_w / 2, y, cx - row_w / 2 + bw1, y + bh);
    SetRect(&g->folder, g->files.right + S(12), y, g->files.right + S(12) + bw2, y + bh);
}

static void paint_nobios(HDC dc, const RECT *area)
{
    nobios_geom g;
    wchar_t *w;

    nobios_layout(area, &g);
    FillRect(dc, area, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (g_big_icon)
        DrawIconEx(dc, g.icon.left, g.icon.top, g_big_icon,
                   g.icon.right - g.icon.left, g.icon.bottom - g.icon.top, 0,
                   NULL, DI_NORMAL);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SelectObject(dc, g_title_font);
    if ((w = intv_wide_dup(k_nobios_title)) != NULL) {
        DrawTextW(dc, w, -1, &g.title, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    }
    SelectObject(dc, g_ui_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    if ((w = intv_wide_dup(k_nobios_text)) != NULL) {
        DrawTextW(dc, w, -1, &g.desc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    }
}

/* The picture fills the client while there is a BIOS; else the page. */
static void layout(void)
{
    RECT c;

    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &c);
    if (g_display)
        MoveWindow(g_display, 0, 0, c.right, c.bottom, TRUE);
    if (g_bios_files_btn && !intvsession_has_system_roms(g_session)) {
        nobios_geom g;
        nobios_layout(&c, &g);
        MoveWindow(g_bios_files_btn, g.files.left, g.files.top,
                   g.files.right - g.files.left, g.files.bottom - g.files.top, TRUE);
        MoveWindow(g_bios_folder_btn, g.folder.left, g.folder.top,
                   g.folder.right - g.folder.left, g.folder.bottom - g.folder.top, TRUE);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void show_page(void)
{
    const int bios = intvsession_has_system_roms(g_session);

    if (g_display)
        ShowWindow(g_display, bios ? SW_SHOW : SW_HIDE);
    if (g_bios_files_btn) {
        ShowWindow(g_bios_files_btn, bios ? SW_HIDE : SW_SHOW);
        ShowWindow(g_bios_folder_btn, bios ? SW_HIDE : SW_SHOW);
    }
    layout();
}

static void make_fonts(void)
{
    if (g_ui_font) DeleteObject(g_ui_font);
    if (g_title_font) DeleteObject(g_title_font);
    g_ui_font = intv_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_title_font = intv_make_font(g_dpi, 22, FW_BOLD, NULL);
    if (g_bios_files_btn)
        SendMessageW(g_bios_files_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    if (g_bios_folder_btn)
        SendMessageW(g_bios_folder_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
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
     * why the two can't both claim it at once.
     *
     * The typed character picks the ECS key (its shifted layer is nothing
     * like a PC's, so "%" and "/" only reach the machine this way); the
     * keysym is the fallback for keys that type nothing;
     * intv_host_key_from_msg is the stable id the release is looked up by
     * (NOT the raw VK -- both shift keys share one). GetKeyState is
     * message-queue-synchronised, so it reports Shift as of when THIS
     * message was generated -- correct on both edges. See
     * intvsession_ecs_key_event. */
    if (intvsession_get_int(g_session, "keyboard_mode", 0)) {
        const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        intvsession_ecs_key_event(g_session, intv_host_key_from_msg(vk, lp),
                                  keysym, intv_char_from_msg(vk, lp, shift),
                                  down);
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
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (!keysym)
        return;

    intvsession_ecs_key_event(g_session, intv_host_key_from_msg(vk, lp),
                              keysym, intv_char_from_msg(vk, lp, shift), down);
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
    show_page();
    if (!intvsession_has_system_roms(g_session))
        return; /* nothing to boot: the No-BIOS page is up */
    intvsession_default_opts(g_session, &opts);
    if (intvsession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, intvsession_last_error(g_session),
                    "FujiNet Go Intv", MB_ICONWARNING);
}

/* After a BIOS import: the picture replaces the page, and the machine is
 * started -- or stopped and started again, so a session that was running
 * (with whatever ROMs it had) boots with the new ones. */
static void start_machine(void)
{
    show_page();
    if (intvsession_has_system_roms(g_session))
        restart_session();
}

static void roms_imported(void)
{
    start_machine();
    refresh_settings_ecs();
}

static void show_roms(void)
{
    static const intv_roms_hooks hooks = { roms_imported };
    intv_roms_show(g_hwnd, g_session, &hooks);
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
static HWND g_settings_ecs_combo, g_settings_ecs_note;
static int g_settings_dirty;

/* The ECS option follows ecs.bin: enabled only once it has been imported. */
static void refresh_settings_ecs(void)
{
    const int have = intvsession_has_ecs_rom(g_session);
    if (!g_settings_window)
        return;
    EnableWindow(g_settings_ecs_combo, have);
    ShowWindow(g_settings_ecs_note, have ? SW_HIDE : SW_SHOW);
}

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
        case IDC_SET_ROMS:
            show_roms();
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
        g_settings_ecs_combo = g_settings_ecs_note = NULL;
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
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 610, NULL, NULL, inst, NULL);

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
    /* Always laid out, shown only while ecs.bin is missing: importing it
     * (System ROMs) refreshes both this and the combo above. */
    g_settings_ecs_note = CreateWindowExA(
        0, "STATIC", "ecs.bin not found in the ROM directory",
        WS_CHILD | (have_ecs_rom ? 0 : WS_VISIBLE), 140, y, 300, 18,
        g_settings_window, NULL, inst, NULL);
    SendMessageA(g_settings_ecs_note, WM_SETFONT, (WPARAM)font, TRUE);
    g_settings_ecs_combo = combo;
    y += 20;

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
        "(the session restarts). ECS Keyboard applies immediately. "
        "Warning: the FujiNet CONFIG ROM has no ECS keyboard support yet, "
        "so while ECS Keyboard is on its menus can only be driven by a "
        "gamepad or the keypad window (F9).",
        WS_CHILD | WS_VISIBLE, 16, y, 420, 130, g_settings_window, NULL, inst,
        NULL);
    SendMessageA(note, WM_SETFONT, (WPARAM)font, TRUE);
    /* Generous: this wraps to five lines at Windows' own metrics and more
     * under Wine's chunkier fonts, and a clipped explanation is worse than a
     * little whitespace. */
    y += 138;

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

    y += 36;
    check = CreateWindowExA(0, "BUTTON", "System ROMs...",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 16, y, 160, 28,
                            g_settings_window, (HMENU)(INT_PTR)IDC_SET_ROMS,
                            inst, NULL);
    SendMessageA(check, WM_SETFONT, (WPARAM)font, TRUE);

    fill_pad_list(g_settings_window, -1);
    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 1000, NULL);

    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- fullscreen ---------------------------------------------------------------- */

/* SWP_FRAMECHANGED rebuilds the window frame and the client size changes
 * with it: resize the picture to the new client at once (the render thread
 * follows WM_SIZE on the display child) rather than waiting for a message
 * that may have been coalesced. */
static void fullscreen_repaint(HWND hwnd)
{
    layout();
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
    HMENU machine = CreatePopupMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU settings = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS_DIR,
                "Import BIOS from a &Folder...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD,
                "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_OPEN_SD, "Open &SD Folder");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");

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
    AppendMenuA(settings, MF_STRING, IDM_ROMS, "System ROMs...");
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
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!intvsession_has_system_roms(g_session)) {
            RECT c;
            GetClientRect(hwnd, &c);
            paint_nobios(hdc, &c);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* the picture or the page covers the client */
    case WM_SIZE:
        layout();
        return 0;
    case WM_DPICHANGED:
        /* PerMonitorV2 is declared in app.manifest.in, which obliges the app
         * to take the suggested rect when the window moves to a monitor at a
         * different scale. */
        {
            const RECT *sug = (const RECT *)lp;
            g_dpi = HIWORD(wp);
            make_fonts();
            if (g_big_icon)
                DestroyIcon(g_big_icon);
            g_big_icon = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                           MAKEINTRESOURCEA(IDI_APPICON),
                                           IMAGE_ICON, S(64), S(64), 0);
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                        sug->right - sug->left, sug->bottom - sug->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
        }
        return 0;
    case WM_TIMER:
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
        case IDM_ABOUT: {
            char text[512];
            MSGBOXPARAMSA mb;
            snprintf(text, sizeof(text),
                     "FujiNet Go Intv\n"
                     "Self-contained Mattel Intellivision with built-in "
                     "FujiNet.\n"
                     "Copyright (C) 2026 Thomas Cherryhomes\n"
                     "GPL-3.0-or-later\n\nDisplay: %s",
                     intv_display_renderer());
            memset(&mb, 0, sizeof(mb));
            mb.cbSize = sizeof(mb);
            mb.hwndOwner = hwnd;
            mb.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            mb.lpszText = text;
            mb.lpszCaption = "About FujiNet Go Intv";
            mb.dwStyle = MB_OK | MB_USERICON;
            mb.lpszIcon = MAKEINTRESOURCEA(IDI_APPICON);
            MessageBoxIndirectA(&mb);
            break;
        }
        case IDM_IMPORT_BIOS:
        case IDC_BIOS_FILES:
            if (intv_import_bios(hwnd, g_session, 0, 0) > 0)
                start_machine();
            break;
        case IDM_IMPORT_BIOS_DIR:
        case IDC_BIOS_FOLDER:
            if (intv_import_bios(hwnd, g_session, 1, 0) > 0)
                start_machine();
            break;
        case IDM_IMPORT_SD: intv_import_carts(hwnd, g_session); break;
        case IDM_OPEN_SD:   intv_open_sd_folder(hwnd, g_session); break;
        case IDM_ROMS:      show_roms(); break;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); break;
        default: break;
        }
        return 0;
    case WM_CLOSE:
        /* The render thread may be inside Present, which can send this
         * window's thread a message: stop it before the window goes. */
        intv_display_destroy();
        DestroyWindow(hwnd);
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
    InitCommonControls();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* the folder browser */

    g_session = intvsession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", "FujiNet Go Intv",
                    MB_ICONERROR);
        return 1;
    }
    /* Without the Executive and Graphics ROMs there is nothing to boot: the
     * session starts once they are imported (start_machine). */
    if (intvsession_has_system_roms(g_session)) {
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
    /* NULL: the picture (a child window) or the No-BIOS page covers the
     * client; a class brush could only flash in the gap Windows erases on its
     * own before they repaint. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = "IntvMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    g_hwnd = CreateWindowExA(0, "IntvMainWindow", "FujiNet Go Intv",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 800, 650, NULL,
                             g_menu, inst, NULL);
    if (!g_hwnd)
        return 1;
    g_dpi = intv_window_dpi(g_hwnd);
    g_big_icon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_APPICON),
                                   IMAGE_ICON, S(64), S(64), 0);
    g_bios_files_btn = CreateWindowExA(
        0, "BUTTON", "Import BIOS...", WS_CHILD | BS_DEFPUSHBUTTON, 0, 0, 10,
        10, g_hwnd, (HMENU)(INT_PTR)IDC_BIOS_FILES, inst, NULL);
    g_bios_folder_btn = CreateWindowExA(
        0, "BUTTON", "Import a Folder...", WS_CHILD | BS_PUSHBUTTON, 0, 0, 10,
        10, g_hwnd, (HMENU)(INT_PTR)IDC_BIOS_FOLDER, inst, NULL);
    make_fonts();
    {
        /* a 4:3 client, at this monitor's DPI */
        SIZE outer = intv_outer_size(g_hwnd, S(768), S(576), TRUE);
        SetWindowPos(g_hwnd, NULL, 0, 0, outer.cx, outer.cy,
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    g_display = intv_display_create(g_hwnd, g_session);
    show_page();
    ShowWindow(g_hwnd, show);

    /* Windows' default timer granularity is ~15.6 ms, which clamped every
     * sub-frame sleep jzIntv's rate control asks for (its plat_delay uses a
     * plain CreateWaitableTimer), so the emulator itself ran in ~15.6 ms
     * steps and declared dropped frames. One call fixes it, and since
     * Windows 10 2004 it affects only this process. */
    timeBeginPeriod(1);
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
    if (getenv("INTV_OPEN_ROMS"))
        show_roms();

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

    KillTimer(g_hwnd, TIMER_SYSACT);
    timeEndPeriod(1);
    intvsession_free(g_session);
    return (int)msg.wParam;
}
