/*
 * The System ROMs window, the other desktops' Preferences "ROMs" page: a
 * note on where the ROMs come from and where they are kept, Import... and
 * Import a Folder..., and each ROM -- under its group -- as Imported or
 * Not imported.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "roms.h"

#include <stdio.h>
#include <string.h>

#include "import.h"
#include "resource.h"
#include "win_common.h"

#define ROMS_CLASS "IntvRomsWindow"
#define ROMS_STYLE (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define MAX_ITEMS 16

/* the layout, in 96-dpi units */
#define CW 520
#define M 14
#define ROW_H 22

static HWND g_win, g_note, g_path, g_files, g_folder, g_close;
static HWND g_group[MAX_ITEMS], g_name[MAX_ITEMS], g_state[MAX_ITEMS];
static int g_rows;      /* items */
static HFONT g_font, g_bold;
static intvsession *g_session;
static intv_roms_hooks g_hooks;

static int S(int v) { return intv_scale(v, intv_window_dpi(g_win)); }

static void refresh(void)
{
    for (int i = 0; i < g_rows; i++)
        SetWindowTextA(g_state[i], intvsession_rom_item_available(g_session, i)
                                       ? "Imported" : "Not imported");
}

static void layout(void)
{
    const int m = S(M), w = S(CW) - 2 * m;
    int y = m;
    const char *last = NULL;

    MoveWindow(g_note, m, y, w, S(64), TRUE);
    y += S(68);
    MoveWindow(g_path, m, y, w, S(20), TRUE);
    y += S(28);
    MoveWindow(g_files, m, y, S(140), S(28), TRUE);
    MoveWindow(g_folder, m + S(148), y, S(160), S(28), TRUE);
    y += S(40);
    for (int i = 0; i < g_rows; i++) {
        const char *grp = intvsession_rom_item(i)->group;
        if (!last || strcmp(last, grp) != 0) {
            if (last) y += S(8);
            MoveWindow(g_group[i], m, y, w, S(ROW_H), TRUE);
            y += S(ROW_H);
            last = grp;
        } else {
            ShowWindow(g_group[i], SW_HIDE);
        }
        MoveWindow(g_name[i], m + S(16), y, w - S(16) - S(120), S(ROW_H), TRUE);
        MoveWindow(g_state[i], m + w - S(120), y, S(120), S(ROW_H), TRUE);
        y += S(ROW_H);
    }
    y += S(12);
    MoveWindow(g_close, S(CW) - m - S(96), y, S(96), S(28), TRUE);
}

static void make_fonts(void)
{
    const int dpi = intv_window_dpi(g_win);
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    g_font = intv_make_font(dpi, 0, FW_NORMAL, NULL);
    g_bold = intv_make_font(dpi, 0, FW_BOLD, NULL);
    intv_set_font_all(g_win, g_font);
    for (int i = 0; i < g_rows; i++)
        SendMessageW(g_group[i], WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* Sizes the window to its client; laid out first so the height is known. */
static void fit_window(const RECT *at)
{
    RECT c = { 0, 0, S(CW), 0 };
    SIZE outer;
    layout();
    {
        /* the client ends a margin below the Close button */
        RECT r;
        GetWindowRect(g_close, &r);
        MapWindowPoints(NULL, g_win, (POINT *)&r, 2);
        c.bottom = r.bottom + S(M);
    }
    outer = intv_outer_size(g_win, c.right, c.bottom, FALSE);
    if (at)
        SetWindowPos(g_win, NULL, at->left, at->top, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(g_win, NULL, 0, 0, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

static void do_import(int folder)
{
    if (intv_import_bios(g_win, g_session, folder, 1) < 0)
        return;
    refresh();
    if (g_hooks.imported)
        g_hooks.imported();
}

static LRESULT CALLBACK roms_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ROMS_FILES:  do_import(0); return 0;
        case IDC_ROMS_FOLDER: do_import(1); return 0;
        case IDC_ROMS_CLOSE:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_DPICHANGED:
        make_fonts();
        fit_window((const RECT *)lp);
        return 0;
    case WM_DESTROY:
        g_win = NULL;
        if (g_font) DeleteObject(g_font);
        if (g_bold) DeleteObject(g_bold);
        g_font = g_bold = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void intv_roms_show(HWND parent, intvsession *session, const intv_roms_hooks *hooks)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    char path[1024];

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_hooks = *hooks;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = roms_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"" ROMS_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        registered = RegisterClassExW(&wc) != 0;
    }
    g_win = CreateWindowExW(WS_EX_DLGMODALFRAME, L"" ROMS_CLASS, L"System ROMs", ROMS_STYLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, parent, NULL, inst, NULL);
    if (!g_win) return;

    g_note = CreateWindowW(L"STATIC",
        L"The Intellivision Executive and Graphics ROMs are not part of the app. Import them "
        L"from files, a zip (MAME's intv set works) or a folder; they are recognised by size "
        L"and checksum, whatever they are named. Kept in:",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 1, 1, g_win, NULL, inst, NULL);
    snprintf(path, sizeof path, "%s.", intvsession_roms_path(session));
    for (char *c = path; *c; c++)
        if (*c == '/') *c = '\\';
    g_path = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS,
                           0, 0, 1, 1, g_win, NULL, inst, NULL);
    intv_set_text_utf8(g_path, path);
    g_files = CreateWindowW(L"BUTTON", L"Import...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            0, 0, 1, 1, g_win, (HMENU)(INT_PTR)IDC_ROMS_FILES, inst, NULL);
    g_folder = CreateWindowW(L"BUTTON", L"Import a Folder...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             0, 0, 1, 1, g_win, (HMENU)(INT_PTR)IDC_ROMS_FOLDER, inst, NULL);
    g_rows = intvsession_rom_item_count();
    if (g_rows > MAX_ITEMS) g_rows = MAX_ITEMS;
    for (int i = 0; i < g_rows; i++) {
        const intv_rom_item *item = intvsession_rom_item(i);
        g_group[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                   0, 0, 1, 1, g_win, NULL, inst, NULL);
        intv_set_text_utf8(g_group[i], item->group);
        g_name[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  0, 0, 1, 1, g_win, NULL, inst, NULL);
        intv_set_text_utf8(g_name[i], item->name);
        g_state[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX,
                                   0, 0, 1, 1, g_win, NULL, inst, NULL);
    }
    g_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 1, 1, g_win, (HMENU)(INT_PTR)IDC_ROMS_CLOSE, inst, NULL);
    make_fonts();
    fit_window(NULL);
    refresh();
    ShowWindow(g_win, SW_SHOW);
}
