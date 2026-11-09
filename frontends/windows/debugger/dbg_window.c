/*
 * Debugger window (Win32): disassembly with click-to-toggle breakpoints,
 * read-only registers, a memory view, and an STIC tab (BACKTAB, all 8
 * MOBs, GRAM/GROM card sheet with pager, palette, decoded state) -- the
 * same content as the GNOME/KDE debugger windows, built from plain common
 * controls (a tab control, EDIT/STATIC/BUTTON children, and a small custom
 * window class for the RGBA pictures).
 *
 * A FujiNet tab, last, shows every FujiBus packet between the machine and
 * FujiNet (intvdebug_fujinet_trace, always on) beside the mailbox's state;
 * the timer also refreshes it twice a second while it shows, since it is
 * the one view that changes while the machine runs. Its text arrives as
 * UTF-8 and is shown as ASCII, like everything else in this ANSI window.
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
 * unlike msxdebug's own -- so this window uses the process-wide
 * intvsymtab_shared() instead of its own instance: a cart arrives through
 * FujiNet CONFIG with no path a .sym could ever be derived from, so a user
 * loading one by hand is the only way symbols get here at all, and that
 * work should survive closing this window, not be thrown away with it. It
 * ships pre-seeded with EXEC ROM/RAM and cart-header symbols (see
 * symbols.c); "Load Symbols..." loads a per-game as1600 .sym file on top,
 * matching the GNOME/KDE ports' own UI. No register/memory editing
 * either: intvdebug has no register-write call.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvdebug.h"
#include "intvstic.h"
#include "../accent_button.h"
#include "../help/help_window.h"
#include "memtools.h"
#include "srcmap.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_WORDS_PER_ROW 8
#define MEM_ROWS 16
#define CARD_ROWS_PER_PAGE 8 /* 128 cards/page at CARDS_PER_ROW=16 */

#define WM_DBG_ACCEPT (WM_APP + 1) /* wp: control id -- Enter in an edit */
#define WM_DBG_LINE (WM_APP + 2)   /* wp: control id, lp: the clicked line's value */

#define TIMER_REFRESH 1

#define PAGE_CPU 0
#define PAGE_BASIC 1
#define PAGE_MEMORY 2
#define PAGE_BREAKS 3
#define PAGE_WATCHES 4
#define PAGE_CART 5
#define PAGE_TRACE 6
#define PAGE_HISTORY 7
#define PAGE_STIC 8
#define PAGE_SYMBOLS 9
#define PAGE_FUJINET 10
#define PAGE_COUNT 11

#define FUJI_TICKS 5 /* the FujiNet tab, every 500ms while it shows */

enum {
    IDC_PAUSE = 1000, IDC_STEP, IDC_STEP_OVER, IDC_STEP_OUT, IDC_CLEAR_BP,
    IDC_LOAD_SYMS, IDC_CLEAR_SYMS, IDC_HELP_BTN,
    IDC_BAS_FILE, IDC_BAS_FOLDER, IDC_BAS_MAP, IDC_BAS_FILES, IDC_BAS_VIEW,
    IDC_BP_TYPE, IDC_BP_ADDR, IDC_BP_COND, IDC_BP_VALUE, IDC_BP_ADD, IDC_BP_LIST,
    IDC_BP_TOGGLE, IDC_BP_REMOVE, IDC_BP_HITS, IDC_BP_CLEAR, IDC_BP_STOP,
    IDC_WT_ENTRY, IDC_WT_ADD, IDC_WT_LIST, IDC_WT_MEM, IDC_WT_BRK, IDC_WT_REMOVE,
    IDC_SRCH_KIND, IDC_SRCH_ENTRY, IDC_SRCH_NEXT, IDC_SRCH_PREV, IDC_SRCH_NOTE,
    IDC_CART_TEXT, IDC_POPOUT, IDC_DOCK,
    IDC_H_REC0, IDC_H_REC1, IDC_H_FILTER0, IDC_H_FILTER1, IDC_H_RANGE0, IDC_H_RANGE1,
    IDC_H_CLEAR0, IDC_H_CLEAR1, IDC_H_NOTE0, IDC_H_NOTE1, IDC_H_TEXT0, IDC_H_TEXT1,
    IDC_STATUS, IDC_TABS,
    IDC_DISASM, IDC_FLAGS,
    IDC_MEM_ADDR, IDC_MEM_VIEW,
    IDC_BREAK_ADDR,
    IDC_BACKTAB, IDC_MOB0, IDC_MOB_LAST = IDC_MOB0 + 7, IDC_MOB_INFO,
    IDC_CARDS, IDC_CARDS_PREV, IDC_CARDS_NEXT, IDC_PALETTE, IDC_STIC_STATE,
    IDC_SYM_FILTER, IDC_SYM_LIST, IDC_SYM_COUNT,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 7,
    IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
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
    HWND load_syms_btn, clear_syms_btn, help_btn;
    HWND status;

    HWND disasm;
    HWND reg_label[8], reg_edit[8], flags;
    HWND mem_label, mem_addr, mem_view;
    HWND break_label, break_addr;
    uint16_t mem_base;

    /* BASIC page: the imported IntyBASIC source (core/debugger/srcmap.h) */
    HWND bas_file_btn, bas_folder_btn, bas_map_btn, bas_files, bas_note, bas_view;

    /* Memory page: the highlighted words and the search */
    int mem_hl;
    uint16_t mem_hl_lo, mem_hl_hi;
    HWND srch_label, srch_kind, srch_entry, srch_next, srch_prev, srch_note;

    /* Breakpoints and Watches pages */
    HWND bp_type, bp_addr, bp_cond, bp_value, bp_add, bp_list;
    HWND bp_toggle, bp_remove, bp_hits, bp_clear, bp_stop;
    unsigned bp_gen;
    HWND wt_label, wt_entry, wt_add, wt_list, wt_mem, wt_brk, wt_remove;

    /* Cart page; Trace (0, live) and History (1, when stopped) share one ring */
    HWND cart_text;
    HWND h_rec[2], h_filter[2], h_range[2], h_clear[2], h_note[2], h_text[2];
    uint64_t h_seen[2];
    int h_ticks;

    /* Each page's controls are children of a container of its own, so a page
     * can be peeled off into a window (SetParent) and put back. */
    HWND pg[PAGE_COUNT];
    HWND popped[PAGE_COUNT];     /* the window a page lives in, or NULL */
    int tab_page[PAGE_COUNT];    /* the page at each tab-control index */
    int ntabs;
    HWND new_parent;             /* where child() puts the next control */
    HWND popout_btn;
    int bas_file;       /* index into the map; -1 = nothing shown */
    unsigned bas_gen;   /* intvsrcmap_generation() the view was built from */
    int bas_pc_line;    /* the machine's line in the file shown, 0 = none */
    uint64_t bas_serial; /* the stop the view last followed */

    HWND backtab_label, backtab_view;
    HWND mob_label, mob_view[INTVSTIC_MOB_COUNT], mob_info;
    HWND cards_label, cards_view, cards_prev, cards_next, cards_range;
    HWND pal_label, pal_view;
    HWND state_label, stic_state;

    HWND sym_filter_label, sym_filter, sym_list, sym_count;
    unsigned sym_seen_gen;
    char sym_filter_text[64];

    HWND fuji_title, fuji_note, fuji_clear, fuji_text;
    HWND fuji_state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;
    int fuji_ticks;

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
    intvsrcmap *srcmap;
    HFONT mono;
    int mono_h; /* a text line's height in mono, for the logs */
    int page;
    uint64_t seen_serial;
    int was_paused;
} debugger;

static debugger *g_dbg;
static const char *const kRegNames[8] = {"R0", "R1", "R2", "R3",
                                         "R4", "R5", "R6(SP)", "R7(PC)"};

/* The tabs, in PAGE_* order; INTV_DEBUGGER_TAB picks one by number or by the
 * start of its name (any case), for tests and captures. */
static const char *const k_tab_names[] = {"CPU", "BASIC", "Memory", "Breakpoints",
                                          "Watches", "Cart", "Trace", "History", "STIC", "Symbols", "FujiNet"};
#define TAB_COUNT ((int)(sizeof(k_tab_names) / sizeof(k_tab_names[0])))

