/*
 * Debugger window (Win32): disassembly with click-to-toggle breakpoints,
 * read-only registers, a memory view, and an STIC tab (BACKTAB, all 8
 * MOBs, GRAM/GROM card sheet with pager, palette, decoded state) -- the
 * same content as the GNOME/KDE debugger windows, built from plain common
 * controls (a tab control, EDIT/STATIC/BUTTON children, and a small custom
 * window class for the RGBA pictures).
 *
 * Refreshing is a WM_TIMER poll (100ms), not a callback: intvdebug.h has
 * no stop-callback mechanism at all (unlike the CoCo/MSX ports' own
 * cocodebug/msxdebug, which fire one on the emulator thread and marshal it
 * with PostMessage) -- the engine stops the machine synchronously inside
 * cp1600_instr_tick on the emulator thread, and this window watches
 * intvdebug_stop_serial()/intvdebug_is_paused() on its own timer instead,
 * same reasoning the GNOME/KDE ports' own debugger windows document.
 *
 * All intvstic_render_* outputs are RGBA8888 (red byte first); GDI's
 * 32bpp BI_RGB DIB format wants BGRX (blue byte first) in memory, so each
 * is converted with rgba_to_bgrx before StretchDIBits, unlike the CoCo/MSX
 * Windows debuggers' own VDP pictures, which already arrive pre-converted
 * from their own core.
 *
 * The symbol table (core/debugger/symbols.h) is not owned by intvdebug --
 * unlike msxdebug's own -- so this window creates and owns its own
 * intvsymtab instance purely for disassembly/memory annotation, destroyed
 * when the window closes. It ships pre-seeded with EXEC ROM/RAM and cart-
 * header symbols (see symbols.c); "Load Symbols..." loads a per-game
 * as1600 .sym file on top, matching the GNOME/KDE ports' own UI. No
 * register/memory editing either: intvdebug has no register-write call.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvdebug.h"
#include "intvstic.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_WORDS_PER_ROW 8
#define MEM_ROWS 16
#define CARD_ROWS_PER_PAGE 8 /* 128 cards/page at CARDS_PER_ROW=16 */

#define WM_DBG_ACCEPT (WM_APP + 1) /* wp: control id -- Enter in an edit */

#define TIMER_REFRESH 1

#define PAGE_CPU 0
#define PAGE_STIC 1

enum {
    IDC_PAUSE = 1000, IDC_STEP, IDC_STEP_OVER, IDC_STEP_OUT, IDC_CLEAR_BP,
    IDC_LOAD_SYMS, IDC_CLEAR_SYMS,
    IDC_STATUS, IDC_TABS,
    IDC_DISASM, IDC_FLAGS,
    IDC_MEM_ADDR, IDC_MEM_VIEW,
    IDC_BACKTAB, IDC_MOB0, IDC_MOB_LAST = IDC_MOB0 + 7, IDC_MOB_INFO,
    IDC_CARDS, IDC_CARDS_PREV, IDC_CARDS_NEXT, IDC_PALETTE, IDC_STIC_STATE,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 7,
    IDC_LABEL_FIRST
};

typedef struct {
    const uint8_t *bgrx; /* owned by the debugger struct */
    int w, h;
} pixel_view;

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND pause_btn, step_btn, step_over_btn, step_out_btn, clear_bp_btn;
    HWND load_syms_btn, clear_syms_btn;
    HWND status;

    HWND disasm;
    HWND reg_label[8], reg_edit[8], flags;
    HWND mem_label, mem_addr, mem_view;

    HWND backtab_label, backtab_view;
    HWND mob_label, mob_view[INTVSTIC_MOB_COUNT], mob_info;
    HWND cards_label, cards_view, cards_prev, cards_next, cards_range;
    HWND pal_label, pal_view;
    HWND state_label, stic_state;

    pixel_view backtab_pv, mob_pv[INTVSTIC_MOB_COUNT], cards_pv, pal_pv;
    uint8_t backtab_bgrx[INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4];
    uint8_t mob_bgrx[INTVSTIC_MOB_COUNT][16 * 16 * 4];
    uint8_t cards_bgrx[INTVSTIC_CARDS_PER_ROW * 8 * CARD_ROWS_PER_PAGE * 8 * 4];
    uint8_t pal_bgrx[INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4];
    int card_first;
    int card_total;

    intvsession *session;
    intvdebug *dbg;
    intvsymtab *symtab;
    HFONT mono;
    int page;
    uint64_t seen_serial;
    int was_paused;
} debugger;