static int tab_from_env_var(const char *var)
{
    const char *tab = getenv(var);
    char *end;
    long n;
    int i;

    if (!tab || !*tab)
        return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < TAB_COUNT ? (int)n : -1;
    for (i = 0; i < TAB_COUNT; i++)
        if (_strnicmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return i;
    return -1;
}

static int tab_from_env(void)
{
    return tab_from_env_var("INTV_DEBUGGER_TAB");
}

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
static WNDPROC g_sym_list_proc;

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

/* ---- line view (the disassembly and the BASIC source) ---------------------
 * An EDIT control cannot paint a current-line bar or a red dot, so these two
 * views are a small window class: it keeps the lines it was given (text, a
 * LV_PC / LV_BP flag set, and a value handed back on a click), paints the
 * current line as a bar in the accent colour, a breakpoint as a red dot in
 * the gutter, and scrolls on its own scroll bar. A click posts WM_DBG_LINE
 * to the parent. */

#define LV_PC 1
#define LV_BP 2
#define LV_BP_OFF 4   /* a disabled breakpoint: a hollow grey dot */
#define LV_GUTTER 22

typedef struct {
    int n, cap, first;
    char **text;
    unsigned char *flags;
    int *aux;
    HFONT font;
} lineview;

static lineview *lv_of(HWND h)
{
    return (lineview *)GetWindowLongPtrA(h, GWLP_USERDATA);
}

static int lv_row_h(HWND h, lineview *lv)
{
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, lv->font ? lv->font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICA tm;
    int rh = 15;
    if (GetTextMetricsA(dc, &tm))
        rh = tm.tmHeight + 2;
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return rh;
}

static int lv_rows(HWND h, lineview *lv)
{
    RECT c;
    int rows;
    GetClientRect(h, &c);
    rows = c.bottom / lv_row_h(h, lv);
    return rows < 1 ? 1 : rows;
}

static void lv_sync(HWND h, lineview *lv)
{
    SCROLLINFO si;
    const int rows = lv_rows(h, lv);
    int max_first = lv->n - rows;

    if (max_first < 0)
        max_first = 0;
    if (lv->first > max_first)
        lv->first = max_first;
    if (lv->first < 0)
        lv->first = 0;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = lv->n > 0 ? lv->n - 1 : 0;
    si.nPage = (UINT)rows;
    si.nPos = lv->first;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
    InvalidateRect(h, NULL, FALSE);
}

static void lv_begin(HWND h)
{
    lineview *lv = lv_of(h);
    int i;
    if (!lv)
        return;
    for (i = 0; i < lv->n; i++)
        free(lv->text[i]);
    lv->n = 0;
}

static void lv_add(HWND h, const char *text, int flags, int aux)
{
    lineview *lv = lv_of(h);
    char buf[512];
    size_t o = 0;

    if (!lv)
        return;
    for (; *text && o + 5 < sizeof(buf); text++) { /* tabs -> 4 spaces */
        if (*text == '\t') {
            memcpy(buf + o, "    ", 4);
            o += 4;
        } else if (*text != '\r' && *text != '\n') {
            buf[o++] = *text;
        }
    }
    buf[o] = '\0';
    if (lv->n == lv->cap) {
        int ncap = lv->cap ? lv->cap * 2 : 64;
        lv->text = realloc(lv->text, (size_t)ncap * sizeof(char *));
        lv->flags = realloc(lv->flags, (size_t)ncap);
        lv->aux = realloc(lv->aux, (size_t)ncap * sizeof(int));
        lv->cap = ncap;
    }
    lv->text[lv->n] = _strdup(buf);
    lv->flags[lv->n] = (unsigned char)flags;
    lv->aux[lv->n] = aux;
    lv->n++;
}

/* reveal: a line to bring into view (centred if it is not), or -1 */
static void lv_end(HWND h, int reveal)
{
    lineview *lv = lv_of(h);
    if (!lv)
        return;
    if (reveal >= 0) {
        const int rows = lv_rows(h, lv);
        if (reveal < lv->first || reveal >= lv->first + rows - 1)
            lv->first = reveal - rows / 2;
    }
    lv_sync(h, lv);
}

static void lv_top(HWND h)
{
    lineview *lv = lv_of(h);
    if (lv)
        lv->first = 0;
}

static LRESULT CALLBACK lines_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    lineview *lv = lv_of(hwnd);
    int i;

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)calloc(1, sizeof(lineview)));
        break;
    case WM_NCDESTROY:
        if (lv) {
            for (i = 0; i < lv->n; i++)
                free(lv->text[i]);
            free(lv->text);
            free(lv->flags);
            free(lv->aux);
            free(lv);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        break;
    case WM_SETFONT:
        if (lv) {
            lv->font = (HFONT)wp;
            lv_sync(hwnd, lv);
        }
        return 0;
    case WM_GETFONT:
        return lv ? (LRESULT)lv->font : 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (lv)
            lv_sync(hwnd, lv);
        return 0;
    case WM_VSCROLL:
        if (lv) {
            const int rows = lv_rows(hwnd, lv);
            int pos = lv->first;
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos--; break;
            case SB_LINEDOWN: pos++; break;
            case SB_PAGEUP: pos -= rows - 1; break;
            case SB_PAGEDOWN: pos += rows - 1; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = lv->n; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = (int)HIWORD(wp); break;
            }
            lv->first = pos;
            lv_sync(hwnd, lv);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (lv) {
            lv->first -= (short)HIWORD(wp) / WHEEL_DELTA * 3;
            lv_sync(hwnd, lv);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (lv) {
            const int line = lv->first + (short)HIWORD(lp) / lv_row_h(hwnd, lv);
            if (line >= 0 && line < lv->n)
                PostMessageA(GetParent(hwnd), WM_DBG_LINE,
                             (WPARAM)GetDlgCtrlID(hwnd), (LPARAM)lv->aux[line]);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c;
        static HBRUSH bar, dot;
        static HPEN dotpen;
        HGDIOBJ oldfont;
        TEXTMETRICA tm;
        int rh = 15, y = 0;

        if (!bar) {
            bar = CreateSolidBrush(INTV_ACCENT_COLORREF);
            dot = CreateSolidBrush(RGB(0xE0, 0x1B, 0x24));
            dotpen = CreatePen(PS_SOLID, 1, RGB(0xE0, 0x1B, 0x24));
        }
        GetClientRect(hwnd, &c);
        FillRect(hdc, &c, GetSysColorBrush(COLOR_WINDOW));
        oldfont = SelectObject(hdc, lv && lv->font ? lv->font : GetStockObject(DEFAULT_GUI_FONT));
        if (GetTextMetricsA(hdc, &tm))
            rh = tm.tmHeight + 2;
        SetBkMode(hdc, TRANSPARENT);
        for (i = lv ? lv->first : 0; lv && i < lv->n && y < c.bottom; i++, y += rh) {
            RECT r;
            r.left = 0; r.top = y; r.right = c.right; r.bottom = y + rh;
            if (lv->flags[i] & LV_PC) {
                FillRect(hdc, &r, bar);
                SetTextColor(hdc, RGB(255, 255, 255));
            } else {
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            }
            TextOutA(hdc, LV_GUTTER, y + 1, lv->text[i], (int)strlen(lv->text[i]));
            if (lv->flags[i] & (LV_BP | LV_BP_OFF)) {
                const int off = (lv->flags[i] & LV_BP_OFF) != 0;
                static HPEN greypen;
                HGDIOBJ ob, op;
                const int top = y + (rh - 10) / 2;
                if (!greypen)
                    greypen = CreatePen(PS_SOLID, 1, RGB(0x8A, 0x8A, 0x8A));
                ob = SelectObject(hdc, off ? GetStockObject(NULL_BRUSH) : dot);
                op = SelectObject(hdc, off ? greypen : dotpen);
                Ellipse(hdc, 6, top, 16, top + 10);
                SelectObject(hdc, ob);
                SelectObject(hdc, op);
            }
        }
        SelectObject(hdc, oldfont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Forward declarations: show_page/refresh_mem are defined later (layout and
 * refreshers, respectively), but sym_list_subclass -- grouped here with the
 * other subclass procs, same as disasm_subclass above -- needs to call
 * both when a symbol row is clicked. */
static void show_page(debugger *d, int page);
static void goto_page(debugger *d, int page);
static void layout(debugger *d);
static void refresh_mem(debugger *d);
static void refresh_fujinet(debugger *d, int force);
static void refresh_all(debugger *d);

/* Symbols page list: a click jumps the memory view to the clicked line's
 * address, the first 4 hex characters of each line (refresh_symbols'
 * "%04X  NAME  ; note" format) -- same click-parses-the-line-prefix idiom
 * as disasm_subclass above, just column 0 instead of 2, and it also
 * switches the visible page to CPU so the jump is actually visible. */
static LRESULT CALLBACK sym_list_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        char buf[256];
        unsigned addr;
        int n;

        *(WORD *)buf = (WORD)(sizeof(buf) - 1);
        n = (int)SendMessageA(hwnd, EM_GETLINE, (WPARAM)line, (LPARAM)buf);
        if (n >= 4) {
            buf[n] = '\0';
            if (sscanf(buf, "%4X", &addr) == 1 && addr <= 0xFFFF) {
                g_dbg->mem_base = (uint16_t)addr;
                g_dbg->seen_serial = 0;
                goto_page(g_dbg, PAGE_CPU);
                if (intvdebug_is_paused(g_dbg->dbg))
                    refresh_mem(g_dbg);
            }
        }
    }
    return CallWindowProcA(g_sym_list_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------- */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style,
                  int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->new_parent ? d->new_parent : d->hwnd,
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

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS);
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        item.pszText = (char *)k_tab_names[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, i, (LPARAM)&item);
        d->tab_page[i] = i;
    }
    d->ntabs = TAB_COUNT;
    for (i = 0; i < PAGE_COUNT; i++) {   /* a container per page: its controls are its children */
        d->pg[i] = CreateWindowExA(0, "IntvDbgPage", "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10,
                                   d->hwnd, NULL, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
        SetWindowLongPtrA(d->pg[i], GWLP_USERDATA, (LONG_PTR)d->hwnd);
    }

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
    d->help_btn = child(d, "BUTTON", "Help", WS_VISIBLE | BS_PUSHBUTTON, IDC_HELP_BTN);
    d->popout_btn = child(d, "BUTTON", "Pop Out", WS_VISIBLE | BS_PUSHBUTTON, IDC_POPOUT);
    d->status = child(d, "STATIC", "Running", WS_VISIBLE | SS_RIGHT,
                     IDC_STATUS);

    /* CPU page */
    d->new_parent = d->pg[PAGE_CPU];
    d->disasm = child(d, "IntvDbgLines", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL,
                      IDC_DISASM);
    d->new_parent = NULL;   /* the registers sit under every page: the main window's */
    for (i = 0; i < 8; i++) {
        d->reg_label[i] = label(d, kRegNames[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i);
        EnableWindow(d->reg_edit[i], FALSE); /* read-only: see file header */
    }
    d->flags = label(d, "");
    d->new_parent = d->pg[PAGE_MEMORY];
    d->mem_label = label(d, "Address:");
    d->mem_addr = field(d, IDC_MEM_ADDR);
    SetWindowTextA(d->mem_addr, "0000");
    d->mem_view = child(d, "EDIT", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                          ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                        IDC_MEM_VIEW);
    d->new_parent = d->pg[PAGE_CPU];
    d->break_label = label(d, "Break at:");
    d->break_addr = field(d, IDC_BREAK_ADDR);

    /* BASIC page */
    d->new_parent = d->pg[PAGE_BASIC];
    d->bas_file_btn = child(d, "BUTTON", "Import File...",
                            WS_VISIBLE | BS_PUSHBUTTON, IDC_BAS_FILE);
    d->bas_folder_btn = child(d, "BUTTON", "Import Folder...",
                              WS_VISIBLE | BS_PUSHBUTTON, IDC_BAS_FOLDER);
    d->bas_map_btn = child(d, "BUTTON", "Load Map...",
                           WS_VISIBLE | BS_PUSHBUTTON, IDC_BAS_MAP);
    d->bas_files = child(d, "COMBOBOX", "",
                         WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST |
                             CBS_HASSTRINGS,
                         IDC_BAS_FILES);

    d->bas_note = label(d, "");
    d->bas_view = child(d, "IntvDbgLines", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL,
                        IDC_BAS_VIEW);
    d->bas_file = -1;

    /* Memory page: search */
    d->new_parent = d->pg[PAGE_MEMORY];
    d->srch_label = label(d, "Search:");
    d->srch_kind = child(d, "COMBOBOX", "", WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                         IDC_SRCH_KIND);
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex words");
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(d->srch_kind, CB_SETCURSEL, 0, 0);
    d->srch_entry = field(d, IDC_SRCH_ENTRY);
    d->srch_next = child(d, "BUTTON", "Find Next", WS_VISIBLE | BS_PUSHBUTTON, IDC_SRCH_NEXT);
    d->srch_prev = child(d, "BUTTON", "Find Previous", WS_VISIBLE | BS_PUSHBUTTON, IDC_SRCH_PREV);
    d->srch_note = label(d, "");

    /* Breakpoints page */
    d->new_parent = d->pg[PAGE_BREAKS];
    d->bp_type = child(d, "COMBOBOX", "", WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                       IDC_BP_TYPE);
    SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)"Execute");
    SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)"Read");
    SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)"Write");
    SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)"Access");
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_addr = field(d, IDC_BP_ADDR);
    d->bp_cond = child(d, "COMBOBOX", "", WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                       IDC_BP_COND);
    SendMessageA(d->bp_cond, CB_ADDSTRING, 0, (LPARAM)"any value");
    SendMessageA(d->bp_cond, CB_ADDSTRING, 0, (LPARAM)"changed");
    SendMessageA(d->bp_cond, CB_ADDSTRING, 0, (LPARAM)"equals");
    SendMessageA(d->bp_cond, CB_ADDSTRING, 0, (LPARAM)"not equal");
    SendMessageA(d->bp_cond, CB_SETCURSEL, 0, 0);
    d->bp_value = field(d, IDC_BP_VALUE);
    d->bp_add = child(d, "BUTTON", "Add", WS_VISIBLE | BS_PUSHBUTTON, IDC_BP_ADD);
    d->bp_list = child(d, "LISTBOX", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       IDC_BP_LIST);
    d->bp_toggle = child(d, "BUTTON", "Enable / Disable", WS_VISIBLE | BS_PUSHBUTTON, IDC_BP_TOGGLE);
    d->bp_remove = child(d, "BUTTON", "Remove", WS_VISIBLE | BS_PUSHBUTTON, IDC_BP_REMOVE);
    d->bp_hits = child(d, "BUTTON", "Clear Hits", WS_VISIBLE | BS_PUSHBUTTON, IDC_BP_HITS);
    d->bp_clear = child(d, "BUTTON", "Remove All", WS_VISIBLE | BS_PUSHBUTTON, IDC_BP_CLEAR);
    d->bp_stop = child(d, "STATIC", "", WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, IDC_BP_STOP);

    /* Watches page */
    d->new_parent = d->pg[PAGE_WATCHES];
    d->wt_label = label(d, "Watch:");
    d->wt_entry = field(d, IDC_WT_ENTRY);
    d->wt_add = child(d, "BUTTON", "Add", WS_VISIBLE | BS_PUSHBUTTON, IDC_WT_ADD);
    d->wt_list = child(d, "LISTBOX", "", WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       IDC_WT_LIST);
    d->wt_mem = child(d, "BUTTON", "Show in Memory", WS_VISIBLE | BS_PUSHBUTTON, IDC_WT_MEM);
    d->wt_brk = child(d, "BUTTON", "Break on Change", WS_VISIBLE | BS_PUSHBUTTON, IDC_WT_BRK);
    d->wt_remove = child(d, "BUTTON", "Remove", WS_VISIBLE | BS_PUSHBUTTON, IDC_WT_REMOVE);

    /* Cart page */
    d->new_parent = d->pg[PAGE_CART];
    d->cart_text = mono_view(d, IDC_CART_TEXT);

    /* Trace (live) and History (when stopped) pages */
    for (i = 0; i < 2; i++) {
        d->new_parent = d->pg[PAGE_TRACE + i];
        d->h_rec[i] = child(d, "BUTTON", "Record", WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE, IDC_H_REC0 + i);
        d->h_filter[i] = child(d, "COMBOBOX", "", WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST |
                                                      CBS_HASSTRINGS, IDC_H_FILTER0 + i);
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"All instructions");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"Branches only");
        SendMessageA(d->h_filter[i], CB_ADDSTRING, 0, (LPARAM)"In range");
        SendMessageA(d->h_filter[i], CB_SETCURSEL, 0, 0);
        d->h_range[i] = field(d, IDC_H_RANGE0 + i);
        d->h_clear[i] = child(d, "BUTTON", "Clear", WS_VISIBLE | BS_PUSHBUTTON, IDC_H_CLEAR0 + i);
        d->h_note[i] = child(d, "STATIC", "", WS_VISIBLE | SS_RIGHT | SS_ENDELLIPSIS, IDC_H_NOTE0 + i);
        d->h_text[i] = mono_view(d, IDC_H_TEXT0 + i);
        SendMessageA(d->h_text[i], EM_SETLIMITTEXT, 0, 0); /* past 32K */
        d->h_seen[i] = (uint64_t)-1;
    }

    /* STIC page */
    d->new_parent = d->pg[PAGE_STIC];
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

    /* Symbols page */
    d->new_parent = d->pg[PAGE_SYMBOLS];
    d->sym_filter_label = label(d, "Filter:");
    d->sym_filter = field(d, IDC_SYM_FILTER);
    d->sym_count = label(d, "");
    d->sym_list = mono_view(d, IDC_SYM_LIST);
    g_sym_list_proc = (WNDPROC)SetWindowLongPtrA(d->sym_list, GWLP_WNDPROC,
                                                 (LONG_PTR)sym_list_subclass);

    /* FujiNet page */
    d->new_parent = d->pg[PAGE_FUJINET];
    d->fuji_title = label(d, "FujiBus packets (SLIP-decoded)");
    d->fuji_note = label(d, "");
    d->fuji_clear = child(d, "BUTTON", "Clear", WS_VISIBLE | BS_PUSHBUTTON,
                          IDC_FUJI_CLEAR);
    d->fuji_text = mono_view(d, IDC_FUJI_TEXT);
    SendMessageA(d->fuji_text, EM_SETLIMITTEXT, 0, 0); /* past 32K */
    d->fuji_state_title = label(d, "FujiNet's mailbox");
    d->fuji_state = mono_view(d, IDC_FUJI_STATE);
    d->fuji_count = -1;

    /* The tab was created first, so it is below the page controls; a repaint
     * of it would still paint its white body over any that had not repainted
     * (the BASIC page's file list vanished). Bottom of the z-order, and with
     * WS_CLIPSIBLINGS it never paints over a sibling. */
    d->new_parent = NULL;
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* ---- layout ------------------------------------------------------------------
 * A fixed, manually-computed grid rather than a resizable one -- matches
 * the scope of the other three frontends' debugger windows (none offer
 * fluid resizing of their internal panes either). Recomputed on WM_SIZE. */

/* Help's contexts (intvhelp.h), one per tab */
static const char *const k_tab_contexts[] = {
    "debugger/cpu", "debugger/basic", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/cart", "debugger/trace", "debugger/history", "debugger/stic",
    "debugger/symbols", "debugger/fujinet",
};

/* On screen: the page the tab control shows, or one living in a window of its own. */
static int page_visible(debugger *d, int page)
{
    return d->popped[page] != NULL || d->page == page;
}

static void show_page(debugger *d, int page)
{
    int p;
    d->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    if (page >= 0 && page < (int)(sizeof k_tab_contexts / sizeof k_tab_contexts[0]))
        intv_help_set_context(d->hwnd, k_tab_contexts[page], 1);
    for (p = 0; p < PAGE_COUNT; p++)   /* a page's controls follow its container */
        ShowWindow(d->pg[p], d->popped[p] || p == page ? SW_SHOW : SW_HIDE);
    if (page == PAGE_FUJINET)
        refresh_fujinet(d, 0);
}

/* The tab control's index for a page, or -1 when it is in a window of its own. */
static int tab_index(debugger *d, int page)
{
    int i;
    for (i = 0; i < d->ntabs; i++)
        if (d->tab_page[i] == page)
            return i;
    return -1;
}

static void goto_page(debugger *d, int page)
{
    const int i = tab_index(d, page);
    if (d->popped[page]) {
        SetForegroundWindow(d->popped[page]);
        return;
    }
    if (i >= 0) {
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)i, 0);
        show_page(d, page);
    }
}

/* ---- peeling a page off into a window of its own, and putting it back ----------- */

static void dock_page(debugger *d, int page)
{
    TCITEMA item;
    HWND pop = d->popped[page];
    int pos = 0, p, i;

    if (!pop)
        return;
    d->popped[page] = NULL;
    SetParent(d->pg[page], d->hwnd);
    for (p = 0; p < page; p++)   /* back where it was in the order */
        if (!d->popped[p])
            pos++;
    for (i = d->ntabs; i > pos; i--)
        d->tab_page[i] = d->tab_page[i - 1];
    d->tab_page[pos] = page;
    d->ntabs++;
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (char *)k_tab_names[page];
    SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(d, page);
    layout(d);
    d->seen_serial = 0;
}