static debugger *g_dbg;
static const char *const kRegNames[8] = {"R0", "R1", "R2", "R3",
                                         "R4", "R5", "R6(SP)", "R7(PC)"};

/* RGBA8888 (red first) -> BGRX (blue first), the byte order GDI's 32bpp
 * BI_RGB DIB format expects in memory. Alpha is dropped -- every
 * intvstic_render_* output is either fully opaque or, for MOBs, uses alpha
 * for transparency that this debugger view does not attempt to composite
 * (it always draws over its own black background instead). */
static void rgba_to_bgrx(const uint8_t *rgba, uint8_t *bgrx, int w, int h)
{
    int i, n = w * h;
    for (i = 0; i < n; i++) {
        bgrx[i * 4 + 0] = rgba[i * 4 + 2];
        bgrx[i * 4 + 1] = rgba[i * 4 + 1];
        bgrx[i * 4 + 2] = rgba[i * 4 + 0];
        bgrx[i * 4 + 3] = 0;
    }
}

/* ---- pixel view (STIC images) --------------------------------------------- */

static LRESULT CALLBACK pixels_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    pixel_view *pv = (pixel_view *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c;
        BITMAPINFO bmi;

        GetClientRect(hwnd, &c);
        if (pv && pv->bgrx) {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = pv->w;
            bmi.bmiHeader.biHeight = -pv->h; /* top-down */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, 0, 0, c.right, c.bottom, 0, 0, pv->w, pv->h,
                         pv->bgrx, &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit/disasm subclassing ----------------------------------------------- */

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_DBG_ACCEPT,
                    (WPARAM)GetWindowLongPtrA(hwnd, GWLP_ID), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
        return 0;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg && intvdebug_is_paused(g_dbg->dbg)) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        char buf[256];
        unsigned addr;
        int n;

        *(WORD *)buf = (WORD)(sizeof(buf) - 1);
        n = (int)SendMessageA(hwnd, EM_GETLINE, (WPARAM)line, (LPARAM)buf);
        if (n >= 6) {
            buf[n] = '\0';
            if (sscanf(buf + 1, "%4X", &addr) == 1 && addr <= 0xFFFF) {
                intvdebug_breakpoint_toggle(g_dbg->dbg, (uint16_t)addr);
                g_dbg->seen_serial = 0;
            }
        }
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------- */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style,
                  int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
        (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)d->mono, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id)
{
    return child(d, "EDIT", "",
                WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                id);
}

static HWND field(debugger *d, int id)
{
    HWND h = child(d, "EDIT", "", WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, id);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                             (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, WS_VISIBLE | SS_LEFT,
                IDC_LABEL_FIRST + next_id++);
}