static void pop_out(debugger *d, int page)
{
    HWND pop;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    char title[96];
    int idx, i, next;

    if (page < 0 || page >= PAGE_COUNT || d->popped[page] || d->ntabs <= 1)
        return;   /* one tab always stays */
    idx = tab_index(d, page);
    if (idx < 0)
        return;
    SendMessageA(d->tabs, TCM_DELETEITEM, (WPARAM)idx, 0);
    for (i = idx; i < d->ntabs - 1; i++)
        d->tab_page[i] = d->tab_page[i + 1];
    d->ntabs--;

    snprintf(title, sizeof(title), "Intellivision Debugger - %s", k_tab_names[page]);
    pop = CreateWindowExA(0, "IntvDbgPop", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 580,
                          d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    {
        HWND dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 4, 4, 70, 26,
                                    pop, (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
        SendMessageA(dock, WM_SETFONT, (WPARAM)d->mono, TRUE);
    }
    SetParent(d->pg[page], pop);
    d->popped[page] = pop;
    next = idx < d->ntabs ? idx : d->ntabs - 1;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
    show_page(d, d->tab_page[next]);
    layout(d);
    ShowWindow(pop, SW_SHOW);
    d->seen_serial = 0;
}

/* A page container: its controls' notifications go to the debugger window. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND main = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_DBG_LINE:
    case WM_DBG_ACCEPT:
        return main ? SendMessageA(main, msg, wp, lp) : 0;
    case WM_ERASEBKGND: {
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect((HDC)wp, &c, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window a peeled-off page lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SIZE:
        if (g_dbg)
            layout(g_dbg);
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wp) == IDC_DOCK)
            dock_page(g_dbg, page);
        return 0;
    case WM_CLOSE:
        if (g_dbg)
            dock_page(g_dbg, page);
        else
            DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void layout(debugger *d)
{
    RECT client, page, pr[PAGE_COUNT];
    int y, i;

    GetClientRect(d->hwnd, &client);

    y = 4;
    MoveWindow(d->pause_btn, 4, y, 110, 26, TRUE);
    MoveWindow(d->step_btn, 118, y, 90, 26, TRUE);
    MoveWindow(d->step_over_btn, 212, y, 110, 26, TRUE);
    MoveWindow(d->step_out_btn, 326, y, 140, 26, TRUE);
    MoveWindow(d->clear_bp_btn, 470, y, 130, 26, TRUE);
    MoveWindow(d->load_syms_btn, 604, y, 120, 26, TRUE);
    MoveWindow(d->clear_syms_btn, 728, y, 110, 26, TRUE);
    MoveWindow(d->help_btn, 842, y, 70, 26, TRUE);
    MoveWindow(d->popout_btn, 916, y, 80, 26, TRUE);
    {
        const int sx = client.right - 210 > 1004 ? client.right - 210 : 1004;
        MoveWindow(d->status, sx, y + 4, client.right - sx - 10 > 40 ? client.right - sx - 10 : 40, 20, TRUE);
    }
    y += 32;

    MoveWindow(d->tabs, 4, y, client.right - 8, client.bottom - y - 4 - 60, TRUE);
    page = client;
    page.top = y;
    page.bottom = client.bottom - 60;   /* the registers strip is below */
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    {   /* each page's own rectangle, relative to its container */
        int p;
        for (p = 0; p < PAGE_COUNT; p++) {
            if (d->popped[p]) {
                RECT pc;
                GetClientRect(d->popped[p], &pc);
                MoveWindow(d->pg[p], 0, 34, pc.right, pc.bottom > 34 ? pc.bottom - 34 : 1, TRUE);
                pr[p].left = pr[p].top = 0;
                pr[p].right = pc.right;
                pr[p].bottom = pc.bottom > 34 ? pc.bottom - 34 : 1;
            } else {
                MoveWindow(d->pg[p], page.left, page.top, page.right - page.left, page.bottom - page.top, TRUE);
                pr[p].left = pr[p].top = 0;
                pr[p].right = page.right - page.left;
                pr[p].bottom = page.bottom - page.top;
            }
        }
    }

    /* CPU page: the disassembly, and a "Break at" row under it */
    page = pr[PAGE_CPU];
    MoveWindow(d->disasm, page.left, page.top, page.right - page.left - 8,
              page.bottom - page.top - 30, TRUE);
    MoveWindow(d->break_label, page.left, page.bottom - 24, 70, 18, TRUE);
    MoveWindow(d->break_addr, page.left + 74, page.bottom - 26, 220, 22, TRUE);

    /* Memory page */
    page = pr[PAGE_MEMORY];
    MoveWindow(d->mem_label, page.left, page.top + 2, 70, 18, TRUE);
    MoveWindow(d->mem_addr, page.left + 74, page.top, 260, 22, TRUE);
    MoveWindow(d->srch_label, page.left, page.top + 34, 70, 18, TRUE);
    MoveWindow(d->srch_kind, page.left + 74, page.top + 30, 110, 200, TRUE);
    MoveWindow(d->srch_entry, page.left + 190, page.top + 30, 260, 22, TRUE);
    MoveWindow(d->srch_next, page.left + 456, page.top + 28, 90, 26, TRUE);
    MoveWindow(d->srch_prev, page.left + 550, page.top + 28, 110, 26, TRUE);
    MoveWindow(d->srch_note, page.left + 668, page.top + 34, 260, 18, TRUE);
    MoveWindow(d->mem_view, page.left, page.top + 60, page.right - page.left - 8,
              page.bottom - page.top - 64, TRUE);

    /* Breakpoints page */
    {
        const int R = pr[PAGE_BREAKS].right - 8, top = 0;
        page = pr[PAGE_BREAKS];
        MoveWindow(d->bp_type, page.left, top, 90, 200, TRUE);
        MoveWindow(d->bp_add, R - 60, top - 2, 60, 26, TRUE);
        MoveWindow(d->bp_value, R - 60 - 4 - 90, top, 90, 22, TRUE);
        MoveWindow(d->bp_cond, R - 60 - 4 - 90 - 4 - 110, top, 110, 200, TRUE);
        MoveWindow(d->bp_addr, page.left + 94, top, R - 60 - 4 - 90 - 4 - 110 - 4 - (page.left + 94), 22, TRUE);
        MoveWindow(d->bp_list, page.left, top + 30, R - page.left, page.bottom - top - 30 - 64, TRUE);
        MoveWindow(d->bp_toggle, page.left, page.bottom - 58, 130, 26, TRUE);
        MoveWindow(d->bp_remove, page.left + 134, page.bottom - 58, 80, 26, TRUE);
        MoveWindow(d->bp_hits, page.left + 218, page.bottom - 58, 90, 26, TRUE);
        MoveWindow(d->bp_clear, page.left + 312, page.bottom - 58, 100, 26, TRUE);
        MoveWindow(d->bp_stop, page.left, page.bottom - 26, R - page.left, 20, TRUE);
    }

    /* Watches page */
    {
        const int R = pr[PAGE_WATCHES].right - 8, top = 0;
        page = pr[PAGE_WATCHES];
        MoveWindow(d->wt_label, page.left, top + 4, 60, 18, TRUE);
        MoveWindow(d->wt_entry, page.left + 64, top, R - 60 - 4 - (page.left + 64), 22, TRUE);
        MoveWindow(d->wt_add, R - 60, top - 2, 60, 26, TRUE);
        MoveWindow(d->wt_list, page.left, top + 30, R - page.left, page.bottom - top - 30 - 36, TRUE);
        MoveWindow(d->wt_mem, page.left, page.bottom - 30, 130, 26, TRUE);
        MoveWindow(d->wt_brk, page.left + 134, page.bottom - 30, 140, 26, TRUE);
        MoveWindow(d->wt_remove, page.left + 278, page.bottom - 30, 80, 26, TRUE);
    }

    /* Registers: a strip under every page */
    {
        const int ry = client.bottom - 56;
        for (i = 0; i < 8; i++) {
            const int rx = 8 + (i % 4) * 128, rr = ry + (i / 4) * 26;
            MoveWindow(d->reg_label[i], rx, rr + 2, 58, 18, TRUE);
            MoveWindow(d->reg_edit[i], rx + 58, rr, 62, 22, TRUE);
        }
        MoveWindow(d->flags, 8 + 4 * 128 + 12, ry + 2, 300, 40, TRUE);
    }

    /* BASIC page */
    {
        int by = pr[PAGE_BASIC].top;
        page = pr[PAGE_BASIC];
        MoveWindow(d->bas_file_btn, page.left, by, 110, 26, TRUE);
        MoveWindow(d->bas_folder_btn, page.left + 114, by, 120, 26, TRUE);
        MoveWindow(d->bas_map_btn, page.left + 238, by, 100, 26, TRUE);
        MoveWindow(d->bas_files, page.left + 346, by, 260, 220, TRUE);
        by += 32;
        MoveWindow(d->bas_note, page.left, by, page.right - page.left - 8, 34, TRUE);
        by += 38;
        MoveWindow(d->bas_view, page.left, by, page.right - page.left - 8,
                  page.bottom - by - 4, TRUE);
    }

    /* STIC page */
    {
        int sx = 0, sy = 0;
        page = pr[PAGE_STIC];
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

    /* Symbols page */
    {
        int sy = 0;
        page = pr[PAGE_SYMBOLS];
        MoveWindow(d->sym_filter_label, page.left, sy, 50, 22, TRUE);
        MoveWindow(d->sym_filter, page.left + 54, sy, 220, 22, TRUE);
        MoveWindow(d->sym_count, page.left + 282, sy, 200, 22, TRUE);
        sy += 28;
        MoveWindow(d->sym_list, page.left, sy, page.right - page.left - 8,
                  page.bottom - sy - 4, TRUE);
    }

    /* FujiNet page: the packets left (a note and Clear over them), the
     * mailbox right */
    {
        int fy = 0;
        page = pr[PAGE_FUJINET];
        int rw = (page.right - page.left) / 3 > 320
                     ? 320 : (page.right - page.left) / 3;
        int lw = page.right - page.left - rw - 12;
        int rx = page.left + lw + 12;
        MoveWindow(d->fuji_title, page.left, fy, lw, 18, TRUE);
        MoveWindow(d->fuji_state_title, rx, fy, rw - 8, 18, TRUE);
        fy += 22;
        MoveWindow(d->fuji_note, page.left, fy + 4, lw - 90, 18, TRUE);
        MoveWindow(d->fuji_clear, page.left + lw - 80, fy, 80, 26, TRUE);
        MoveWindow(d->fuji_state, rx, fy, rw - 8, page.bottom - fy - 4, TRUE);
        fy += 30;
        MoveWindow(d->fuji_text, page.left, fy, lw, page.bottom - fy - 4,
                  TRUE);
    }

    /* Cart page */
    MoveWindow(d->cart_text, 0, 0, pr[PAGE_CART].right - 8, pr[PAGE_CART].bottom - 4, TRUE);

    /* Trace and History pages */
    for (i = 0; i < 2; i++) {
        const RECT r = pr[PAGE_TRACE + i];
        MoveWindow(d->h_rec[i], 0, 0, 90, 26, TRUE);
        MoveWindow(d->h_filter[i], 96, 2, 140, 200, TRUE);
        MoveWindow(d->h_range[i], 242, 2, 200, 22, TRUE);
        MoveWindow(d->h_clear[i], 448, 0, 70, 26, TRUE);
        MoveWindow(d->h_note[i], 526, 6, r.right - 526 - 8 > 40 ? r.right - 526 - 8 : 40, 18, TRUE);
        MoveWindow(d->h_text[i], 0, 32, r.right - 8, r.bottom - 36, TRUE);
    }
}

/* ---- refreshers --------------------------------------------------------------- */

static void refresh_disasm(debugger *d)
{
    intvdebug_dasm_line lines[DISASM_LINES];
    intvdebug_regs r;
    int n, i;

    lv_begin(d->disasm);
    if (!intvdebug_regs_get(d->dbg, &r)) {
        lv_end(d->disasm, -1);
        return;
    }
    n = intvdebug_disassemble(d->dbg, r.pc, r.D, lines, DISASM_LINES);
    for (i = 0; i < n; i++) {
        char symsuffix[96], text[256];
        intvsym_annotate_line(d->symtab, &lines[i], symsuffix,
                              sizeof(symsuffix));
        snprintf(text, sizeof(text), "%04X  %-24s%s", lines[i].addr,
                 lines[i].text, symsuffix);
        lv_add(d->disasm, text,
               (lines[i].addr == r.pc ? LV_PC : 0) |
                   (intvdebug_breakpoint_state(d->dbg, lines[i].addr) == 1 ? LV_BP :
                    intvdebug_breakpoint_state(d->dbg, lines[i].addr) == 2 ? LV_BP_OFF : 0),
               lines[i].addr);
    }
    lv_top(d->disasm);
    lv_end(d->disasm, -1);
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
    uint16_t words[MEM_WORDS_PER_ROW * MEM_ROWS];
    char text[8192];
    int n, row, col, len = 0, sel_start = -1, sel_end = -1;

    n = intvdebug_read(d->dbg, d->mem_base, words, MEM_WORDS_PER_ROW * MEM_ROWS);
    {
        const char *last_region = NULL;
        for (row = 0; row * MEM_WORDS_PER_ROW < n; row++) {
            uint16_t row_addr = (uint16_t)(d->mem_base + row * MEM_WORDS_PER_ROW);
            const char *region = intvsym_region(row_addr);

            len += snprintf(text + len, sizeof(text) - (size_t)len, "%04X ",
                           row_addr);
            for (col = 0; col < MEM_WORDS_PER_ROW; col++) {
                int idx = row * MEM_WORDS_PER_ROW + col;
                const uint16_t a = (uint16_t)(row_addr + col);
                if (idx >= n)
                    continue;
                if (d->mem_hl && a >= d->mem_hl_lo && a <= d->mem_hl_hi) {
                    if (sel_start < 0)
                        sel_start = len + 1;   /* the word, after its space */
                    sel_end = len + 5;
                }
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
    if (sel_start >= 0)   /* the words of a variable, symbol or hit: selected */
        SendMessageA(d->mem_view, EM_SETSEL, (WPARAM)sel_start, (LPARAM)sel_end);
}

/* Shows a range in the Memory tab, highlighted, on a row boundary. */
static void show_in_memory(debugger *d, uint16_t lo, uint16_t hi)
{
    d->mem_base = (uint16_t)(lo & 0xFFF8);
    d->mem_hl = 1;
    d->mem_hl_lo = lo;
    d->mem_hl_hi = hi;
    d->seen_serial = 0;
    if (intvdebug_is_paused(d->dbg))
        refresh_mem(d);
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

/* Case-insensitive substring test -- MinGW does not reliably provide
 * strcasestr (it's a glibc/BSD extension, not standard C), and matching
 * the GNOME/KDE/macOS frontends' filter behaviour (all case-insensitive) is
 * worth a small local helper rather than special-casing this one port. */
static int contains_ci(const char *hay, const char *needle)
{
    size_t hlen = strlen(hay), nlen = strlen(needle), i;
    if (nlen == 0)
        return 1;
    if (nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++)
        if (_strnicmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* Only rebuilds the list when the table's generation or the filter text has
 * changed since the last call -- a real game .sym can be a few hundred
 * entries, no reason to re-render it on every 100ms timer tick. */
static void refresh_symbols(debugger *d)
{
    char filter[64];
    char text[16384];
    int n, i, len = 0, shown = 0;
    unsigned gen = intvsymtab_generation(d->symtab);
    char countbuf[64];

    GetWindowTextA(d->sym_filter, filter, sizeof(filter));
    if (gen == d->sym_seen_gen && strcmp(filter, d->sym_filter_text) == 0)
        return;
    d->sym_seen_gen = gen;
    snprintf(d->sym_filter_text, sizeof(d->sym_filter_text), "%s", filter);

    n = intvsymtab_count(d->symtab);
    for (i = 0; i < n && len < (int)sizeof(text) - 128; i++) {
        uint16_t addr;
        char name[64], note[64];
        if (!intvsymtab_enum(d->symtab, i, &addr, name, sizeof(name), note,
                             sizeof(note)))
            continue;
        if (filter[0] && !contains_ci(name, filter))
            continue;
        len += snprintf(text + len, sizeof(text) - (size_t)len,
                       "%04X  %-24s%s%s\r\n", addr, name,
                       note[0] ? "  ; " : "", note);
        shown++;
    }
    SetWindowTextA(d->sym_list, text);
    snprintf(countbuf, sizeof(countbuf), "%d symbol%s%s", shown,
            shown == 1 ? "" : "s", filter[0] ? " (filtered)" : "");
    SetWindowTextA(d->sym_count, countbuf);
}

/* ---- the FujiNet page ------------------------------------------------------- */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* A log follows its end while its reader is there. */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageA(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageA(h, EM_LINESCROLL, 0, SendMessageA(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageA(h, EM_LINESCROLL, 0,
                 first - (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageA(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextA(h, text);
    if (follow)
        edit_to_end(h);
    else
        edit_scroll_to(h, first);
    SendMessageA(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
}

/* The core's UTF-8 as this window's ASCII ("->", "x", "..."), with the CR LF
 * an EDIT wants. */
static void ascii_view_text(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    while (*in && o + 4 < cap) {
        const unsigned char c = (unsigned char)*in;
        if (strncmp(in, "\xe2\x86\x92", 3) == 0) {
            memcpy(out + o, "->", 2);
            o += 2;
            in += 3;
        } else if (strncmp(in, "\xe2\x80\xa6", 3) == 0) {
            memcpy(out + o, "...", 3);
            o += 3;
            in += 3;
        } else if (strncmp(in, "\xc3\x97", 2) == 0) {
            out[o++] = 'x';
            in += 2;
        } else if (c == '\n') {
            out[o++] = '\r';
            out[o++] = '\n';
            in++;
        } else {
            out[o++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
            in++;
        }
    }
    out[o] = '\0';
}

static void refresh_fujinet(debugger *d, int force)
{
    static intvdebug_fuji_packet p[INTVDEBUG_FUJI_TRACE_MAX];
    static char t[INTVDEBUG_FUJI_TRACE_MAX * 320 + 128];
    static char a[INTVDEBUG_FUJI_TRACE_MAX * 330 + 128];
    char state[2048], state_a[2600], cur[2600];
    size_t len = 0;
    int n = intvdebug_fujinet_trace(d->dbg, p, INTVDEBUG_FUJI_TRACE_MAX), i;
    int follow;

    d->fuji_ticks = 0;
    if (!d->session || (!intvsession_fujinet_running(d->session) &&
                        !intvsession_fujinet_external(d->session))) {
        SetWindowTextA(d->fuji_note, "FujiNet is not running.");
    } else {
        char note[160];
        snprintf(note, sizeof(note), "%s FujiNet at %s, cartridge %s.",
                 intvsession_fujinet_external(d->session) ? "External" : "Built-in",
                 intvsession_boip_endpoint(d->session),
                 intvsession_fujinet_link(d->session) ? "connected" : "not connected");
        SetWindowTextA(d->fuji_note, note);
    }
    intvdebug_fujinet_state(d->dbg, state, (int)sizeof(state));
    ascii_view_text(state, state_a, sizeof(state_a));
    GetWindowTextA(d->fuji_state, cur, sizeof(cur));
    if (strcmp(cur, state_a) != 0) /* unchanged keeps a selection */
        set_text_at(d->fuji_state, state_a, 0);

    if (!force && n == d->fuji_count &&
        (n == 0 || p[n - 1].serial == d->fuji_last))
        return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n && len < sizeof(t) - 320; i++) {
        int k;
        len += (size_t)snprintf(t + len, sizeof(t) - len, "%s%s\n     ",
                                i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            len += (size_t)snprintf(t + len, sizeof(t) - len, "%02X ",
                                    p[i].data[k]);
        if (p[i].nbytes > (int)sizeof(p[i].data))
            len += (size_t)snprintf(t + len, sizeof(t) - len, "...");
    }
    if (n == 0)
        snprintf(t, sizeof(t), "(no FujiBus packets since the trace was cleared)");
    ascii_view_text(t, a, sizeof(a));
    set_text_at(d->fuji_text, a, follow);
}

/* ---- the BASIC page ------------------------------------------------------- */

/* The map changed (an import, a Load Map): the file list. */
static void basic_reload(debugger *d)
{
    const int n = intvsrcmap_file_count(d->srcmap);
    int i;

    SendMessageA(d->bas_files, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++)
        SendMessageA(d->bas_files, CB_ADDSTRING, 0,
                     (LPARAM)intvsrcmap_file_name(d->srcmap, i));
    if (d->bas_file >= n)
        d->bas_file = -1;
    if (d->bas_file < 0 && n > 0) {
        int f, l, found = 0;
        d->bas_file = 0; /* the first file with any code on it */
        for (f = 0; f < n && !found; f++) {
            const int lines = intvsrcmap_line_count(d->srcmap, f);
            for (l = 1; l <= lines && !found; l++)
                if (intvsrcmap_first_addr(d->srcmap, f, l) >= 0) {
                    d->bas_file = f;
                    found = 1;
                }
        }
    }
    SendMessageA(d->bas_files, CB_SETCURSEL, (WPARAM)d->bas_file, 0);
    d->bas_pc_line = 0;
    lv_top(d->bas_view);
    if (n == 0)
        SetWindowTextA(d->bas_note,
            "Import a .bas file, or a folder of them. The assembler's map "
            "(as1600 -j game.smap) puts each line at its address.");
}

static void refresh_basic(debugger *d, int paused)
{
    const unsigned gen = intvsrcmap_generation(d->srcmap);
    intvdebug_regs r;
    int pf = -1, pl = 0;
    int n, i, line;

    if (gen != d->bas_gen) {
        d->bas_gen = gen;
        basic_reload(d);
    }
    if (paused && intvdebug_regs_get(d->dbg, &r)) {
        int f, l;
        if (intvsrcmap_lookup(d->srcmap, r.pc, &f, &l)) {
            pf = f;
            pl = l;
        }
    }
    if (pf >= 0 && pf != d->bas_file) { /* followed into another file */
        d->bas_file = pf;
        SendMessageA(d->bas_files, CB_SETCURSEL, (WPARAM)pf, 0);
    }

    n = d->bas_file >= 0 ? intvsrcmap_line_count(d->srcmap, d->bas_file) : 0;
    lv_begin(d->bas_view);
    for (i = 1; i <= n; i++) {
        const int addr = intvsrcmap_first_addr(d->srcmap, d->bas_file, i);
        const char *src = intvsrcmap_line_text(d->srcmap, d->bas_file, i);
        char text[300];
        snprintf(text, sizeof(text), "%5d  %.250s", i, src ? src : "");
        lv_add(d->bas_view, text,
               (pf == d->bas_file && i == pl ? LV_PC : 0) |
                   (addr < 0 ? 0 : intvdebug_breakpoint_state(d->dbg, (uint16_t)addr) == 1 ? LV_BP :
                    intvdebug_breakpoint_state(d->dbg, (uint16_t)addr) == 2 ? LV_BP_OFF : 0),
               i);
    }
    line = pf == d->bas_file ? pl : 0;
    {
        int reveal = -1;
        /* every new stop brings the current line into view, even the same line as last time */
        const uint64_t ser = intvdebug_stop_serial(d->dbg);
        const int new_stop = paused && ser != d->bas_serial;
        if (paused)
            d->bas_serial = ser;
        if (line != d->bas_pc_line || (new_stop && line > 0)) {
            d->bas_pc_line = line;
            if (line > 0)
                reveal = line - 1;
        }
        lv_end(d->bas_view, reveal);
    }
}

static void basic_note(debugger *d, int files, const intvsrcmap_import_result *r)
{
    char msg[600];
    const char *base = strrchr(r->map_path, '\\');
    if (!base) base = strrchr(r->map_path, '/');
    base = base ? base + 1 : r->map_path;
    if (r->mapped > 0)
        snprintf(msg, sizeof(msg), "%d file%s, %d address range%s from %s%s.",
                 files, files == 1 ? "" : "s", r->mapped, r->mapped == 1 ? "" : "s",
                 base, r->symbols ? ", with its symbols" : "");
    else
        snprintf(msg, sizeof(msg),
                 "%d file%s loaded, but no .smap or .lst was found beside them. "
                 "Build with as1600 -j game.smap, then Load Map.",
                 files, files == 1 ? "" : "s");
    SetWindowTextA(d->bas_note, msg);
}

/* Import File / Import Folder, and the INTV_BASIC_IMPORT hook. */
static void basic_import(debugger *d, const char *path, int folder)
{
    intvsrcmap_import_result r;
    const int n = intvsrcmap_import(d->srcmap, path, d->symtab, &r);

    if (n < 0) {
        SetWindowTextA(d->bas_note, folder ? "No .bas files in that folder."
                                           : "Could not read that file.");
        return;
    }
    basic_note(d, n, &r);
    d->bas_file = -1; /* basic_reload picks the first file with code */
    d->seen_serial = 0;
    if (intvdebug_is_paused(d->dbg))
        refresh_all(d);
}

static int pick_bas_folder(HWND owner, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";

    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = "Import a folder of IntyBASIC source";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListA(pidl, dir))
        dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

/* kind: IDC_BAS_FILE, IDC_BAS_FOLDER or IDC_BAS_MAP */
static void basic_pick(debugger *d, int kind)
{
    char path[MAX_PATH] = "", last_dir[1024] = "", dir[1024];

    if (d->session) {
        const char *s = intvsession_get_str(d->session, "bas_dir", "");
        snprintf(last_dir, sizeof(last_dir), "%s", s);
    }
    if (kind == IDC_BAS_FOLDER) {
        if (!pick_bas_folder(d->hwnd, path, sizeof(path)))
            return;
        snprintf(dir, sizeof(dir), "%s", path);
    } else {
        OPENFILENAMEA ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = d->hwnd;
        ofn.lpstrFilter = kind == IDC_BAS_MAP
            ? "as1600 source maps and listings (*.smap;*.lst)\0*.smap;*.lst\0All files (*.*)\0*.*\0"
            : "IntyBASIC source (*.bas)\0*.bas\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = sizeof(path);
        ofn.lpstrTitle = kind == IDC_BAS_MAP ? "Load Map" : "Import File";
        ofn.lpstrInitialDir = last_dir[0] ? last_dir : NULL;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&ofn))
            return;
        intvsym_dirname(path, dir, sizeof(dir));
    }
    if (d->session && dir[0]) { /* start the next picker where this one ended */
        intvsession_set_str(d->session, "bas_dir", dir);
        intvsession_settings_flush(d->session);
    }
    if (kind == IDC_BAS_MAP) {
        const int n = intvsrcmap_load_map(d->srcmap, path);
        if (n < 0) {
            SetWindowTextA(d->bas_note, "Could not read that map.");
        } else {
            intvsrcmap_import_result r;
            memset(&r, 0, sizeof(r));
            r.mapped = n;
            r.symbols = intvsrcmap_load_sibling_sym(path, d->symtab);
            snprintf(r.map_path, sizeof(r.map_path), "%s", path);
            basic_note(d, intvsrcmap_file_count(d->srcmap), &r);
            d->seen_serial = 0;
            if (intvdebug_is_paused(d->dbg))
                refresh_all(d);
        }
    } else {
        basic_import(d, path, kind == IDC_BAS_FOLDER);
    }
}

/* On the BASIC page the three steppers go by BASIC line. */
static void do_step(debugger *d, int how)
{
    const int by_line = d->page == PAGE_BASIC ||
                        (d->popped[PAGE_BASIC] && GetActiveWindow() == d->popped[PAGE_BASIC]);
    switch (how) {
    case IDC_STEP:
        if (by_line) intvdebug_step_line(d->dbg); else intvdebug_step(d->dbg);
        break;
    case IDC_STEP_OVER:
        if (by_line) intvdebug_step_line_over(d->dbg); else intvdebug_step_over(d->dbg);
        break;
    default:
        if (by_line) intvdebug_step_line_out(d->dbg); else intvdebug_step_out(d->dbg);
        break;
    }
}

/* ---- Breakpoints, Watches, memory search ------------------------------------ */

/* One line on why the machine stopped. */
static void stop_text(const intvdebug_stop_info *st, char *dst, size_t n)
{
    switch (st->reason) {
    case INTVDEBUG_STOP_WATCH_WRITE:
        snprintf(dst, n, "wrote $%04X = $%02X (was $%02X), now at $%04X", st->addr,
                 st->value & 0xFF, st->old_value & 0xFF, st->pc);
        break;
    case INTVDEBUG_STOP_WATCH_READ:
        snprintf(dst, n, "read $%04X, now at $%04X", st->addr, st->pc);
        break;
    case INTVDEBUG_STOP_BREAKPOINT:
        snprintf(dst, n, "breakpoint at $%04X", st->pc);
        break;
    default:
        dst[0] = '\0';
    }
}

static void refresh_breaks(debugger *d)
{
    const unsigned gen = intvdebug_bp_generation(d->dbg);
    intvdebug_stop_info st;
    char why[160], line[220];
    const int paused = intvdebug_is_paused(d->dbg);

    if (gen != d->bp_gen) {
        intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
        const int n = intvdebug_bp_list(d->dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
        const int keep = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
        int i;
        d->bp_gen = gen;
        SendMessageA(d->bp_list, WM_SETREDRAW, FALSE, 0);
        SendMessageA(d->bp_list, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < n; i++) {
            char desc[160];
            int at;
            intvbp_describe(&bps[i], d->symtab, desc, sizeof(desc));
            snprintf(line, sizeof(line), "[%c] %s    hits %u", bps[i].enabled ? 'x' : ' ', desc, bps[i].hits);
            at = (int)SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)line);
            SendMessageA(d->bp_list, LB_SETITEMDATA, (WPARAM)at, (LPARAM)bps[i].id);
        }
        if (keep >= 0 && keep < n)
            SendMessageA(d->bp_list, LB_SETCURSEL, (WPARAM)keep, 0);
        SendMessageA(d->bp_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(d->bp_list, NULL, TRUE);
    }
    why[0] = '\0';
    if (paused && intvdebug_last_stop(d->dbg, &st))
        stop_text(&st, why, sizeof(why));
    snprintf(line, sizeof(line), "%s", paused ? (why[0] ? "Stopped: " : "Paused.") : "Running.");
    if (paused && why[0])
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", why);
    SetWindowTextA(d->bp_stop, line);
}

static void refresh_watches(debugger *d)
{
    const int n = intvwatch_count();
    const int keep = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    int i;

    SendMessageA(d->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->wt_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        char line[200];
        intvwatch_format(d->dbg, d->symtab, i, line, sizeof(line));
        SendMessageA(d->wt_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (keep >= 0 && keep < n)
        SendMessageA(d->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(d->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->wt_list, NULL, TRUE);
}

static void add_break(debugger *d)
{
    char text[128], val[32];
    const int type = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    uint16_t lo = 0, hi = 0;
    intvdebug_bp bp;
    const char *v = val;

    GetWindowTextA(d->bp_addr, text, sizeof(text));
    GetWindowTextA(d->bp_value, val, sizeof(val));
    memset(&bp, 0, sizeof(bp));
    if (type == 0 ? !intvsym_parse_addr(d->symtab, text, &lo)
                  : !intvsym_parse_range(d->symtab, text, &lo, &hi, NULL)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "No such symbol, variable or address: %s", text);
        SetWindowTextA(d->status, msg);
        return;
    }
    if (type == 0)
        hi = lo;
    bp.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE);
    bp.lo = lo;
    bp.hi = hi;
    bp.cond = (int)SendMessageA(d->bp_cond, CB_GETCURSEL, 0, 0);
    if (*v == '$')
        v++;
    bp.value = (uint16_t)strtoul(v, NULL, 16);
    bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF; /* the scratchpad is 8 bits */
    if (intvdebug_bp_add(d->dbg, &bp) < 0) {
        SetWindowTextA(d->status, "The breakpoint table is full");
        return;
    }
    SetWindowTextA(d->bp_addr, "");
    d->seen_serial = 0;
}

static void add_watch(debugger *d)
{
    char text[64];
    GetWindowTextA(d->wt_entry, text, sizeof(text));
    if (intvwatch_add(text) < 0) {
        SetWindowTextA(d->status, "Type an address, symbol or BASIC variable (x, #score, a(3))");
        return;
    }
    SetWindowTextA(d->wt_entry, "");
    d->seen_serial = 0;
}

static void do_search(debugger *d, int backward)
{
    char text[200];
    uint16_t pat[64], mask, found, from;
    int flags, n;

    GetWindowTextA(d->srch_entry, text, sizeof(text));
    n = intvsearch_parse((int)SendMessageA(d->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &mask, &flags);
    if (!n) {
        SetWindowTextA(d->srch_note, "Type hex words (02A0 0268) or some text.");
        return;
    }
    if (!intvdebug_is_paused(d->dbg)) {
        SetWindowTextA(d->srch_note, "Pause the machine to search its memory.");
        return;
    }
    from = d->mem_hl ? (backward ? d->mem_hl_lo : (uint16_t)(d->mem_hl_lo + 1)) : d->mem_base;
    if (intvsearch_next(d->dbg, pat, n, mask, flags, from, backward, &found)) {
        char note[40];
        snprintf(note, sizeof(note), "Found at $%04X", found);
        SetWindowTextA(d->srch_note, note);
        show_in_memory(d, found, (uint16_t)(found + n - 1));
    } else {
        SetWindowTextA(d->srch_note, "Not found.");
    }
}

static void selected_watch_range(debugger *d, uint16_t *lo, uint16_t *hi, int *ok)
{
    const int sel = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    *ok = sel >= 0 && intvwatch_range(d->symtab, sel, lo, hi);
}

/* ---- Cart, Trace and History pages ------------------------------------------- */

static void refresh_cart(debugger *d)
{
    intvdebug_cart cart;
    intvdebug_region reg[64];
    char text[4096];
    int len = 0, n, i;

    intvdebug_cart_info(d->dbg, &cart);
    len += snprintf(text + len, sizeof(text) - (size_t)len, "Cartridge\r\n  File     %s\r\n",
                    cart.path[0] ? cart.path : "(none)");
    if (cart.name[0])
        len += snprintf(text + len, sizeof(text) - (size_t)len, "  Title    %s%s%.0d\r\n", cart.name,
                        cart.year ? ", " : "", cart.year);
    len += snprintf(text + len, sizeof(text) - (size_t)len,
                    "  FujiNet  %s\r\n  ECS      %s\r\n  JLP      %s\r\n\r\nMemory map\r\n",
                    cart.fujinet_mailbox ? "mailbox mapped at $9C00" : "no mailbox", cart.ecs ? "present" : "no",
                    cart.jlp ? "present" : "no");
    n = intvdebug_memory_map(d->dbg, reg, 64);
    if (!n)
        len += snprintf(text + len, sizeof(text) - (size_t)len, "  Pause the machine to read its memory map.\r\n");
    for (i = 0; i < n && len < (int)sizeof(text) - 96; i++)
        len += snprintf(text + len, sizeof(text) - (size_t)len, "  %04X-%04X  %c%c  %s\r\n", reg[i].lo, reg[i].hi,
                        reg[i].readable ? 'r' : '-', reg[i].writable ? 'w' : '-', reg[i].name);
    {
        char cur[4096];
        GetWindowTextA(d->cart_text, cur, sizeof(cur));
        if (strcmp(cur, text) != 0) /* unchanged keeps a selection */
            set_text_at(d->cart_text, text, 0);
    }
}

#define HIST_SHOWN 400   /* lines on the page */

static void hist_refresh(debugger *d, int idx, int force)
{
    static intvdebug_hist buf[4000];
    static char text[HIST_SHOWN * 240];
    const int live = idx == 0;
    const int paused = intvdebug_is_paused(d->dbg);
    const int on = intvdebug_history_enabled(d->dbg);
    const uint64_t count = intvdebug_history_count(d->dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = 0, n, i, shown = 0, hit[HIST_SHOWN];
    size_t len = 0;
    char note[96];

    SendMessageA(d->h_rec[0], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);   /* both Record buttons follow the engine */
    SendMessageA(d->h_rec[1], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(d->h_rec[0], NULL, FALSE);
    InvalidateRect(d->h_rec[1], NULL, FALSE);

    if (!force && count == d->h_seen[idx] && (live || paused))
        return;
    d->h_seen[idx] = count;
    if (!live && !paused) {
        SetWindowTextA(d->h_note[idx], "Pause the machine to read its history.");
        return;
    }
    switch ((int)SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0)) {
    case 1: filter = INTVDEBUG_HIST_BRANCHES; break;
    case 2: {
        char r[64];
        GetWindowTextA(d->h_range[idx], r, sizeof(r));
        if (intvsym_parse_range(d->symtab, r, &lo, &hi, NULL))
            filter = INTVDEBUG_HIST_RANGE;
        break;
    }
    }
    n = intvdebug_history_copy(d->dbg, buf, 4000);
    for (i = n - 1; i >= 0 && shown < HIST_SHOWN; i--)   /* the newest matches, scanning back */
        if (intvdebug_history_match(&buf[i], filter, lo, hi))
            hit[shown++] = i;
    text[0] = '\0';
    if (!on && n == 0)
        snprintf(text, sizeof(text), "History is off. Press Record, then let the machine run.");
    for (i = shown - 1; i >= 0 && len + 260 < sizeof(text); i--) {   /* oldest first */
        char line[256];
        intvdebug_history_format(&buf[hit[i]], line, sizeof(line));
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s\r\n", line);
    }
    set_text_at(d->h_text[idx], text, 1);
    snprintf(note, sizeof(note), "%s%llu instructions recorded, last %d shown", on ? "Recording. " : "",
             (unsigned long long)count, shown);
    SetWindowTextA(d->h_note[idx], note);
}

static void refresh_all(debugger *d)
{
    int paused = intvdebug_is_paused(d->dbg);
    SetWindowTextA(d->pause_btn, paused ? "Continue (F5)" : "Pause (F5)");
    {
        intvdebug_stop_info st;
        char why[160], status[220];
        why[0] = '\0';
        if (paused && intvdebug_last_stop(d->dbg, &st) && st.reason != INTVDEBUG_STOP_BREAKPOINT)
            stop_text(&st, why, sizeof(why));
        if (why[0]) {
            snprintf(status, sizeof(status), "Paused: %s", why);
            SetWindowTextA(d->status, status);
        } else {
            SetWindowTextA(d->status, paused ? "Paused" : "Running");
        }
    }

    if (paused) {
        refresh_regs(d);
        refresh_disasm(d);
        refresh_mem(d);
        refresh_stic(d);
    } else {
        int i;
        for (i = 0; i < 8; i++) SetWindowTextA(d->reg_edit[i], "");
        SetWindowTextA(d->flags, "");
        lv_begin(d->disasm);
        lv_end(d->disasm, -1);
        SetWindowTextA(d->mem_view, "");
    }
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refresh_symbols's own comment). */
    refresh_symbols(d);
    refresh_basic(d, paused);
    refresh_breaks(d);
    refresh_watches(d);
    refresh_cart(d);
    hist_refresh(d, 0, 0);
    hist_refresh(d, 1, 0);
    if (page_visible(d, PAGE_FUJINET))
        refresh_fujinet(d, 0);
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
        if (!d)         /* a notification during teardown */
            return 0;
        if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == d->pause_btn) /* Continue wears the accent */
            return intv_accent_customdraw((const NMCUSTOMDRAW *)lp, intvdebug_is_paused(d->dbg));
        if ((nm->hwndFrom == d->h_rec[0] || nm->hwndFrom == d->h_rec[1]) && nm->code == NM_CUSTOMDRAW) /* Record: the accent while on */
            return intv_accent_customdraw((const NMCUSTOMDRAW *)lp,
                                          SendMessageA(nm->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (nm->hwndFrom == d->tabs && nm->code == TCN_SELCHANGE) {
            const int idx = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < d->ntabs)
                show_page(d, d->tab_page[idx]);
        }
        return 0;
    }
    case WM_TIMER:
        if (d) {
            uint64_t serial = intvdebug_stop_serial(d->dbg);
            if (serial != d->seen_serial ||
                intvdebug_is_paused(d->dbg) != d->was_paused) {
                d->seen_serial = serial;
                refresh_all(d);
            } else if (page_visible(d, PAGE_TRACE) && ++d->h_ticks >= 4) {
                d->h_ticks = 0;   /* the live view moves while the machine runs */
                hist_refresh(d, 0, 0);
            } else if (page_visible(d, PAGE_FUJINET) &&
                       ++d->fuji_ticks >= FUJI_TICKS) {
                /* the one view that moves while the machine runs */
                refresh_fujinet(d, 0);
            }
        }
        return 0;
    case WM_DBG_LINE:
        if (!d)
            return 0;
        if (wp == IDC_DISASM) {
            intvdebug_breakpoint_toggle(d->dbg, (uint16_t)lp);
            d->seen_serial = 0;
        } else if (wp == IDC_BAS_VIEW && d->bas_file >= 0) {
            const int addr = intvsrcmap_first_addr(d->srcmap, d->bas_file, (int)lp);
            if (addr >= 0) {
                intvdebug_breakpoint_toggle(d->dbg, (uint16_t)addr);
                d->seen_serial = 0;
            } else {
                SetWindowTextA(d->status, "No code on that line");
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        if (!d)
            return 0;
        if (wp == IDC_H_RANGE0 || wp == IDC_H_RANGE1) {
            d->h_seen[0] = d->h_seen[1] = (uint64_t)-1;
            d->seen_serial = 0;
        } else if (wp == IDC_BP_ADDR || wp == IDC_BP_VALUE) {
            add_break(d);
        } else if (wp == IDC_WT_ENTRY) {
            add_watch(d);
        } else if (wp == IDC_SRCH_ENTRY) {
            do_search(d, 0);
        } else if (wp == IDC_MEM_ADDR) {
            char buf[64];
            uint16_t lo, hi;
            GetWindowTextA(d->mem_addr, buf, sizeof(buf));
            if (intvsym_parse_range(d->symtab, buf, &lo, &hi, NULL)) {
                show_in_memory(d, lo, hi);
            } else {
                char msg[96];
                snprintf(msg, sizeof(msg), "No such symbol or address: %s",
                        buf);
                SetWindowTextA(d->status, msg);
            }
        } else if (wp == IDC_BREAK_ADDR) {
            char buf[64];
            uint16_t addr;
            GetWindowTextA(d->break_addr, buf, sizeof(buf));
            if (intvsym_parse_addr(d->symtab, buf, &addr)) {
                intvdebug_breakpoint_toggle(d->dbg, addr);
                d->seen_serial = 0; /* redraw the marker immediately */
            } else {
                char msg[96];
                snprintf(msg, sizeof(msg), "No such symbol or address: %s",
                        buf);
                SetWindowTextA(d->status, msg);
            }
        } else if (wp == IDC_SYM_FILTER) {
            /* refresh_symbols itself detects the text change; nothing to
             * store here -- this just gives the filter box the same
             * Enter-to-apply convention as mem_addr/break_addr above. */
            refresh_symbols(d);
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
        case IDC_STEP:
        case IDC_STEP_OVER:
        case IDC_STEP_OUT:
            do_step(d, LOWORD(wp));
            return 0;
        case IDC_BAS_FILE:
        case IDC_BAS_FOLDER:
        case IDC_BAS_MAP:
            basic_pick(d, LOWORD(wp));
            return 0;
        case IDC_POPOUT: pop_out(d, d->page); return 0;
        case IDC_H_REC0:
        case IDC_H_REC1:
            intvdebug_history_enable(d->dbg, SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            d->seen_serial = 0;
            return 0;
        case IDC_H_CLEAR0:
        case IDC_H_CLEAR1:
            intvdebug_history_clear(d->dbg);
            d->h_seen[0] = d->h_seen[1] = (uint64_t)-1;
            d->seen_serial = 0;
            return 0;
        case IDC_H_FILTER0:
        case IDC_H_FILTER1:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                d->h_seen[0] = d->h_seen[1] = (uint64_t)-1;
                d->seen_serial = 0;
            }
            return 0;
        case IDC_BP_ADD: add_break(d); return 0;
        case IDC_WT_ADD: add_watch(d); return 0;
        case IDC_SRCH_NEXT: do_search(d, 0); return 0;
        case IDC_SRCH_PREV: do_search(d, 1); return 0;
        case IDC_BP_TOGGLE:
        case IDC_BP_REMOVE: {
            const int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                const int id = (int)SendMessageA(d->bp_list, LB_GETITEMDATA, (WPARAM)sel, 0);
                intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
                const int n = intvdebug_bp_list(d->dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
                int i;
                if (LOWORD(wp) == IDC_BP_REMOVE)
                    intvdebug_bp_remove(d->dbg, id);
                else
                    for (i = 0; i < n; i++)
                        if (bps[i].id == id)
                            intvdebug_bp_enable(d->dbg, id, !bps[i].enabled);
                d->seen_serial = 0;
            }
            return 0;
        }
        case IDC_BP_HITS:
            intvdebug_bp_clear_hits(d->dbg);
            d->seen_serial = 0;
            return 0;
        case IDC_BP_CLEAR:
            intvdebug_breakpoint_clear_all(d->dbg);
            d->seen_serial = 0;
            return 0;
        case IDC_WT_MEM: {
            uint16_t lo, hi;
            int ok;
            selected_watch_range(d, &lo, &hi, &ok);
            if (ok) {
                goto_page(d, PAGE_MEMORY);
                show_in_memory(d, lo, hi);
            }
            return 0;
        }
        case IDC_WT_BRK: {
            uint16_t lo, hi;
            int ok;
            selected_watch_range(d, &lo, &hi, &ok);
            if (ok) {
                intvdebug_bp bp;
                memset(&bp, 0, sizeof(bp));
                bp.type = INTVDEBUG_BP_WRITE;
                bp.lo = lo;
                bp.hi = hi;
                bp.cond = INTVDEBUG_COND_CHANGED;
                bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF;
                if (intvdebug_bp_add(d->dbg, &bp) >= 0)
                    SetWindowTextA(d->status, "Breaking when it changes");
                d->seen_serial = 0;
            }
            return 0;
        }
        case IDC_WT_REMOVE: {
            const int sel = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                intvwatch_remove(sel);
                d->seen_serial = 0;
            }
            return 0;
        }
        case IDC_BAS_FILES:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageA(d->bas_files, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    d->bas_file = sel;
                    d->bas_pc_line = 0;
                    lv_top(d->bas_view);
                    d->seen_serial = 0;
                    refresh_basic(d, intvdebug_is_paused(d->dbg));
                }
            }
            return 0;
        case IDC_CLEAR_BP:
            intvdebug_breakpoint_clear_all(d->dbg);
            d->seen_serial = 0;
            return 0;
        case IDC_LOAD_SYMS: {
            char path[MAX_PATH] = "";
            char last_dir[1024] = "";
            OPENFILENAMEA ofn;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "as1600 symbol files (*.sym)\0*.sym\0"
                              "All files (*.*)\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = sizeof(path);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            /* Remember the directory (settings key "sym_dir") so re-opening
             * the picker -- expected to happen often, since loading is
             * always a manual step taken after CONFIG has already booted a
             * cart, never automatic -- starts back where the user just
             * was. */
            if (d->session) {
                const char *dir = intvsession_get_str(d->session, "sym_dir", "");
                if (dir[0]) {
                    snprintf(last_dir, sizeof(last_dir), "%s", dir);
                    ofn.lpstrInitialDir = last_dir;
                }
            }
            if (GetOpenFileNameA(&ofn) && intvsymtab_load(d->symtab, path) >= 0) {
                if (d->session) {
                    char dir[1024];
                    if (intvsym_dirname(path, dir, sizeof(dir)) > 0) {
                        intvsession_set_str(d->session, "sym_dir", dir);
                        intvsession_settings_flush(d->session);
                    }
                }
                d->seen_serial = 0;
                if (intvdebug_is_paused(d->dbg)) refresh_all(d);
            }
            return 0;
        }
        case IDC_FUJI_CLEAR:
            intvdebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDC_HELP_BTN: intv_help_for(d->hwnd); return 0;
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
            int pp;
            KillTimer(hwnd, TIMER_REFRESH);
            for (pp = 0; pp < PAGE_COUNT; pp++)   /* a peeled-off page's window goes with the debugger */
                if (d->popped[pp]) {
                    HWND w2 = d->popped[pp];
                    d->popped[pp] = NULL;
                    DestroyWindow(w2);
                }
            /* Releasing costs the machine nothing further (see
             * intvdebug.h) and leaving it engaged+paused would look like
             * the emulator had hung. */
            intvdebug_set_engaged(d->dbg, 0);
            /* d->symtab is intvsymtab_shared() -- process-wide, not owned
             * by this window -- so it is deliberately NOT destroyed here;
             * see this file's header comment. */
            DeleteObject(d->mono);
            g_dbg = NULL;
            /* Clear the back-pointer before freeing: DestroyWindow goes on
             * to send WM_NCDESTROY/WM_NOTIFY from the children being torn
             * down, and dbg_wnd_proc reads GWLP_USERDATA for each of them. */
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
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
    {
        HWND root = GetAncestor(msg->hwnd, GA_ROOT);
        int mine = msg->hwnd == g_dbg->hwnd || GetParent(msg->hwnd) == g_dbg->hwnd || root == g_dbg->hwnd, pp;
        for (pp = 0; !mine && pp < PAGE_COUNT; pp++)   /* a peeled-off page's window too */
            if (g_dbg->popped[pp] && (root == g_dbg->popped[pp] || msg->hwnd == g_dbg->popped[pp]))
                mine = 1;
        if (!mine)
            return 0;
    }
    if (msg->message != WM_KEYDOWN)
        return 0;
    switch (msg->wParam) {
    case VK_F5:
        SendMessageA(g_dbg->hwnd, WM_COMMAND, IDC_PAUSE, 0);
        return 1;
    case VK_F7:
        do_step(g_dbg, IDC_STEP);
        return 1;
    case VK_F8:
        do_step(g_dbg, GetKeyState(VK_SHIFT) & 0x8000 ? IDC_STEP_OUT : IDC_STEP_OVER);
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
        pc.lpfnWndProc = page_proc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursor(NULL, IDC_ARROW);
        pc.lpszClassName = "IntvDbgPage";
        RegisterClassA(&pc);

        memset(&pc, 0, sizeof(pc));
        pc.lpfnWndProc = pop_proc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursor(NULL, IDC_ARROW);
        pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        pc.lpszClassName = "IntvDbgPop";
        RegisterClassA(&pc);

        memset(&pc, 0, sizeof(pc));
        pc.lpfnWndProc = lines_proc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursor(NULL, IDC_ARROW);
        pc.lpszClassName = "IntvDbgLines";
        RegisterClassA(&pc);

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
    d->symtab = intvsymtab_shared();
    d->srcmap = intvsrcmap_shared();
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH | FF_MODERN, "Consolas");
    d->mono_h = 15;

    d->hwnd = CreateWindowExA(0, "IntvDebuggerWindow",
                              "Intellivision Debugger", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrA(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);
    g_dbg = d;

    {
        HDC dc = GetDC(d->hwnd);
        HGDIOBJ prev = SelectObject(dc, d->mono);
        TEXTMETRICA tm;
        if (GetTextMetricsA(dc, &tm))
            d->mono_h = tm.tmHeight;
        SelectObject(dc, prev);
        ReleaseDC(d->hwnd, dc);
    }

    build_controls(d);
    layout(d);
    {
        int tab = tab_from_env();
        if (tab >= 0)
            SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)tab, 0);
        show_page(d, tab >= 0 ? tab : PAGE_CPU);
        if (getenv("INTV_HISTORY"))   /* record from the start, for captures */
            intvdebug_history_enable(d->dbg, 1);
        if (tab_from_env_var("INTV_POPOUT") >= 0)   /* peel a tab off at the start, for captures */
            pop_out(d, tab_from_env_var("INTV_POPOUT"));
    }

    /* For captures and tests, like INTV_DEBUGGER_TAB: INTV_BASIC_IMPORT=<.bas
     * file or folder> imports it as Import File / Import Folder would, and
     * INTV_BASIC_BREAK=<name.bas>:<line> sets a breakpoint on that line. */
    {
        const char *imp = getenv("INTV_BASIC_IMPORT");
        const char *brk = getenv("INTV_BASIC_BREAK");
        if (imp && *imp)
            basic_import(d, imp, 0);
        if (brk && strchr(brk, ':')) {
            char name[260];
            const int line = atoi(strchr(brk, ':') + 1);
            int f;
            snprintf(name, sizeof(name), "%.*s", (int)(strchr(brk, ':') - brk), brk);
            for (f = 0; f < intvsrcmap_file_count(d->srcmap); f++) {
                const int addr = intvsrcmap_first_addr(d->srcmap, f, line);
                if (strcmp(intvsrcmap_file_name(d->srcmap, f), name) == 0 && addr >= 0 &&
                    !intvdebug_breakpoint_is_set(d->dbg, (uint16_t)addr))
                    intvdebug_breakpoint_toggle(d->dbg, (uint16_t)addr);
            }
        }
    }

    /* More captures: INTV_WATCH="x" adds a watch, INTV_WATCH_BREAK="x" a
     * write-when-changed breakpoint on it, INTV_MEM_GOTO="x" shows it in Memory. */
    {
        const char *wt = getenv("INTV_WATCH"), *wb = getenv("INTV_WATCH_BREAK"),
                   *mg = getenv("INTV_MEM_GOTO");
        uint16_t lo, hi;
        if (wt && *wt)
            intvwatch_add(wt);
        if (wb && *wb && intvsym_parse_range(d->symtab, wb, &lo, &hi, NULL)) {
            intvdebug_bp bp;
            memset(&bp, 0, sizeof(bp));
            bp.type = INTVDEBUG_BP_WRITE;
            bp.lo = lo;
            bp.hi = hi;
            bp.cond = INTVDEBUG_COND_CHANGED;
            bp.mask = 0xFF;
            intvdebug_bp_add(d->dbg, &bp);
        }
        if (mg && *mg && intvsym_parse_range(d->symtab, mg, &lo, &hi, NULL))
            show_in_memory(d, lo, hi);
    }

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(d->dbg, 1);
    intvdebug_pause(d->dbg);

    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    refresh_all(d);

    ShowWindow(d->hwnd, SW_SHOW);
}