static HWND pixels(debugger *d, int id, pixel_view *pv, const uint8_t *bgrx,
                   int w, int h)
{
    HWND view = child(d, "IntvDbgPixels", "", WS_VISIBLE, id);
    pv->bgrx = bgrx;
    pv->w = w;
    pv->h = h;
    SetWindowLongPtrA(view, GWLP_USERDATA, (LONG_PTR)pv);
    return view;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE, IDC_TABS);
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (char *)"CPU";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 0, (LPARAM)&item);
    item.pszText = (char *)"STIC";
    SendMessageA(d->tabs, TCM_INSERTITEMA, 1, (LPARAM)&item);

    d->pause_btn = child(d, "BUTTON", "Pause (F5)",
                        WS_VISIBLE | BS_PUSHBUTTON, IDC_PAUSE);
    d->step_btn = child(d, "BUTTON", "Step (F7)",
                       WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP);
    d->step_over_btn = child(d, "BUTTON", "Step Over (F8)",
                            WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP_OVER);
    d->step_out_btn = child(d, "BUTTON", "Step Out (Shift+F8)",
                           WS_VISIBLE | BS_PUSHBUTTON, IDC_STEP_OUT);
    d->clear_bp_btn = child(d, "BUTTON", "Clear Breakpoints",
                           WS_VISIBLE | BS_PUSHBUTTON, IDC_CLEAR_BP);
    d->load_syms_btn = child(d, "BUTTON", "Load Symbols...",
                             WS_VISIBLE | BS_PUSHBUTTON, IDC_LOAD_SYMS);
    d->clear_syms_btn = child(d, "BUTTON", "Clear Symbols",
                              WS_VISIBLE | BS_PUSHBUTTON, IDC_CLEAR_SYMS);
    d->status = child(d, "STATIC", "Running", WS_VISIBLE | SS_RIGHT,
                     IDC_STATUS);

    /* CPU page */
    d->disasm = mono_view(d, IDC_DISASM);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC,
                                               (LONG_PTR)disasm_subclass);
    for (i = 0; i < 8; i++) {
        d->reg_label[i] = label(d, kRegNames[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i);
        EnableWindow(d->reg_edit[i], FALSE); /* read-only: see file header */
    }
    d->flags = label(d, "");
    d->mem_label = label(d, "Memory");
    d->mem_addr = field(d, IDC_MEM_ADDR);
    SetWindowTextA(d->mem_addr, "0000");
    d->mem_view = mono_view(d, IDC_MEM_VIEW);

    /* STIC page */
    d->backtab_label = label(d, "BACKTAB");
    d->backtab_view = pixels(d, IDC_BACKTAB, &d->backtab_pv, d->backtab_bgrx,
                             INTVSTIC_BACKTAB_W, INTVSTIC_BACKTAB_H);
    d->mob_label = label(d, "MOBs");
    for (i = 0; i < INTVSTIC_MOB_COUNT; i++)
        d->mob_view[i] = pixels(d, IDC_MOB0 + i, &d->mob_pv[i],
                                d->mob_bgrx[i], 16, 16);
    d->mob_info = mono_view(d, IDC_MOB_INFO);
    d->cards_label = label(d, "GRAM/GROM cards");
    d->cards_view = pixels(d, IDC_CARDS, &d->cards_pv, d->cards_bgrx,
                           INTVSTIC_CARDS_PER_ROW * 8, CARD_ROWS_PER_PAGE * 8);
    d->cards_prev = child(d, "BUTTON", "< Prev", WS_VISIBLE | BS_PUSHBUTTON,
                          IDC_CARDS_PREV);
    d->cards_next = child(d, "BUTTON", "Next >", WS_VISIBLE | BS_PUSHBUTTON,
                          IDC_CARDS_NEXT);
    d->cards_range = label(d, "");
    d->pal_label = label(d, "Palette");
    d->pal_view = pixels(d, IDC_PALETTE, &d->pal_pv, d->pal_bgrx,
                        INTVSTIC_PAL_CELL * 16, INTVSTIC_PAL_CELL);
    d->state_label = label(d, "Registers & mode");
    d->stic_state = mono_view(d, IDC_STIC_STATE);
}

/* ---- layout ------------------------------------------------------------------
 * A fixed, manually-computed grid rather than a resizable one -- matches
 * the scope of the other three frontends' debugger windows (none offer
 * fluid resizing of their internal panes either). Recomputed on WM_SIZE. */

static void show_page(debugger *d, int page)
{
    int i;
    d->page = page;
    ShowWindow(d->disasm, page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    for (i = 0; i < 8; i++) {
        ShowWindow(d->reg_label[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
        ShowWindow(d->reg_edit[i], page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(d->flags, page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    ShowWindow(d->mem_label, page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    ShowWindow(d->mem_addr, page == PAGE_CPU ? SW_SHOW : SW_HIDE);
    ShowWindow(d->mem_view, page == PAGE_CPU ? SW_SHOW : SW_HIDE);

    ShowWindow(d->backtab_label, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->backtab_view, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->mob_label, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    for (i = 0; i < INTVSTIC_MOB_COUNT; i++)
        ShowWindow(d->mob_view[i], page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->mob_info, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cards_label, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cards_view, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cards_prev, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cards_next, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->cards_range, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->pal_label, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->pal_view, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->state_label, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
    ShowWindow(d->stic_state, page == PAGE_STIC ? SW_SHOW : SW_HIDE);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, i;

    GetClientRect(d->hwnd, &client);

    y = 4;
    MoveWindow(d->pause_btn, 4, y, 110, 26, TRUE);
    MoveWindow(d->step_btn, 118, y, 90, 26, TRUE);
    MoveWindow(d->step_over_btn, 212, y, 110, 26, TRUE);
    MoveWindow(d->step_out_btn, 326, y, 140, 26, TRUE);
    MoveWindow(d->clear_bp_btn, 470, y, 130, 26, TRUE);
    MoveWindow(d->load_syms_btn, 604, y, 120, 26, TRUE);
    MoveWindow(d->clear_syms_btn, 728, y, 110, 26, TRUE);
    MoveWindow(d->status, client.right - 210, y + 4, 200, 20, TRUE);
    y += 32;

    MoveWindow(d->tabs, 4, y, client.right - 8, client.bottom - y - 4, TRUE);
    page = client;
    page.top = y;
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);

    /* CPU page */
    MoveWindow(d->disasm, page.left, page.top,
              (page.right - page.left) * 2 / 3, page.bottom - page.top, TRUE);
    x = page.left + (page.right - page.left) * 2 / 3 + 8;
    {
        int ry = page.top;
        for (i = 0; i < 8; i++) {
            MoveWindow(d->reg_label[i], x + (i % 2) * 100, ry, 60, 18, TRUE);
            MoveWindow(d->reg_edit[i], x + (i % 2) * 100 + 60, ry, 60, 20,
                      TRUE);
            if (i % 2 == 1)
                ry += 24;
        }
        ry += 8;
        MoveWindow(d->flags, x, ry, 260, 40, TRUE);
        ry += 48;
        MoveWindow(d->mem_label, x, ry, 100, 18, TRUE);
        ry += 20;
        MoveWindow(d->mem_addr, x, ry, 100, 22, TRUE);
        ry += 26;
        MoveWindow(d->mem_view, x, ry, page.right - x - 8,
                  page.bottom - ry - 4, TRUE);
    }

    /* STIC page */
    {
        int sx = page.left, sy = page.top;
        MoveWindow(d->backtab_label, sx, sy, 200, 18, TRUE);
        MoveWindow(d->backtab_view, sx, sy + 20, INTVSTIC_BACKTAB_W * 2,
                  INTVSTIC_BACKTAB_H * 2, TRUE);

        int mx = sx + INTVSTIC_BACKTAB_W * 2 + 12;
        MoveWindow(d->mob_label, mx, sy, 200, 18, TRUE);
        for (i = 0; i < INTVSTIC_MOB_COUNT; i++)
            MoveWindow(d->mob_view[i], mx + (i % 4) * 36, sy + 20 + (i / 4) * 36,
                      32, 32, TRUE);
        MoveWindow(d->mob_info, mx, sy + 96, 340, 130, TRUE);

        int cy = sy + INTVSTIC_BACKTAB_H * 2 + 28;
        MoveWindow(d->cards_label, sx, cy, 200, 18, TRUE);
        MoveWindow(d->cards_view, sx, cy + 20, INTVSTIC_CARDS_PER_ROW * 8 * 2,
                  CARD_ROWS_PER_PAGE * 8 * 2, TRUE);
        MoveWindow(d->cards_prev, sx,
                  cy + 24 + CARD_ROWS_PER_PAGE * 8 * 2, 70, 24, TRUE);
        MoveWindow(d->cards_next, sx + 74,
                  cy + 24 + CARD_ROWS_PER_PAGE * 8 * 2, 70, 24, TRUE);
        MoveWindow(d->cards_range, sx + 150,
                  cy + 28 + CARD_ROWS_PER_PAGE * 8 * 2, 160, 18, TRUE);

        MoveWindow(d->pal_label, mx, cy, 200, 18, TRUE);
        MoveWindow(d->pal_view, mx, cy + 20, INTVSTIC_PAL_CELL * 16,
                  INTVSTIC_PAL_CELL, TRUE);

        MoveWindow(d->state_label, sx, cy + 200, 200, 18, TRUE);
        MoveWindow(d->stic_state, sx, cy + 220, page.right - sx - 8,
                  page.bottom - (cy + 220) - 4, TRUE);
    }
}

/* ---- refreshers --------------------------------------------------------------- */

static void refresh_disasm(debugger *d)
{
    intvdebug_dasm_line lines[DISASM_LINES];
    intvdebug_regs r;
    char text[16384];
    int n, i, len = 0;

    if (!intvdebug_regs_get(d->dbg, &r)) {
        SetWindowTextA(d->disasm, "");
        return;
    }
    n = intvdebug_disassemble(d->dbg, r.pc, r.D, lines, DISASM_LINES);
    for (i = 0; i < n && len < (int)sizeof(text) - 128; i++) {
        char symsuffix[96];
        intvsym_annotate_line(d->symtab, &lines[i], symsuffix,
                              sizeof(symsuffix));
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                       "%c%c%04X  %-24s%s\r\n",
                       lines[i].addr == r.pc ? '>' : ' ',
                       intvdebug_breakpoint_is_set(d->dbg, lines[i].addr)
                           ? '*'
                           : ' ',
                       lines[i].addr, lines[i].text, symsuffix);
    }
    SetWindowTextA(d->disasm, text);
}

static void refresh_regs(debugger *d)
{
    intvdebug_regs r;
    char buf[16];
    char flagbuf[48];
    int i;

    if (!intvdebug_regs_get(d->dbg, &r)) {
        for (i = 0; i < 8; i++) SetWindowTextA(d->reg_edit[i], "");
        SetWindowTextA(d->flags, "");
        return;
    }
    for (i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "%04X", r.r[i]);
        SetWindowTextA(d->reg_edit[i], buf);
    }
    snprintf(flagbuf, sizeof(flagbuf), "S%d C%d O%d Z%d I%d D%d", r.S, r.C,
             r.O, r.Z, r.I, r.D);
    SetWindowTextA(d->flags, flagbuf);
}

static void refresh_mem(debugger *d)
{
    uint16_t addr = 0;
    uint16_t words[MEM_WORDS_PER_ROW * MEM_ROWS];
    char text[8192];
    int n, row, col, len = 0;

    {
        char buf[16];
        GetWindowTextA(d->mem_addr, buf, sizeof(buf));
        sscanf(buf, "%4hX", &addr);
    }
    n = intvdebug_read(d->dbg, addr, words, MEM_WORDS_PER_ROW * MEM_ROWS);
    {
        const char *last_region = NULL;
        for (row = 0; row * MEM_WORDS_PER_ROW < n; row++) {
            uint16_t row_addr = (uint16_t)(addr + row * MEM_WORDS_PER_ROW);
            const char *region = intvsym_region(row_addr);

            len += snprintf(text + len, sizeof(text) - (size_t)len, "%04X ",
                           row_addr);
            for (col = 0; col < MEM_WORDS_PER_ROW; col++) {
                int idx = row * MEM_WORDS_PER_ROW + col;
                if (idx < n)
                    len += snprintf(text + len, sizeof(text) - (size_t)len,
                                   " %04X", words[idx]);
            }
            /* Only label the first row of a run in the same region -- see
             * symbols.c's k_regions and the GNOME frontend's own
             * refresh_mem. */
            if (region && region != last_region)
                len += snprintf(text + len, sizeof(text) - (size_t)len,
                               "  ; %s", region);
            last_region = region;
            len += snprintf(text + len, sizeof(text) - (size_t)len, "\r\n");
        }
    }
    SetWindowTextA(d->mem_view, text);
}

/* Shared scratch buffer for all four STIC renders below -- sized to the
 * largest of them (the palette sheet, INTVSTIC_PAL_CELL*16 wide by
 * INTVSTIC_PAL_CELL tall), not just the cards sheet. A buffer sized only
 * for the cards sheet caused intvstic_render_backtab/_palette to overrun it
 * by tens of KB into adjacent .bss, corrupting g_dbg itself -- caught via a
 * real Wine crash (a garbage read at intv_debugger_pretranslate, traced
 * back to pixel data bleeding into g_dbg's storage). */
#define STIC_SCRATCH_BYTES_A (INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4)
#define STIC_SCRATCH_BYTES_B \
    (INTVSTIC_CARDS_PER_ROW * 8 * CARD_ROWS_PER_PAGE * 8 * 4)
#define STIC_SCRATCH_BYTES_C (INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4)
#define STIC_SCRATCH_BYTES \
    (STIC_SCRATCH_BYTES_A > STIC_SCRATCH_BYTES_B \
         ? (STIC_SCRATCH_BYTES_A > STIC_SCRATCH_BYTES_C ? STIC_SCRATCH_BYTES_A \
                                                        : STIC_SCRATCH_BYTES_C) \
         : (STIC_SCRATCH_BYTES_B > STIC_SCRATCH_BYTES_C ? STIC_SCRATCH_BYTES_B \
                                                        : STIC_SCRATCH_BYTES_C))

static void refresh_stic(debugger *d)
{
    static intvstic_snapshot snap;
    static uint8_t rgba_scratch[STIC_SCRATCH_BYTES];
    char text[2048];
    char mobtext[2048];
    int len = 0, i, total;

    intvstic_snapshot_get(d->dbg, &snap);

    intvstic_render_backtab(&snap, rgba_scratch);
    rgba_to_bgrx(rgba_scratch, d->backtab_bgrx, INTVSTIC_BACKTAB_W,
                INTVSTIC_BACKTAB_H);
    InvalidateRect(d->backtab_view, NULL, FALSE);

    len = snprintf(mobtext, sizeof(mobtext),
                  "## X   Y  CARD CLR SZx SZy VIS PRI GRAM COLL\r\n");
    for (i = 0; i < INTVSTIC_MOB_COUNT; i++) {
        intvstic_mob mob;
        intvstic_mob_get(&snap, i, &mob);
        intvstic_render_mob(&snap, i, rgba_scratch);
        rgba_to_bgrx(rgba_scratch, d->mob_bgrx[i], 16, 16);
        InvalidateRect(d->mob_view[i], NULL, FALSE);
        len += snprintf(mobtext + len, sizeof(mobtext) - (size_t)len,
                       "%d  %3d %3d  %3d  %2d  %dx  %dx  %d   %d   %d    "
                       "%03X\r\n",
                       i, mob.x, mob.y, mob.card, mob.color, mob.x_size,
                       mob.y_stretch, mob.visible, mob.priority, mob.gram,
                       mob.collision);
    }
    SetWindowTextA(d->mob_info, mobtext);

    total = intvstic_card_count(&snap);
    d->card_total = total;
    if (d->card_first >= total)
        d->card_first = 0;
    intvstic_render_cards(&snap, d->card_first, CARD_ROWS_PER_PAGE,
                          rgba_scratch);
    rgba_to_bgrx(rgba_scratch, d->cards_bgrx, INTVSTIC_CARDS_PER_ROW * 8,
                CARD_ROWS_PER_PAGE * 8);
    InvalidateRect(d->cards_view, NULL, FALSE);
    {
        int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
        int last_shown =
            (d->card_first + page < total ? d->card_first + page : total) - 1;
        char range_text[64];
        snprintf(range_text, sizeof(range_text), "cards %d-%d of %d",
                d->card_first, last_shown, total);
        SetWindowTextA(d->cards_range, range_text);
    }

    intvstic_render_palette(d->dbg, rgba_scratch);
    rgba_to_bgrx(rgba_scratch, d->pal_bgrx, INTVSTIC_PAL_CELL * 16,
                INTVSTIC_PAL_CELL);
    InvalidateRect(d->pal_view, NULL, FALSE);

    intvstic_format_state(&snap, text, (int)sizeof(text));
    SetWindowTextA(d->stic_state, text);
}

static void refresh_all(debugger *d)
{
    int paused = intvdebug_is_paused(d->dbg);
    SetWindowTextA(d->pause_btn, paused ? "Continue (F5)" : "Pause (F5)");
    SetWindowTextA(d->status, paused ? "Paused" : "Running");

    if (paused) {
        refresh_regs(d);
        refresh_disasm(d);
        refresh_mem(d);
        refresh_stic(d);
    } else {
        int i;
        for (i = 0; i < 8; i++) SetWindowTextA(d->reg_edit[i], "");
        SetWindowTextA(d->flags, "");
        SetWindowTextA(d->disasm, "");
        SetWindowTextA(d->mem_view, "");
    }
    d->was_paused = paused;
}

/* ---- window procedure ---------------------------------------------------------- */

static LRESULT CALLBACK dbg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = (debugger *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SIZE:
        if (d) layout(d);
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (d && nm->hwndFrom == d->tabs && nm->code == TCN_SELCHANGE)
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_TIMER:
        if (d) {
            uint64_t serial = intvdebug_stop_serial(d->dbg);
            if (serial != d->seen_serial ||
                intvdebug_is_paused(d->dbg) != d->was_paused) {
                d->seen_serial = serial;
                refresh_all(d);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        if (d && wp == IDC_MEM_ADDR) {
            d->seen_serial = 0;
        }
        return 0;
    case WM_COMMAND:
        if (!d)
            break;
        switch (LOWORD(wp)) {
        case IDC_PAUSE:
            if (intvdebug_is_paused(d->dbg))
                intvdebug_resume(d->dbg);
            else
                intvdebug_pause(d->dbg);
            return 0;
        case IDC_STEP: intvdebug_step(d->dbg); return 0;
        case IDC_STEP_OVER: intvdebug_step_over(d->dbg); return 0;
        case IDC_STEP_OUT: intvdebug_step_out(d->dbg); return 0;
        case IDC_CLEAR_BP:
            intvdebug_breakpoint_clear_all(d->dbg);
            d->seen_serial = 0;
            return 0;
        case IDC_LOAD_SYMS: {
            char path[MAX_PATH] = "";
            OPENFILENAMEA ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "as1600 symbol files (*.sym)\0*.sym\0"
                              "All files (*.*)\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = sizeof(path);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn) && intvsymtab_load(d->symtab, path) >= 0) {
                d->seen_serial = 0;
                if (intvdebug_is_paused(d->dbg)) refresh_all(d);
            }
            return 0;
        }
        case IDC_CLEAR_SYMS:
            intvsymtab_clear_user(d->symtab);
            d->seen_serial = 0;
            if (intvdebug_is_paused(d->dbg)) refresh_all(d);
            return 0;
        case IDC_CARDS_PREV: {
            int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
            d->card_first -= page;
            if (d->card_first < 0) d->card_first = 0;
            d->seen_serial = 0;
            if (intvdebug_is_paused(d->dbg)) refresh_stic(d);
            return 0;
        }
        case IDC_CARDS_NEXT: {
            int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
            int last_page = d->card_total > 0
                               ? ((d->card_total - 1) / page) * page
                               : 0;
            d->card_first += page;
            if (d->card_first > last_page) d->card_first = last_page;
            d->seen_serial = 0;
            if (intvdebug_is_paused(d->dbg)) refresh_stic(d);
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (d) {
            KillTimer(hwnd, TIMER_REFRESH);
            /* Releasing costs the machine nothing further (see
             * intvdebug.h) and leaving it engaged+paused would look like
             * the emulator had hung. */
            intvdebug_set_engaged(d->dbg, 0);
            intvsymtab_destroy(d->symtab);
            DeleteObject(d->mono);
            g_dbg = NULL;
            free(d);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int intv_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg)
        return 0;
    if (msg->hwnd != g_dbg->hwnd && GetParent(msg->hwnd) != g_dbg->hwnd &&
        GetAncestor(msg->hwnd, GA_ROOT) != g_dbg->hwnd)
        return 0;
    if (msg->message != WM_KEYDOWN)
        return 0;
    switch (msg->wParam) {
    case VK_F5:
        SendMessageA(g_dbg->hwnd, WM_COMMAND, IDC_PAUSE, 0);
        return 1;
    case VK_F7:
        intvdebug_step(g_dbg->dbg);
        return 1;
    case VK_F8:
        if (GetKeyState(VK_SHIFT) & 0x8000)
            intvdebug_step_out(g_dbg->dbg);
        else
            intvdebug_step_over(g_dbg->dbg);
        return 1;
    default:
        return 0;
    }
}

void intv_debugger_show(HWND parent, intvsession *session)
{
    static int registered;
    debugger *d;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    if (g_dbg) {
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    if (!registered) {
        WNDCLASSA wc, pc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = dbg_wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "IntvDebuggerWindow";
        RegisterClassA(&wc);

        memset(&pc, 0, sizeof(pc));
        pc.lpfnWndProc = pixels_proc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursor(NULL, IDC_ARROW);
        pc.lpszClassName = "IntvDbgPixels";
        RegisterClassA(&pc);

        registered = 1;
    }

    d = (debugger *)calloc(1, sizeof(*d));
    d->session = session;
    d->dbg = intvdebug_get();
    d->symtab = intvsymtab_create();
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH | FF_MODERN, "Consolas");

    d->hwnd = CreateWindowExA(0, "IntvDebuggerWindow",
                              "Intellivision Debugger", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1080, 780, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrA(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);
    g_dbg = d;

    build_controls(d);
    show_page(d, PAGE_CPU);
    layout(d);

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(d->dbg, 1);
    intvdebug_pause(d->dbg);

    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    refresh_all(d);

    ShowWindow(d->hwnd, SW_SHOW);
}
