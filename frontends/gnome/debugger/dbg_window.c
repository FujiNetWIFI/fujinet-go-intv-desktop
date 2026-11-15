/*
 * The CP-1610/STIC debugger window (GTK4/libadwaita), modelled on the CoCo
 * port's own dbg_window.c (poll-based stop tracking: cocodebug_stop_serial
 * there, intvdebug_stop_snapshot here -- see that file's header for why this
 * is a poll and not a callback). Thirteen tabs: Console, Disassembly, BASIC,
 * Memory (with a Poke bar), Breakpoints (conditions are expressions),
 * Watches, Cart & I/O, Trace, History, Video (dbg_video.c; the old STIC panes
 * are its Decoded sources), Sound, Symbols and FujiNet, with an editable
 * register strip under all of them.
 *
 * A FujiNet tab, last, shows every FujiBus packet between the machine and
 * FujiNet (intvdebug_fujinet_trace, always on) beside the mailbox's state.
 * It is the one view that changes while the machine runs, so the frame
 * clock also refreshes it twice a second while it shows.
 *
 * The BASIC tab shows IntyBASIC source (core/debugger/srcmap.h) beside the
 * stepping: on it, Step / Step Over / Step Out go by BASIC line. Like the
 * symbol table, the imported source is process-wide (intvsrcmap_shared()),
 * so it survives closing this window.
 *
 * The disassembly, register and memory views drive intvdebug.h directly.
 * The symbol table (core/debugger/symbols.h) is not owned by intvdebug --
 * unlike cocodebug's built-in one -- so this window uses the process-wide
 * intvsymtab_shared() instead of its own instance: a cart arrives through
 * FujiNet CONFIG with no path a .sym could ever be derived from, so a user
 * loading one by hand is the only way symbols get here at all, and that
 * work should survive closing this window, not be thrown away with it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/dbg_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intvdebug.h"
#include "memtools.h"
#include "srcmap.h"
#include "symbols.h"
#include "../help/help_window.h"
#include "debugger/dbg_video.h"

#define DISASM_LINES 32
#define MEM_WORDS_PER_ROW 8
#define MEM_ROWS 16
#define POKE_MAX 32
#define FUJI_REFRESH_US 500000 /* the FujiNet tab, while it shows */

enum { TAB_CONSOLE, TAB_DISASM, TAB_BASIC, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART,
       TAB_TRACE, TAB_HISTORY, TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The notebook's pages, in order. */
static const char *const k_tab_names[] = { "Console", "Disassembly", "BASIC", "Memory", "Breakpoints",
    "Watches", "Cart & I/O", "Trace", "History", "Video", "Sound", "Symbols", "FujiNet" };

/* Names INTV_DEBUGGER_TAB and INTV_POPOUT also take: the tabs' older names. */
static const struct { const char *name; int tab; } k_tab_aliases[] = {
    { "cpu", TAB_DISASM }, { "stic", TAB_VIDEO }, { "cart", TAB_CART }, { "io", TAB_CART },
    { "i/o", TAB_CART }, { "psg", TAB_SOUND },
};

/* The register strip: R0-R7 (R6 is the stack, growing up; R7 is the PC) and the flags. */
#define REG_COUNT 8
#define FLAG_COUNT 5
static const struct { const char *name; int reg; } k_flags[FLAG_COUNT] = {
    { "S", INTVDEBUG_REG_S }, { "Z", INTVDEBUG_REG_Z }, { "O", INTVDEBUG_REG_O },
    { "C", INTVDEBUG_REG_C }, { "I", INTVDEBUG_REG_I },
};

/* A breakpoint in a gutter: a red dot. */
#define BP_DOT "<span foreground=\"#e01b24\">\xe2\x97\x8f</span>"
/* a disabled one: a hollow grey dot */
#define BP_DOT_OFF "<span foreground=\"#8a8a8a\">\xe2\x97\x8b</span>"

typedef struct {
    GtkWindow *win;
    intvsession *session;
    intvdebug *dbg;
    intvsymtab *symtab;

    GtkLabel *status;
    AdwToastOverlay *toasts;
    GtkButton *run_pause;
    GtkListBox *disasm;

    /* Console tab */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the register strip under every tab */
    GtkEntry *reg[REG_COUNT];
    GtkEntry *flag[FLAG_COUNT];
    GtkLabel *dflag, *intr;
    gboolean force_entries;   /* a refresh may overwrite a focused entry */

    GtkEntry *mem_addr;
    GtkEntry *poke_entry;
    GtkLabel *mem;
    uint16_t mem_view_addr;
    GtkEntry *break_addr;

    GtkListBox *symlist;
    GtkEntry *sym_filter;
    GtkLabel *sym_count;
    unsigned sym_seen_gen;
    char sym_filter_text[64];

    /* BASIC tab */
    intvsrcmap *srcmap;
    GtkLabel *bas_note;
    GtkDropDown *bas_files;
    GtkStringList *bas_names;
    GtkScrolledWindow *bas_scroll;
    GtkListBox *bas_list;
    GPtrArray *bas_labels;   /* one GtkLabel per line of the file shown */
    GArray *bas_marks;       /* char[3] per line: what its gutter shows now */
    int bas_file;            /* index into the map; -1 = nothing shown */
    unsigned bas_gen;        /* intvsrcmap_generation() the list was built from */
    int bas_scroll_line;     /* a line to bring into view once it has a size */
    gboolean bas_syncing;    /* dropdown being set by us, not the user */

    /* Memory tab: the highlighted words, and the search */
    gboolean mem_hl;
    uint16_t mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

    /* Breakpoints tab */
    GtkDropDown *bp_type;
    GtkEntry *bp_addr, *bp_cond;
    GtkListBox *bp_list;
    GtkLabel *bp_stop;
    unsigned bp_gen;

    /* Watches tab */
    GtkEntry *wt_entry;
    GtkListBox *wt_list;

    /* Cart & I/O and Sound tabs */
    GtkTextView *cart_text, *io_text, *sound_text;

    /* Trace (0, live) and History (1, when stopped) pages share one ring */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkTextView *h_text[2];
    GtkLabel *h_note[2];
    uint64_t h_seen[2];
    gint64 h_at;
    gboolean h_sync;

    /* Tabs that have been peeled off into windows of their own */
    GtkWidget *pages[TAB_COUNT];
    GtkWindow *detached[TAB_COUNT];

    GtkNotebook *notebook;
    GtkWidget *fuji_page;
    GtkNotebook *tabs;
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
    gint64 fuji_at;

    guint tick_id;
    int kick;            /* ticks to keep running while paused after an action */
    gulong map_id, unmap_id, susp_id;
    uint64_t seen_serial;
    unsigned seen_gen;   /* intvdebug_generation at the last refresh */
    int was_paused;
} DbgWin;

/* The tick (one per display frame) runs only while the window is mapped and
 * there is something to watch: the machine running (a breakpoint can stop it
 * at any time), or a short while after an action while paused (a step lands
 * asynchronously). A paused debugger with nothing happening costs no
 * wakeups. */
static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data);

static void update_tick(DbgWin *w)
{
    gboolean want = w->win && gtk_widget_get_mapped(GTK_WIDGET(w->win)) &&
                    !gtk_window_is_suspended(w->win) && w->dbg &&
                    (!intvdebug_is_paused(w->dbg) || w->kick > 0);

    if (want && !w->tick_id)
        w->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(w->win), tick_cb,
                                                  w, NULL);
    else if (!want && w->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w->win), w->tick_id);
        w->tick_id = 0;
    }
}

/* An action that will change what the panes show: keep ticking for a second
 * or so after it (paused), until the result has landed. */
static void kick(DbgWin *w)
{
    w->kick = 60;
    update_tick(w);
}

/* Force the next tick to rebuild the panes. */
static void invalidate(DbgWin *w)
{
    w->seen_serial = 0;
    kick(w);
}

/* ---- logs ------------------------------------------------------------------ */

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

static gboolean at_end(GtkAdjustment *adj)
{
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
           gtk_adjustment_get_upper(adj) - 2.0;
}

/* A log follows its end while its reader is there. New text grows the view
 * only once it is laid out (or its tab first shows), so the end is pinned
 * whenever the view's height changes; whether to follow is decided as text
 * arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    /* _object: the handler goes when the view does, so a scroll adjustment
     * that changes while the window is torn down never calls into it. */
    g_signal_connect_object(vadj(view), "changed",
                            G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 || at_end(adj)));
}

static void log_to_end(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    on_log_resized(vadj(view), view);
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    GtkTextMark *mark;
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, text, -1);
    if (follow) {
        log_to_end(view);
        return;
    }
    if (top <= 0) return;
    /* scroll_to_mark waits for the new text's layout */
    gtk_text_buffer_get_iter_at_line(b, &it, top);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(b, mark);
}

/* ---- refreshers ----------------------------------------------------------- */

/* ---- text helpers and messages -------------------------------------------- */

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = g_utf8_make_valid(text, -1);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, # for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t n;
    long val;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    n = strlen(buf);
    while (n && buf[n - 1] == ' ') buf[--n] = '\0';
    if (!n) return 0;
    if (buf[0] == '$') val = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) val = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') val = strtol(buf + 1, &end, 10);
    else if (buf[n - 1] == 'h' || buf[n - 1] == 'H') { buf[n - 1] = '\0'; val = strtol(buf, &end, 16); }
    else val = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = val;
    return 1;
}

/* A message that must outlast the next refresh (the status label is rewritten by it). */
static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* ---- registers --------------------------------------------------------------- */

/* The register entries show what the engine has, but a refresh leaves an entry
 * alone while it has the focus and holds something else (a half-typed value),
 * until Enter (force_entries) ends the edit. */
static void set_entry(DbgWin *w, GtkEntry *entry, const char *text)
{
    const char *shown = gtk_editable_get_text(GTK_EDITABLE(entry));
    gboolean focus = gtk_widget_has_focus(GTK_WIDGET(entry)) ||
                     gtk_widget_is_focus(GTK_WIDGET(entry)) ||
                     gtk_widget_has_focus(gtk_widget_get_first_child(GTK_WIDGET(entry)));
    const char *last = g_object_get_data(G_OBJECT(entry), "shown");

    if (focus && !w->force_entries && last && strcmp(shown, last) != 0)
        return;
    if (strcmp(shown, text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(entry), text);
    g_object_set_data_full(G_OBJECT(entry), "shown", g_strdup(text), g_free);
}

static void refresh_regs(DbgWin *w)
{
    intvdebug_regs r;
    int i;
    char buf[16];
    gboolean paused = intvdebug_regs_get(w->dbg, &r) != 0;

    for (i = 0; i < REG_COUNT; i++)
        gtk_widget_set_sensitive(GTK_WIDGET(w->reg[i]), paused);
    for (i = 0; i < FLAG_COUNT; i++)
        gtk_widget_set_sensitive(GTK_WIDGET(w->flag[i]), paused);
    if (!paused) {
        for (i = 0; i < REG_COUNT; i++)
            set_entry(w, w->reg[i], "");
        for (i = 0; i < FLAG_COUNT; i++)
            set_entry(w, w->flag[i], "");
        gtk_label_set_text(w->dflag, "D -");
        gtk_label_set_text(w->intr, "");
        return;
    }
    for (i = 0; i < REG_COUNT; i++) {
        g_snprintf(buf, sizeof buf, "%04X", r.r[i]);
        set_entry(w, w->reg[i], buf);
    }
    {
        const int values[FLAG_COUNT] = { r.S, r.Z, r.O, r.C, r.I };
        for (i = 0; i < FLAG_COUNT; i++) {
            g_snprintf(buf, sizeof buf, "%d", values[i] ? 1 : 0);
            set_entry(w, w->flag[i], buf);
        }
    }
    g_snprintf(buf, sizeof buf, "D %d%s", r.D ? 1 : 0, r.D ? " (SDBD pending)" : "");
    gtk_label_set_text(w->dflag, buf);
    {
        char text[96];
        g_snprintf(text, sizeof text, "%s   INTRQ %d  BUSRQ %d   frame %u   cycle %" G_GUINT64_FORMAT,
                   r.interruptible ? "interruptible" : "not interruptible", r.intrq, r.busrq, r.frame, r.cycles);
        gtk_label_set_text(w->intr, text);
    }
}

static void refresh_all(DbgWin *w);

/* Enter in a register entry sets it while the machine is stopped. */
static void on_reg_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    const int reg = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long val;

    if (!intvdebug_is_paused(w->dbg))
        toast(w, "Stop the machine to edit the registers");
    else if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &val))
        toast(w, "A register takes a number: hex by default, # for decimal");
    else if (!intvdebug_regs_set(w->dbg, reg, (unsigned)val))
        toast(w, "That register could not be set");
    /* Enter ends the edit: show the engine's value, or put the old one back */
    w->force_entries = TRUE;
    refresh_all(w);
    w->force_entries = FALSE;
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                             gpointer user_data)
{
    DbgWin *w = user_data;
    gpointer addr;
    (void)box;

    addr = g_object_get_data(G_OBJECT(row), "intv-addr");
    intvdebug_breakpoint_toggle(w->dbg, (uint16_t)GPOINTER_TO_UINT(addr));
    invalidate(w); /* redraw the marker immediately */
}

/* The gutter's mark for an EXEC breakpoint's state (0 none, 1 on, 2 off). */
static const char *bp_dot(int state)
{
    return state == 1 ? BP_DOT : state == 2 ? BP_DOT_OFF : " ";
}

static void refresh_disasm(DbgWin *w)
{
    intvdebug_dasm_line lines[DISASM_LINES];
    intvdebug_regs r;
    int n, i;

    gtk_list_box_remove_all(w->disasm);
    if (!intvdebug_regs_get(w->dbg, &r))
        return;

    n = intvdebug_disassemble(w->dbg, r.pc, r.D, lines, DISASM_LINES);
    for (i = 0; i < n; i++) {
        const intvdebug_dasm_line *l = &lines[i];
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);
        char symsuffix[96];
        g_autofree char *body = NULL;
        g_autofree char *esc = NULL;
        g_autofree char *text = NULL;

        intvsym_annotate_line(w->symtab, l, symsuffix, sizeof(symsuffix));
        body = g_strdup_printf("%04X  %-24s%s", l->addr, l->text, symsuffix);
        esc = g_markup_escape_text(body, -1);
        text = g_strdup_printf(
            "%s %s", bp_dot(intvdebug_breakpoint_state(w->dbg, l->addr)), esc);
        gtk_label_set_markup(GTK_LABEL(label), text);
        if (l->addr == r.pc)   /* the machine is here: a bar in the accent */
            gtk_widget_add_css_class(row, "intv-pcrow");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "intv-addr",
                          GUINT_TO_POINTER((guint)l->addr));
        gtk_list_box_append(w->disasm, row);
    }
    {   /* the list always starts at the PC: a stop shows it, not wherever the reader had scrolled */
        GtkWidget *sw = gtk_widget_get_ancestor(GTK_WIDGET(w->disasm), GTK_TYPE_SCROLLED_WINDOW);
        if (sw)
            gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)), 0);
    }
}

/* Words are 16 bits wide, but the scratchpad, GROM, GRAM and the ECS RAM are
 * 8: those show two digits, padded so the columns line up. */
static void refresh_mem(DbgWin *w)
{
    uint16_t words[MEM_WORDS_PER_ROW * MEM_ROWS];
    GString *text = g_string_new(NULL);
    int n, row, col;
    const char *last_region = NULL;

    n = intvdebug_read(w->dbg, w->mem_view_addr, words,
                       MEM_WORDS_PER_ROW * MEM_ROWS);
    for (row = 0; row * MEM_WORDS_PER_ROW < n; row++) {
        uint16_t row_addr = (uint16_t)(w->mem_view_addr + row * MEM_WORDS_PER_ROW);
        const char *region = intvsym_region(row_addr);

        g_string_append_printf(text, "%04X ", row_addr);
        for (col = 0; col < MEM_WORDS_PER_ROW; col++) {
            int idx = row * MEM_WORDS_PER_ROW + col;
            const uint16_t a = (uint16_t)(row_addr + col);
            const gboolean narrow = intvdebug_word_width(w->dbg, a) == 8;
            char cell[16];
            if (idx >= n)
                break;
            if (narrow)
                g_snprintf(cell, sizeof cell, "  %02X", words[idx] & 0xFF);
            else
                g_snprintf(cell, sizeof cell, "%04X", words[idx]);
            if (w->mem_hl && a >= w->mem_hl_lo && a <= w->mem_hl_hi)   /* a variable, symbol or hit */
                g_string_append_printf(text,
                    " <span background=\"#%06X\" foreground=\"#%06X\">%s</span>",
                    (unsigned)INTVSESSION_ACCENT_RGB, (unsigned)INTVSESSION_ACCENT_INK_RGB, cell);
            else
                g_string_append_printf(text, " %s", cell);
        }
        /* Only label the first row of a run in the same region -- repeating
         * it on every row would just be noise (see PORTING.md's region
         * map, symbols.c's k_regions). */
        if (region && region != last_region)
            g_string_append_printf(text, "  ; %s", region);
        last_region = region;
        if ((row + 1) * MEM_WORDS_PER_ROW < n)
            g_string_append_c(text, '\n');
    }
    gtk_label_set_markup(w->mem, text->str);
    g_string_free(text, TRUE);
}

/* Case-insensitive substring test -- strcasestr is a glibc/BSD extension,
 * not portable C, so this stays a plain local helper instead. */
static int contains_ci(const char *hay, const char *needle)
{
    size_t hlen = strlen(hay), nlen = strlen(needle);
    size_t i;
    if (nlen == 0)
        return 1;
    if (nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++)
        if (g_ascii_strncasecmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data)
{
    DbgWin *w = user_data;
    gpointer addr;
    (void)box;
    addr = g_object_get_data(G_OBJECT(row), "intv-addr");
    w->mem_view_addr = (uint16_t)GPOINTER_TO_UINT(addr);
    gtk_editable_set_text(GTK_EDITABLE(w->mem_addr), "");
    invalidate(w);
}

/* Only rebuilds the list when the table's generation or the filter text has
 * changed since the last call -- a real game .sym can be a few hundred
 * entries, no reason to re-walk and re-widget it every tick. */
static void refresh_symbols(DbgWin *w)
{
    unsigned gen = intvsymtab_generation(w->symtab);
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(w->sym_filter));
    int n, i, shown = 0;
    char text[128];

    if (gen == w->sym_seen_gen && strcmp(filter, w->sym_filter_text) == 0)
        return;
    w->sym_seen_gen = gen;
    snprintf(w->sym_filter_text, sizeof(w->sym_filter_text), "%s", filter);

    gtk_list_box_remove_all(w->symlist);
    n = intvsymtab_count(w->symtab);
    for (i = 0; i < n; i++) {
        uint16_t addr;
        char name[64], note[64];
        GtkWidget *row, *label;

        if (!intvsymtab_enum(w->symtab, i, &addr, name, sizeof(name), note,
                             sizeof(note)))
            continue;
        if (filter[0] && !contains_ci(name, filter))
            continue;
        snprintf(text, sizeof(text), "%04X  %-24s%s%s", addr, name,
                note[0] ? "  ; " : "", note);
        row = gtk_list_box_row_new();
        label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "intv-addr", GUINT_TO_POINTER(addr));
        gtk_list_box_append(w->symlist, row);
        shown++;
    }
    snprintf(text, sizeof(text), "%d symbol%s%s", shown, shown == 1 ? "" : "s",
            filter[0] ? " (filtered)" : "");
    gtk_label_set_text(w->sym_count, text);
}

static void on_sym_filter_changed(GtkEditable *entry, gpointer user_data)
{ (void)entry; refresh_symbols((DbgWin *)user_data); }

/* Tabs can be moved into windows of their own, so a page's place in the
 * notebook is not its identity: each carries its original index. */
static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab-index")) - 1;
}

static int current_tab(DbgWin *w)
{
    GtkWidget *page = gtk_notebook_get_nth_page(w->notebook, gtk_notebook_get_current_page(w->notebook));
    return page ? page_tab(page) : -1;
}

/* On screen: the notebook's page, or a window of its own. */
static gboolean tab_visible(DbgWin *w, int tab)
{
    return current_tab(w) == tab || w->detached[tab] != NULL;
}

static void show_tab(DbgWin *w, int tab)
{
    if (w->detached[tab])
        gtk_window_present(w->detached[tab]);
    else
        gtk_notebook_set_current_page(w->notebook, gtk_notebook_page_num(w->notebook, w->pages[tab]));
}

static gboolean fuji_showing(DbgWin *w)
{
    return tab_visible(w, TAB_FUJINET);
}

static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static intvdebug_fuji_packet p[INTVDEBUG_FUJI_TRACE_MAX];
    static char state[2048];
    GtkTextBuffer *sb = gtk_text_view_get_buffer(w->fuji_state);
    GtkTextIter a, b;
    GString *t;
    char *cur;
    int n = intvdebug_fujinet_trace(w->dbg, p, INTVDEBUG_FUJI_TRACE_MAX), i;

    w->fuji_at = g_get_monotonic_time();
    if (!w->session || (!intvsession_fujinet_running(w->session) &&
                        !intvsession_fujinet_external(w->session))) {
        gtk_label_set_text(w->fuji_note, "FujiNet is not running.");
    } else {
        char note[160];
        g_snprintf(note, sizeof(note), "%s FujiNet at %s, cartridge %s.",
                   intvsession_fujinet_external(w->session) ? "External"
                                                            : "Built-in",
                   intvsession_boip_endpoint(w->session),
                   intvsession_fujinet_link(w->session) ? "connected"
                                                        : "not connected");
        gtk_label_set_text(w->fuji_note, note);
    }
    intvdebug_fujinet_state(w->dbg, state, (int)sizeof(state));
    gtk_text_buffer_get_bounds(sb, &a, &b);
    cur = gtk_text_buffer_get_text(sb, &a, &b, FALSE);
    if (strcmp(cur, state) != 0) /* unchanged keeps a selection */
        set_text_at(w->fuji_state, state, FALSE);
    g_free(cur);

    if (!force && n == w->fuji_count &&
        (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof(p[i].data))
            g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

/* ---- BASIC tab ------------------------------------------------------------ */

static gboolean basic_tab_active(DbgWin *w)
{
    return w->tabs && (current_tab(w) == TAB_BASIC ||
                       (w->detached[TAB_BASIC] && gtk_window_is_active(w->detached[TAB_BASIC])));
}

/* A line's gutter: '>' the machine is here, '*' a breakpoint on its code. */
static void basic_mark(DbgWin *w, int line, int pc_line, char out[3])
{
    int addr = intvsrcmap_first_addr(w->srcmap, w->bas_file, line);
    out[0] = line == pc_line ? '>' : ' ';
    out[1] = addr < 0 ? ' ' : intvdebug_breakpoint_state(w->dbg, (uint16_t)addr) == 1 ? '*'
             : intvdebug_breakpoint_state(w->dbg, (uint16_t)addr) == 2 ? 'o' : ' ';
    out[2] = '\0';
}

static void basic_set_row(DbgWin *w, int line, const char mark[3])
{
    GtkLabel *label = g_ptr_array_index(w->bas_labels, line - 1);
    const char *src = intvsrcmap_line_text(w->srcmap, w->bas_file, line);
    g_autofree char *esc = g_markup_escape_text(src ? src : "", -1);
    g_autofree char *text = g_strdup_printf(
        "%s %5d  %s", mark[1] == '*' ? BP_DOT : mark[1] == 'o' ? BP_DOT_OFF : " ", line, esc);
    GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(label));

    gtk_label_set_markup(label, text);
    if (row && mark[0] == '>')   /* the machine's line: a bar in the accent */
        gtk_widget_add_css_class(row, "intv-pcrow");
    else if (row)
        gtk_widget_remove_css_class(row, "intv-pcrow");
    memcpy(w->bas_marks->data + 3 * (line - 1), mark, 3);
}

static void basic_rebuild(DbgWin *w)
{
    int n, i;

    gtk_list_box_remove_all(w->bas_list);
    g_ptr_array_set_size(w->bas_labels, 0);
    g_array_set_size(w->bas_marks, 0);
    if (w->bas_file < 0)
        return;

    n = intvsrcmap_line_count(w->srcmap, w->bas_file);
    for (i = 1; i <= n; i++) {
        static const char blank[3] = "  ";
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);

        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        /* lines with no code (comments, labels, blanks) recede */
        if (intvsrcmap_first_addr(w->srcmap, w->bas_file, i) < 0)
            gtk_widget_add_css_class(label, "dim-label");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "intv-line", GINT_TO_POINTER(i));
        gtk_list_box_append(w->bas_list, row);
        g_ptr_array_add(w->bas_labels, label);
        g_array_append_vals(w->bas_marks, blank, 1);
        basic_set_row(w, i, blank);
    }
}

/* Brings the line the machine is on to the middle of the view. A row has no
 * height until it has been laid out, so this is retried from the tick. */
static void basic_try_scroll(DbgWin *w)
{
    int line = w->bas_scroll_line;
    GtkWidget *row;
    graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
    GtkAdjustment *adj;
    double h, page, top;

    if (line < 1 || line > (int)w->bas_labels->len) {
        w->bas_scroll_line = 0;
        return;
    }
    row = gtk_widget_get_ancestor(
        GTK_WIDGET(g_ptr_array_index(w->bas_labels, line - 1)),
        GTK_TYPE_LIST_BOX_ROW);
    h = row ? gtk_widget_get_height(row) : 0;
    adj = gtk_scrolled_window_get_vadjustment(w->bas_scroll);
    page = gtk_adjustment_get_page_size(adj);
    if (h <= 0 || page <= 0 ||
        !gtk_widget_compute_point(row, GTK_WIDGET(w->bas_list), &in, &out))
        return;
    top = gtk_adjustment_get_value(adj);
    if (out.y < top + h || out.y + 2 * h > top + page)
        gtk_adjustment_set_value(adj, out.y - page / 2 + h / 2);
    w->bas_scroll_line = 0;
}

static void basic_select_file(DbgWin *w, int file)
{
    w->bas_syncing = TRUE;
    gtk_drop_down_set_selected(w->bas_files,
                               file >= 0 ? (guint)file : GTK_INVALID_LIST_POSITION);
    w->bas_syncing = FALSE;
}

/* The map changed (an import, a Load Map): the file list, the list. */
static void basic_reload(DbgWin *w)
{
    int n = intvsrcmap_file_count(w->srcmap), i;
    g_autoptr(GPtrArray) names = g_ptr_array_new();

    for (i = 0; i < n; i++)
        g_ptr_array_add(names, (gpointer)intvsrcmap_file_name(w->srcmap, i));
    g_ptr_array_add(names, NULL);
    w->bas_syncing = TRUE;
    gtk_string_list_splice(w->bas_names, 0,
                           g_list_model_get_n_items(G_LIST_MODEL(w->bas_names)),
                           (const char *const *)names->pdata);
    w->bas_syncing = FALSE;

    if (w->bas_file >= n)
        w->bas_file = -1;
    if (w->bas_file < 0 && n > 0) {
        /* the first file with any code on it */
        w->bas_file = 0;
        for (i = 0; i < n; i++) {
            int l, lines = intvsrcmap_line_count(w->srcmap, i);
            for (l = 1; l <= lines; l++)
                if (intvsrcmap_first_addr(w->srcmap, i, l) >= 0)
                    break;
            if (l <= lines) {
                w->bas_file = i;
                break;
            }
        }
    }
    basic_select_file(w, w->bas_file);
    if (n == 0)
        gtk_label_set_text(w->bas_note,
            "Import a .bas file, or a folder of them. The assembler's map "
            "(as1600 -j game.smap) puts each line at its address.");
    basic_rebuild(w);
}

static void basic_refresh(DbgWin *w, int paused)
{
    unsigned gen = intvsrcmap_generation(w->srcmap);
    int pf = -1, pl = 0, n, i;
    intvdebug_regs r;

    if (gen != w->bas_gen) {
        w->bas_gen = gen;
        basic_reload(w);
    }
    if (paused && intvdebug_regs_get(w->dbg, &r)) {
        int f, l;
        if (intvsrcmap_lookup(w->srcmap, r.pc, &f, &l)) {
            pf = f;
            pl = l;
        }
    }
    if (pf >= 0 && pf != w->bas_file) {   /* followed into another file */
        w->bas_file = pf;
        basic_select_file(w, pf);
        basic_rebuild(w);
    }
    n = (int)w->bas_labels->len;
    for (i = 1; i <= n; i++) {
        char mark[3];
        basic_mark(w, i, pf == w->bas_file ? pl : 0, mark);
        if (memcmp(mark, w->bas_marks->data + 3 * (i - 1), 3) != 0)
            basic_set_row(w, i, mark);
    }
    if (pf >= 0 && pf == w->bas_file) {
        w->bas_scroll_line = pl;
        basic_try_scroll(w);
    }
}

static void on_bas_file_selected(GObject *dd, GParamSpec *pspec,
                                 gpointer user_data)
{
    DbgWin *w = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    (void)pspec;

    if (w->bas_syncing)
        return;
    w->bas_file = sel == GTK_INVALID_LIST_POSITION ? -1 : (int)sel;
    basic_rebuild(w);
    invalidate(w);
}

static void on_bas_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data)
{
    DbgWin *w = user_data;
    int line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "intv-line"));
    int addr = intvsrcmap_first_addr(w->srcmap, w->bas_file, line);
    (void)box;

    if (addr < 0) {   /* a message, not a refresh: see go_to_mem_addr */
        toast(w, "No code on that line");
        return;
    }
    intvdebug_breakpoint_toggle(w->dbg, (uint16_t)addr);
    invalidate(w);
}

/* ---- Breakpoints and Watches tabs ----------------------------------------- */

static void show_in_memory(DbgWin *w, uint16_t lo, uint16_t hi);

/* One line on why the machine stopped, from the core (so the width of a word
 * written is right): "Wrote $0200 = $1234 (was $1233) at $5012". */
static void stop_line(const intvdebug_snapshot *snap, char *dst, size_t n)
{
    if (!snap->paused) {
        g_strlcpy(dst, "Running", n);
        return;
    }
    g_strlcpy(dst, snap->text[0] ? snap->text : "paused", n);
    dst[0] = (char)g_ascii_toupper(dst[0]);
}

static void on_bp_enable_toggled(GtkCheckButton *cb, gpointer user_data)
{
    DbgWin *w = user_data;
    intvdebug_bp_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cb), "bp-id")),
                        gtk_check_button_get_active(cb));
    invalidate(w);
}

static void on_bp_remove(GtkButton *b, gpointer user_data)
{
    DbgWin *w = user_data;
    intvdebug_bp_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp-id")));
    invalidate(w);
}

static void bp_rebuild(DbgWin *w)
{
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    const int n = intvdebug_bp_list(w->dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
    int i;

    gtk_list_box_remove_all(w->bp_list);
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *check = gtk_check_button_new();
        GtkWidget *label = gtk_label_new(NULL);
        GtkWidget *del = gtk_button_new_with_label("Remove");
        char desc[160];
        g_autofree char *text = NULL;

        intvbp_describe(&bps[i], w->symtab, desc, sizeof(desc));
        text = g_strdup_printf("%s    hits %u", desc, bps[i].hits);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), bps[i].enabled);
        gtk_widget_set_tooltip_text(check, "Enabled");
        g_object_set_data(G_OBJECT(check), "bp-id", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(check, "toggled", G_CALLBACK(on_bp_enable_toggled), w);
        gtk_widget_add_css_class(del, "flat");
        g_object_set_data(G_OBJECT(del), "bp-id", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(del, "clicked", G_CALLBACK(on_bp_remove), w);
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), check);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), del);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void bp_refresh(DbgWin *w)
{
    const unsigned gen = intvdebug_bp_generation(w->dbg);
    intvdebug_snapshot snap;
    char text[200];

    if (gen != w->bp_gen) {
        w->bp_gen = gen;
        bp_rebuild(w);
    }
    intvdebug_stop_snapshot(w->dbg, &snap);
    stop_line(&snap, text, sizeof text);
    if (snap.paused)
        gtk_label_set_text(w->bp_stop, text);
    else
        gtk_label_set_text(w->bp_stop, "Running.");
}

static void on_bp_add(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    const guint type = gtk_drop_down_get_selected(w->bp_type);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->bp_addr));
    const char *cond = gtk_editable_get_text(GTK_EDITABLE(w->bp_cond));
    intvdebug_bp bp;
    uint16_t lo, hi;
    (void)button;

    memset(&bp, 0, sizeof(bp));
    if (type == 0 ? !intvsym_parse_addr(w->symtab, text, &lo)
                  : !intvsym_parse_range(w->symtab, text, &lo, &hi, NULL)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No such symbol, variable or address: %s", text);
        toast(w, msg);
        return;
    }
    if (type == 0)
        hi = lo;
    bp.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : INTVDEBUG_BP_ACCESS;
    bp.lo = lo;
    bp.hi = hi;
    g_strlcpy(bp.condition, cond, sizeof bp.condition);
    g_strstrip(bp.condition);
    if (intvdebug_bp_add(w->dbg, &bp) < 0) {
        intvdebug_bp all[INTVDEBUG_MAX_BREAKPOINTS];
        toast(w, intvdebug_bp_list(w->dbg, all, INTVDEBUG_MAX_BREAKPOINTS) >= INTVDEBUG_MAX_BREAKPOINTS
                     ? "The breakpoint table is full"
                     : "The condition does not parse (try: value != old, r0 == $12, [$0200] > 5)");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(w->bp_addr), "");
    invalidate(w);
}

static void on_bp_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    on_bp_add(NULL, user_data);
}

static void on_bp_clear_hits(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_bp_clear_hits(w->dbg);
    invalidate(w);
}

static void on_bp_remove_all(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_breakpoint_clear_all(w->dbg);
    invalidate(w);
}

/* ---- watches ---- */

static void on_wt_remove(GtkButton *b, gpointer user_data)
{
    DbgWin *w = user_data;
    intvwatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")));
    invalidate(w);
}

static void on_wt_memory(GtkButton *b, gpointer user_data)
{
    DbgWin *w = user_data;
    uint16_t lo, hi;
    if (intvwatch_range(w->symtab, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")), &lo, &hi)) {
        show_tab(w, TAB_MEMORY);
        show_in_memory(w, lo, hi);
    }
}

static void on_wt_break(GtkButton *b, gpointer user_data)
{
    DbgWin *w = user_data;
    intvdebug_bp bp;
    uint16_t lo, hi;
    if (!intvwatch_range(w->symtab, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "wt-idx")), &lo, &hi))
        return;
    memset(&bp, 0, sizeof(bp));
    bp.type = INTVDEBUG_BP_WRITE;
    bp.lo = lo;
    bp.hi = hi;
    g_strlcpy(bp.condition, "value != old", sizeof bp.condition);
    if (intvdebug_bp_add(w->dbg, &bp) >= 0)
        toast(w, "Breaking when it changes");
    invalidate(w);
}

static void wt_rebuild(DbgWin *w)
{
    const int n = intvwatch_count();
    int i;

    gtk_list_box_remove_all(w->wt_list);
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(NULL);
        static const struct { const char *text, *tip; GCallback cb; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break", "Break when it changes", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        char line[200];
        size_t k;

        intvwatch_format(w->dbg, w->symtab, i, line, sizeof(line));
        gtk_label_set_text(GTK_LABEL(label), line);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        gtk_box_append(GTK_BOX(row), label);
        for (k = 0; k < G_N_ELEMENTS(btns); k++) {
            GtkWidget *b = gtk_button_new_with_label(btns[k].text);
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_set_tooltip_text(b, btns[k].tip);
            g_object_set_data(G_OBJECT(b), "wt-idx", GINT_TO_POINTER(i));
            g_signal_connect(b, "clicked", btns[k].cb, w);
            gtk_box_append(GTK_BOX(row), b);
        }
        gtk_list_box_append(w->wt_list, row);
    }
}

static void on_wt_add(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)button;
    if (intvwatch_add(gtk_editable_get_text(GTK_EDITABLE(w->wt_entry))) < 0) {
        toast(w, "Type an address, symbol or BASIC variable (x, #score, a(3))");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(w->wt_entry), "");
    invalidate(w);
}

static void on_wt_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    on_wt_add(NULL, user_data);
}

/* ---- memory search ---- */

static void on_search(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    uint16_t pat[64], mask, found;
    int flags, n;
    uint16_t from;

    n = intvsearch_parse((int)gtk_drop_down_get_selected(w->srch_kind),
                         gtk_editable_get_text(GTK_EDITABLE(w->srch_entry)), pat, &mask, &flags);
    if (!n) {
        gtk_label_set_text(w->srch_note, "Type hex words (02A0 0268) or some text.");
        return;
    }
    if (!intvdebug_is_paused(w->dbg)) {
        gtk_label_set_text(w->srch_note, "Pause the machine to search its memory.");
        return;
    }
    from = w->mem_hl ? (backward ? w->mem_hl_lo : (uint16_t)(w->mem_hl_lo + 1)) : w->mem_view_addr;
    if (intvsearch_next(w->dbg, pat, n, mask, flags, from, backward, &found)) {
        g_autofree char *msg = g_strdup_printf("Found at $%04X", found);
        gtk_label_set_text(w->srch_note, msg);
        show_in_memory(w, found, (uint16_t)(found + n - 1));
    } else {
        gtk_label_set_text(w->srch_note, "Not found.");
    }
}

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    GtkWidget *fwd = g_object_get_data(G_OBJECT(entry), "next-button");
    on_search(GTK_BUTTON(fwd), w);
}

/* ---- Cart, Trace and History tabs ------------------------------------------- */

/* The cartridge and the memory map on the left; the controllers, PSG ports,
 * ECS keyboard, Intellivoice and JLP on the right. */
static void pad_text(GString *t, const intvdebug_pad *pad, const char *title)
{
    int side, line;

    if (!pad->present)
        return;
    g_string_append_printf(t, "  %s  ports $%02X $%02X%s\n", title, pad->side[0], pad->side[1],
                           pad->output[0] || pad->output[1] ? "  (set for output)" : "");
    for (side = 0; side < 2; side++) {
        gboolean held = FALSE;
        g_string_append_printf(t, "    %-5s controller:", side ? "right" : "left");
        for (line = 0; line < 18; line++)
            if ((side ? pad->right : pad->left)[line]) {
                g_string_append_printf(t, " %d", line);
                held = TRUE;
            }
        g_string_append(t, held ? "\n" : " (idle)\n");
    }
}

static void io_refresh(DbgWin *w)
{
    intvdebug_io ports;
    GString *t = g_string_new(NULL);
    int row;

    if (!intvdebug_io_get(w->dbg, &ports)) {
        g_string_append(t, "The Intellivision is off.\n");
    } else {
        g_string_append(t, "Controllers (event inputs held: keypad, disc, action buttons)\n");
        pad_text(t, &ports.pad[0], "Base ");
        pad_text(t, &ports.pad[1], "ECS  ");
        if (ports.ecs) {
            gboolean any = FALSE;
            g_string_append(t, "\nECS keyboard rows");
            for (row = 0; row < 8; row++) {
                g_string_append_printf(t, " %02X", ports.pad[1].keys[row] & 0xFF);
                any = any || ports.pad[1].keys[row] != 0;
            }
            g_string_append_printf(t, "%s\nECS ROM pages  $2xxx %d   $7xxx %d   $Exxx %d\n",
                                   any ? "" : "  (no key)", ports.ecs_page[0], ports.ecs_page[1], ports.ecs_page[2]);
        }
        if (ports.ivoice)
            g_string_append_printf(t,
                "\nIntellivoice\n  LRQ %d   ALD %d   PC %03X   mode %X   page %X\n  FIFO %u..%u   %s%s\n",
                ports.ivoice_lrq, ports.ivoice_ald, ports.ivoice_pc & 0xFFF, ports.ivoice_mode, ports.ivoice_page,
                ports.ivoice_fifo_tail, ports.ivoice_fifo_head, ports.ivoice_halted ? "halted" : "running",
                ports.ivoice_fifo_sel ? ", reading the FIFO" : "");
        if (ports.jlp)
            g_string_append_printf(t, "\nJLP\n  accelerators %s   flash %s",
                                   ports.jlp_accel ? "on" : "off",
                                   ports.jlp_flash ? "open" : "none");
        if (ports.jlp && ports.jlp_flash)
            g_string_append_printf(t, " (%u bytes)", ports.jlp_flash_bytes);
        if (ports.jlp)
            g_string_append_printf(t, "%s\n", ports.jlp_mb_hole ? "   FujiNet mailbox hole at $9C00-$9F3F" : "");
        if (ports.locutus)
            g_string_append(t, "\nLocutus mapper owns the cartridge space\n");
        if (ports.fujinet_mailbox)
            g_string_append(t, "\nFujiNet mailbox mapped at $9C00\n");
        if (!ports.ecs && !ports.ivoice && !ports.jlp && !ports.locutus && !ports.fujinet_mailbox)
            g_string_append(t, "\nNo ECS, Intellivoice, JLP or mailbox on this machine.\n");
    }
    set_text_at(w->io_text, t->str, FALSE);
    g_string_free(t, TRUE);
}

static void cart_refresh(DbgWin *w)
{
    intvdebug_cart cart;
    intvdebug_region reg[64];
    GString *t = g_string_new(NULL);
    int n, i;

    intvdebug_cart_info(w->dbg, &cart);
    g_string_append(t, "Cartridge\n");
    g_string_append_printf(t, "  File     %s\n", cart.path[0] ? cart.path : "(none)");
    if (cart.name[0]) {
        g_string_append_printf(t, "  Title    %s", cart.name);
        if (cart.year)
            g_string_append_printf(t, ", %d", cart.year);
        g_string_append_c(t, '\n');
    }
    g_string_append_printf(t, "  FujiNet  %s\n", cart.fujinet_mailbox ? "mailbox mapped at $9C00" : "no mailbox");
    g_string_append_printf(t, "  ECS      %s\n", cart.ecs ? "present" : "no");
    g_string_append_printf(t, "  JLP      %s\n", cart.jlp ? "present" : "no");
    g_string_append_printf(t, "  Voice    %s\n", cart.ivoice ? "Intellivoice" : "no");
    g_string_append_printf(t, "  Video    %s\n", cart.pal ? "PAL" : "NTSC");
    g_string_append(t, "\nMemory map\n");
    n = intvdebug_memory_map(w->dbg, reg, 64);
    if (!n)
        g_string_append(t, "  Pause the machine to read its memory map.\n");
    for (i = 0; i < n; i++)
        g_string_append_printf(t, "  %04X-%04X  %c%c  %s\n", reg[i].lo, reg[i].hi,
                               reg[i].readable ? 'r' : '-', reg[i].writable ? 'w' : '-', reg[i].name);
    set_text_at(w->cart_text, t->str, FALSE);
    g_string_free(t, TRUE);
    io_refresh(w);
}

/* Sound: the AY-3-8914s, read straight out of the chips (stopped only). */
static void sound_refresh(DbgWin *w)
{
    static const char voices[] = "ABC";
    GString *t = g_string_new(NULL);
    int unit, voice, any = 0;

    for (unit = 0; unit < 2; unit++) {
        intvdebug_psg psg;
        int reg;
        if (!intvdebug_sound_get(w->dbg, unit, &psg) || !psg.present)
            continue;
        any = 1;
        g_string_append_printf(t, "%s PSG (AY-3-8914 at $%04X)\n", unit ? "ECS" : "Base",
                               unit ? 0x00F0 : 0x01F0);
        for (voice = 0; voice < 3; voice++) {
            char bar[17];
            int lit;
            for (lit = 0; lit < 16; lit++)
                bar[lit] = lit < psg.level[voice] ? '#' : '.';
            bar[16] = '\0';
            g_string_append_printf(t, "  %c  period %03X  %8.1f Hz  %-4s%-6s  level %X [%s]%s\n", voices[voice],
                                   psg.period[voice], psg.hz[voice], psg.tone_on[voice] ? "tone" : "----",
                                   psg.noise_on[voice] ? "+noise" : "", psg.level[voice], bar,
                                   psg.envelope[voice] ? "  (envelope)" : "");
        }
        g_string_append_printf(t, "  noise period %02X   envelope period %04X   shape %X   level %X\n  registers:",
                               psg.noise_period, psg.env_period, psg.env_shape, psg.env_level);
        for (reg = 0; reg < 14; reg++)
            g_string_append_printf(t, " %02X", psg.reg[reg] & 0xFF);
        g_string_append(t, "\n\n");
    }
    if (!any)
        g_string_append(t, intvdebug_is_paused(w->dbg) ? "No sound chip is present.\n"
                                                       : "Pause the machine to read the sound chips.\n");
    set_text_at(w->sound_text, t->str, FALSE);
    g_string_free(t, TRUE);
}

#define HIST_SHOWN 400   /* lines on the page */

static void hist_refresh(DbgWin *w, int idx, gboolean force)
{
    static intvdebug_hist buf[4000];
    const gboolean live = idx == 0;
    const gboolean paused = intvdebug_is_paused(w->dbg);
    const gboolean on = intvdebug_history_enabled(w->dbg);
    const uint64_t count = intvdebug_history_count(w->dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = 0, n, i, shown = 0, first;
    g_autofree char *note = NULL;
    GString *t;
    int hit[HIST_SHOWN];

    w->h_sync = TRUE;   /* the two Record buttons follow the engine */
    gtk_toggle_button_set_active(w->h_rec[0], on);
    gtk_toggle_button_set_active(w->h_rec[1], on);
    w->h_sync = FALSE;

    if (!force && count == w->h_seen[idx] && (live || paused))
        return;
    w->h_seen[idx] = count;
    if (!live && !paused) {
        gtk_label_set_text(w->h_note[idx], "Pause the machine to read its history.");
        return;
    }
    switch (gtk_drop_down_get_selected(w->h_filter[idx])) {
    case 1: filter = INTVDEBUG_HIST_BRANCHES; break;
    case 2:
        if (intvsym_parse_range(w->symtab, gtk_editable_get_text(GTK_EDITABLE(w->h_range[idx])), &lo, &hi, NULL))
            filter = INTVDEBUG_HIST_RANGE;
        break;
    }
    n = intvdebug_history_copy(w->dbg, buf, 4000);
    first = 0;
    for (i = n - 1; i >= 0 && shown < HIST_SHOWN; i--)   /* the newest matches, scanning back */
        if (intvdebug_history_match(&buf[i], filter, lo, hi))
            hit[shown++] = i;
    (void)first;
    t = g_string_new(NULL);
    if (!on && n == 0)
        g_string_append(t, "History is off. Press Record, then let the machine run.");
    for (i = shown - 1; i >= 0; i--) {   /* oldest first */
        char line[256];
        intvdebug_history_format(&buf[hit[i]], line, sizeof(line));
        g_string_append(t, line);
        g_string_append_c(t, '\n');
    }
    set_text_at(w->h_text[idx], t->str, TRUE);
    g_string_free(t, TRUE);
    note = g_strdup_printf("%s%" G_GUINT64_FORMAT " instructions recorded, last %d shown",
                           on ? "Recording. " : "", count, shown);
    gtk_label_set_text(w->h_note[idx], note);
}

static void on_hist_rec(GtkToggleButton *b, gpointer u)
{
    DbgWin *w = u;
    if (w->h_sync)
        return;
    intvdebug_history_enable(w->dbg, gtk_toggle_button_get_active(b));
    invalidate(w);
}

static void on_hist_clear(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_history_clear(w->dbg);
    w->h_seen[0] = w->h_seen[1] = (uint64_t)-1;
    invalidate(w);
}

static void on_hist_changed(GObject *o, GParamSpec *p, gpointer u)
{
    DbgWin *w = u;
    (void)o; (void)p;
    w->h_seen[0] = w->h_seen[1] = (uint64_t)-1;
    invalidate(w);
}

/* ---- moving tabs into windows of their own ---------------------------------- */

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user_data);

static void dock_tab(DbgWin *w, int tab, gboolean destroy)
{
    GtkWindow *win = w->detached[tab];
    GtkWidget *page = w->pages[tab], *vbox;
    int pos = 0, t;

    if (!win)
        return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    w->detached[tab] = NULL;
    for (t = 0; t < tab; t++)   /* back where it was in the order */
        if (!w->detached[t])
            pos++;
    gtk_notebook_insert_page(w->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(w->notebook, pos);
    if (destroy)
        gtk_window_destroy(win);
    invalidate(w);
}

static void on_dock_button(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    dock_tab(w, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer u)
{
    DbgWin *w = u;
    dock_tab(w, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;   /* let it close */
}

static void pop_out_tab(DbgWin *w, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    g_autofree char *title = NULL;

    if (tab < 0 || w->detached[tab] || gtk_notebook_get_n_pages(w->notebook) <= 1)
        return;   /* one tab always stays */
    page = w->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(w->notebook, gtk_notebook_page_num(w->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    title = g_strdup_printf("Intellivision Debugger \xe2\x80\x93 %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_default_size(win, 760, 520);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), w);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), w);
    w->detached[tab] = win;
    gtk_window_present(win);
    invalidate(w);
}

static void on_pop_out(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    pop_out_tab(w, current_tab(w));
}

static void refresh_all(DbgWin *w)
{
    intvdebug_snapshot snap;
    int paused;
    char status[200];

    intvdebug_stop_snapshot(w->dbg, &snap);
    w->seen_gen = intvdebug_generation(w->dbg);
    paused = snap.paused;
    gtk_button_set_label(w->run_pause, paused ? "_Run" : "_Pause");
    /* the action on offer wears the accent: Run while stopped */
    if (paused)
        gtk_widget_add_css_class(GTK_WIDGET(w->run_pause), "intv-accent");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(w->run_pause), "intv-accent");
    stop_line(&snap, status, sizeof status);
    gtk_label_set_text(w->status, status);

    refresh_regs(w);
    if (paused) {
        refresh_disasm(w);
        refresh_mem(w);
    } else {
        gtk_list_box_remove_all(w->disasm);
        gtk_label_set_text(w->mem, "");
    }
    basic_refresh(w, paused);
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refresh_symbols's own comment). */
    refresh_symbols(w);
    bp_refresh(w);
    wt_rebuild(w);
    cart_refresh(w);
    sound_refresh(w);
    hist_refresh(w, 0, FALSE);
    hist_refresh(w, 1, FALSE);
    if (tab_visible(w, TAB_VIDEO))
        dbg_video_refresh(w->pages[TAB_VIDEO], FALSE);
    if (fuji_showing(w))
        refresh_fujinet(w, FALSE);
    w->was_paused = paused;
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    DbgWin *w = user_data;
    intvdebug_snapshot snap;
    (void)widget;
    (void)clock;

    if (!w->dbg)
        return G_SOURCE_CONTINUE;
    if (w->kick > 0)
        w->kick--;

    intvdebug_stop_snapshot(w->dbg, &snap);
    if (snap.serial != w->seen_serial || snap.paused != w->was_paused ||
        intvdebug_generation(w->dbg) != w->seen_gen) {
        w->seen_serial = snap.serial;
        refresh_all(w);
    } else if (fuji_showing(w) &&
               g_get_monotonic_time() - w->fuji_at >= FUJI_REFRESH_US) {
        /* the one view that moves while the machine runs */
        refresh_fujinet(w, FALSE);
    }
    if (w->bas_scroll_line)
        basic_try_scroll(w);
    if (tab_visible(w, TAB_VIDEO))   /* a running machine publishes a frame every 1/60 s */
        dbg_video_refresh(w->pages[TAB_VIDEO], FALSE);
    if (tab_visible(w, TAB_TRACE) && g_get_monotonic_time() - w->h_at >= 400000) {
        w->h_at = g_get_monotonic_time();   /* the live view moves while the machine runs */
        hist_refresh(w, 0, FALSE);
    }
    /* Paused with nothing pending: stop (this removes the callback). */
    if (snap.paused && w->kick <= 0) {
        w->tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_window_state(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(user_data);
}

/* ---- actions --------------------------------------------------------------- */

static void on_run_pause(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)button;
    if (intvdebug_is_paused(w->dbg))
        intvdebug_resume(w->dbg);
    else
        intvdebug_pause(w->dbg);
}

/* On the BASIC tab the three steppers go by BASIC line. */
static void on_step(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    if (basic_tab_active(w))
        intvdebug_step_line(w->dbg);
    else
        intvdebug_step(w->dbg);
}
static void on_step_over(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    if (basic_tab_active(w))
        intvdebug_step_line_over(w->dbg);
    else
        intvdebug_step_over(w->dbg);
}
static void on_step_out(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    if (basic_tab_active(w))
        intvdebug_step_line_out(w->dbg);
    else
        intvdebug_step_out(w->dbg);
}
static void on_clear_bp(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_breakpoint_clear_all(w->dbg);
    invalidate(w);
}

/* Messages go to a toast, not the status label: touching seen_serial would
 * make the next tick overwrite them with the plain Paused/Running text. */
/* Shows a range in the Memory tab, highlighted, on a row boundary. */
static void show_in_memory(DbgWin *w, uint16_t lo, uint16_t hi)
{
    w->mem_view_addr = (uint16_t)(lo & 0xFFF8);
    w->mem_hl = TRUE;
    w->mem_hl_lo = lo;
    w->mem_hl_hi = hi;
    invalidate(w);
    if (intvdebug_is_paused(w->dbg))
        refresh_mem(w);
}

static void go_to_mem_addr(DbgWin *w)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->mem_addr));
    uint16_t lo, hi;
    if (intvsym_parse_range(w->symtab, text, &lo, &hi, NULL)) {
        show_in_memory(w, lo, hi);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "No such symbol, variable or address: %s", text);
        toast(w, msg);
    }
}

static void on_mem_addr_activate(GtkEntry *entry, gpointer user_data)
{ (void)entry; go_to_mem_addr(user_data); }

/* "address value...": words written from the address on, through the bus. A
 * value wider than the word at its address (8 bits in the scratchpad, GROM/GRAM
 * and ECS RAM) is refused rather than cut. */
static void on_poke(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(w->poke_entry)), " \t:=,", -1);
    uint16_t words[POKE_MAX], addr = 0;
    gboolean have_addr = FALSE, valid = TRUE;
    int n = 0, i;
    (void)entry;

    for (i = 0; parts[i] && valid; i++) {
        long val;
        if (!*parts[i])
            continue;
        if (!have_addr) {
            valid = intvsym_parse_addr(w->symtab, parts[i], &addr);
            have_addr = TRUE;
        } else if (n < POKE_MAX && parse_num(parts[i], &val) && val >= 0 && val <= 0xFFFF) {
            if (val > (intvdebug_word_width(w->dbg, (uint16_t)(addr + n)) == 8 ? 0xFF : 0xFFFF)) {
                toast(w, "That value is wider than the word at that address (8 bits there)");
                g_strfreev(parts);
                return;
            }
            words[n++] = (uint16_t)val;
        } else {
            valid = FALSE;
        }
    }
    g_strfreev(parts);
    if (!valid || !have_addr || n == 0) {
        toast(w, "Poke takes an address and hex words, e.g. 0200 1234 5678");
        return;
    }
    if (!intvdebug_is_paused(w->dbg)) {
        toast(w, "Stop the machine to poke its memory");
        return;
    }
    intvdebug_write(w->dbg, addr, words, n);
    gtk_editable_set_text(GTK_EDITABLE(w->poke_entry), "");
    show_in_memory(w, addr, (uint16_t)(addr + n - 1));
}

static void on_break_addr_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    uint16_t addr;
    if (intvsym_parse_addr(w->symtab, text, &addr)) {
        intvdebug_breakpoint_toggle(w->dbg, addr);
        invalidate(w); /* redraw the marker immediately */
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "No such symbol or address: %s", text);
        toast(w, msg);
    }
}

/* ---- the Console tab ---------------------------------------------------------- */

static void on_console(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];

    if (!cmd || !*cmd)
        return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 || strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->console_out, line, TRUE);
    out[0] = '\0';
    intvdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    invalidate(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and the
 * labels; several matches are listed instead. Up and Down walk the history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType st, gpointer user_data)
{
    DbgWin *w = user_data;
    char comps[4096];
    const char *text, *word;
    int n;
    (void)c; (void)code; (void)st;

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab)
        return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the last word is replaced */
    n = intvdebug_completions(w->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (n == 1) {
        char merged[600];
        char *eol = strchr(comps, '\n');
        if (eol) *eol = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
    } else if (n > 1) {
        /* the common start is filled in; the candidates are listed */
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int i;
        for (i = 1; cand[i] && *cand[i]; i++) {
            size_t k = 0;
            while (k < common && cand[i][k] == cand[0][k]) k++;
            common = k;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text, (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
        }
        g_strfreev(cand);
        append_text(w->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void weak_free(gpointer p)
{
    g_weak_ref_clear(p);
    g_free(p);
}

static void load_symbols_done(GObject *source, GAsyncResult *result,
                              gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(ref);
    DbgWin *w = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = NULL;
    int n;

    weak_free(ref);
    /* The debugger may have been closed while the picker was up. */
    if (!w || !file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    n = intvsymtab_load(w->symtab, path);
    if (n < 0) {
        toast(w, "Could not open symbol file");
        return;
    }
    /* Remember the directory (settings key "sym_dir") so re-opening the
     * picker -- expected to happen often, since loading is always a manual
     * step taken after CONFIG has already booted a cart, never automatic --
     * starts back where the user just was. */
    {
        char dir[1024];
        if (intvsym_dirname(path, dir, sizeof(dir)) > 0 && w->session) {
            intvsession_set_str(w->session, "sym_dir", dir);
            intvsession_settings_flush(w->session);
        }
    }
    invalidate(w);
}

static void on_load_symbols(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    const char *last_dir = w->session
                              ? intvsession_get_str(w->session, "sym_dir", "")
                              : "";
    (void)button;

    gtk_file_filter_set_name(filter, "as1600 symbol files");
    gtk_file_filter_add_suffix(filter, "sym");
    gtk_file_filter_add_pattern(filter, "*");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Load Symbols");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    if (last_dir[0]) {
        g_autoptr(GFile) folder = g_file_new_for_path(last_dir);
        gtk_file_dialog_set_initial_folder(dialog, folder);
    }
    {
        GWeakRef *ref = g_new0(GWeakRef, 1);
        g_weak_ref_init(ref, w->win);
        gtk_file_dialog_open(dialog, w->win, NULL, load_symbols_done, ref);
    }
    g_object_unref(filters);
    g_object_unref(dialog);
}

/* ---- BASIC tab: Import File / Import Folder / Load Map ------------------- */

enum { PICK_FILE, PICK_FOLDER, PICK_MAP };

typedef struct {
    GWeakRef win;
    int kind;
} PickReq;

static void basic_after_import(DbgWin *w, const char *path, int kind)
{
    intvsrcmap_import_result r;
    g_autofree char *msg = NULL;
    int n = intvsrcmap_import(w->srcmap, path, w->symtab, &r);

    if (n < 0) {
        gtk_label_set_text(w->bas_note,
            kind == PICK_FOLDER ? "No .bas files in that folder."
                                : "Could not read that file.");
        return;
    }
    if (r.mapped > 0) {
        g_autofree char *base = g_path_get_basename(r.map_path);
        msg = g_strdup_printf("%d file%s, %d address range%s from %s%s%s",
                              r.files, r.files == 1 ? "" : "s", r.mapped,
                              r.mapped == 1 ? "" : "s", base,
                              r.symbols ? ", with its symbols" : "", ".");
    } else {
        msg = g_strdup_printf(
            "%d file%s loaded, but no .smap or .lst was found beside them. "
            "Build with as1600 -j game.smap, then Load Map.",
            r.files, r.files == 1 ? "" : "s");
    }
    gtk_label_set_text(w->bas_note, msg);
    w->bas_file = -1;   /* basic_reload picks the first file with code */
    invalidate(w);
}

static void pick_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PickReq *req = user_data;
    g_autoptr(GObject) wobj = g_weak_ref_get(&req->win);
    DbgWin *w = wobj ? g_object_get_data(wobj, "dbgwin") : NULL;
    int kind = req->kind;
    g_autoptr(GFile) file =
        kind == PICK_FOLDER
            ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL)
            : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = NULL;

    g_weak_ref_clear(&req->win);
    g_free(req);
    if (!w || !file)    /* closed, or the picker was cancelled */
        return;
    path = g_file_get_path(file);
    if (!path)
        return;

    {   /* start the next picker where this one ended */
        char dir[1024];
        if (kind == PICK_FOLDER)
            g_strlcpy(dir, path, sizeof(dir));
        else
            intvsym_dirname(path, dir, sizeof(dir));
        if (dir[0] && w->session) {
            intvsession_set_str(w->session, "bas_dir", dir);
            intvsession_settings_flush(w->session);
        }
    }
    if (kind == PICK_MAP) {
        int n = intvsrcmap_load_map(w->srcmap, path);
        if (n < 0) {
            gtk_label_set_text(w->bas_note, "Could not read that map.");
        } else {
            g_autofree char *base = g_path_get_basename(path);
            int syms = intvsrcmap_load_sibling_sym(path, w->symtab);
            g_autofree char *msg = g_strdup_printf(
                "%d address range%s from %s%s.", n, n == 1 ? "" : "s", base,
                syms ? ", with its symbols" : "");
            gtk_label_set_text(w->bas_note, msg);
        }
        invalidate(w);
    } else {
        basic_after_import(w, path, kind);
    }
}

static void on_basic_pick(GtkButton *button, gpointer user_data)
{
    DbgWin *w = user_data;
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "kind"));
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    const char *last_dir =
        w->session ? intvsession_get_str(w->session, "bas_dir", "") : "";
    PickReq *req = g_new0(PickReq, 1);

    if (kind == PICK_MAP) {
        gtk_file_filter_set_name(filter, "as1600 source maps and listings");
        gtk_file_filter_add_suffix(filter, "smap");
        gtk_file_filter_add_suffix(filter, "lst");
    } else {
        gtk_file_filter_set_name(filter, "IntyBASIC source");
        gtk_file_filter_add_suffix(filter, "bas");
    }
    gtk_file_filter_add_pattern(filter, "*");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, kind == PICK_FOLDER ? "Import Folder"
                                      : kind == PICK_MAP  ? "Load Map"
                                                          : "Import File");
    if (kind != PICK_FOLDER)
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    if (last_dir[0]) {
        g_autoptr(GFile) folder = g_file_new_for_path(last_dir);
        gtk_file_dialog_set_initial_folder(dialog, folder);
    }
    req->kind = kind;
    g_weak_ref_init(&req->win, w->win);
    if (kind == PICK_FOLDER)
        gtk_file_dialog_select_folder(dialog, w->win, NULL, pick_done, req);
    else
        gtk_file_dialog_open(dialog, w->win, NULL, pick_done, req);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void on_clear_symbols(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvsymtab_clear_user(w->symtab);
    invalidate(w);
}

static void on_fuji_clear(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

/* Help's contexts (intvhelp.h), one per tab */
static const char *const k_tab_contexts[] = {
    "debugger/console", "debugger/disassembly", "debugger/basic", "debugger/memory",
    "debugger/breakpoints", "debugger/watches", "debugger/cart", "debugger/trace",
    "debugger/history", "debugger/video", "debugger/sound", "debugger/symbols",
    "debugger/fujinet",
};

static gboolean on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num,
                               gpointer user_data)
{
    DbgWin *w = user_data;
    const int tab = page_tab(page);
    (void)nb;
    (void)num;
    if (tab >= 0 && tab < (int)G_N_ELEMENTS(k_tab_contexts))
        intv_help_set_context(w->win, k_tab_contexts[tab]);
    if (page == w->fuji_page)
        refresh_fujinet(w, FALSE);
    kick(w);   /* a page may have changed under another one (a Video break, the console): catch up */
    if (tab == TAB_VIDEO)
        dbg_video_refresh(w->pages[TAB_VIDEO], TRUE);
    if (tab == TAB_CONSOLE && w->console_in)
        gtk_widget_grab_focus(GTK_WIDGET(w->console_in));
    return FALSE;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)c;
    (void)keycode;
    kick(w);
    switch (keyval) {
    case GDK_KEY_F1:
        intv_help_for(w->win);
        return TRUE;
    case GDK_KEY_F5:
        on_run_pause(NULL, w);
        return TRUE;
    case GDK_KEY_F7:
        on_step(NULL, w);
        return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK)
            on_step_out(NULL, w);
        else
            on_step_over(NULL, w);
        return TRUE;
    default:
        return FALSE;
    }
}

/* The window's memory outlives its widgets: freed once the window is gone,
 * when no child can still signal into it. */
static void dbgwin_free(gpointer data, GObject *where)
{
    DbgWin *w = data;
    (void)where;
    if (w->bas_labels)
        g_ptr_array_free(w->bas_labels, TRUE);
    if (w->bas_marks)
        g_array_free(w->bas_marks, TRUE);
    if (w->history)
        g_ptr_array_free(w->history, TRUE);
    g_free(data);
}

static gboolean on_closed(GtkWindow *window, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)window;

    /* Teardown re-selects notebook pages; nothing may call back into w. */
    if (w->tabs)
        g_signal_handlers_disconnect_by_data(w->tabs, w);
    {
        int t;   /* a peeled-off tab's window goes with the debugger */
        for (t = 0; t < TAB_COUNT; t++)
            if (w->detached[t]) {
                GtkWindow *dw = w->detached[t];
                g_signal_handlers_disconnect_by_data(dw, w);
                w->detached[t] = NULL;
                gtk_window_destroy(dw);
            }
    }

    if (w->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w->win), w->tick_id);
        w->tick_id = 0;
    }
    g_signal_handler_disconnect(w->win, w->map_id);
    g_signal_handler_disconnect(w->win, w->unmap_id);
    g_signal_handler_disconnect(w->win, w->susp_id);
    /* Releasing costs the machine nothing further (see intvdebug.h) and
     * leaving it engaged+paused would look like the emulator had hung. */
    intvdebug_set_engaged(w->dbg, 0);
    /* w->symtab is intvsymtab_shared() -- process-wide, not owned by this
     * window -- so it is deliberately NOT destroyed here; see this file's
     * header comment. */
    return FALSE; /* go on and close */
}

/* ---- construction ----------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tooltip,
                              GCallback cb, DbgWin *w)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, w);
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(kick), w);
    return b;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *mono_view(GtkTextView **out)
{
    GtkWidget *view = gtk_text_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *note = gtk_label_new("");
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *log_scroll, *state_scroll;

    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_hexpand(note, TRUE);
    w->fuji_note = GTK_LABEL(note);
    gtk_widget_set_tooltip_text(clear, "Forget the packets so far");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), note);
    gtk_box_append(GTK_BOX(row), clear);

    log_scroll = mono_view(&w->fuji_text);
    gtk_text_view_set_wrap_mode(w->fuji_text, GTK_WRAP_NONE);
    make_log(w->fuji_text);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), log_scroll);
    state_scroll = mono_view(&w->fuji_state);

    gtk_paned_set_start_child(GTK_PANED(paned),
                              labeled("FujiBus packets (SLIP-decoded)", left));
    gtk_paned_set_end_child(GTK_PANED(paned),
                            labeled("FujiNet's mailbox", state_scroll));
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(log_scroll, 560, -1);
    gtk_widget_set_size_request(state_scroll, 320, -1);
    gtk_widget_set_margin_top(paned, 6);
    gtk_widget_set_margin_bottom(paned, 6);
    gtk_widget_set_margin_start(paned, 6);
    gtk_widget_set_margin_end(paned, 6);
    w->fuji_count = -1;
    return paned;
}

static GtkWidget *build_basic(DbgWin *w)
{
    static const struct { const char *label, *tip; int kind; } buttons[] = {
        { "Import _File…", "Load a .bas file and everything it INCLUDEs", PICK_FILE },
        { "Import F_older…", "Load every .bas file under a folder", PICK_FOLDER },
        { "Load _Map…", "Load an as1600 -j source map, or a -l listing", PICK_MAP },
    };
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *note = gtk_label_new("");
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(buttons); i++) {
        GtkWidget *b = tool_button(buttons[i].label, buttons[i].tip,
                                   G_CALLBACK(on_basic_pick), w);
        g_object_set_data(G_OBJECT(b), "kind", GINT_TO_POINTER(buttons[i].kind));
        gtk_box_append(GTK_BOX(bar), b);
    }
    w->bas_names = gtk_string_list_new(NULL);
    w->bas_files = GTK_DROP_DOWN(
        gtk_drop_down_new(G_LIST_MODEL(w->bas_names), NULL));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->bas_files),
                                "The source file shown");
    g_signal_connect(w->bas_files, "notify::selected",
                     G_CALLBACK(on_bas_file_selected), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->bas_files));

    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    w->bas_note = GTK_LABEL(note);

    w->bas_labels = g_ptr_array_new();
    w->bas_marks = g_array_new(FALSE, TRUE, 3);
    w->bas_file = -1;
    w->bas_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bas_list, GTK_SELECTION_NONE);
    g_signal_connect(w->bas_list, "row-activated",
                     G_CALLBACK(on_bas_row_activated), w);
    w->bas_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(w->bas_scroll, GTK_WIDGET(w->bas_list));
    gtk_widget_set_vexpand(GTK_WIDGET(w->bas_scroll), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(w->bas_scroll), TRUE);

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), note);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bas_scroll));
    return box;
}

static GtkWidget *build_search_bar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = tool_button("Find _Next", "Next match after the highlight", G_CALLBACK(on_search), w);
    GtkWidget *prev = tool_button("Find _Previous", "Previous match before the highlight", G_CALLBACK(on_search), w);
    static const char *const kinds[] = { "Hex words", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    w->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->srch_kind), "Hex words (02A0 0268) or text, one character per word");
    w->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(w->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(w->srch_entry), "next-button", next);
    g_signal_connect(w->srch_entry, "activate", G_CALLBACK(on_search_entry_activate), w);
    w->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->srch_note), "dim-label");
    gtk_widget_set_margin_top(bar, 0);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search:"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->srch_note));
    return bar;
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const char *const types[] = { "Execute", "Read", "Write", "Access", NULL };

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->bp_type),
        "Execute stops before an instruction; Read, Write and Access stop after the "
        "instruction that touched the words (data accesses only, not code fetches)");
    w->bp_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_addr, "address, lo-hi, symbol, x, #score, a(3)");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_addr), TRUE);
    g_signal_connect(w->bp_addr, "activate", G_CALLBACK(on_bp_entry_activate), w);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (optional): value != old, r0 == $12, [$0200] > 5");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->bp_cond),
        "An expression: r0-r7 pc sp, flags s z o c i d, [addr], labels, frame, cycles; "
        "in an access `value`, `addr` and `old` (the word before a write)");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_entry_activate), w);
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->bp_addr));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Add the breakpoint", G_CALLBACK(on_bp_add), w));

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->bp_stop, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_stop), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_stop), TRUE);
    gtk_box_append(GTK_BOX(foot), GTK_WIDGET(w->bp_stop));
    gtk_box_append(GTK_BOX(foot), tool_button("Clear _Hits", "Zero the hit counts", G_CALLBACK(on_bp_clear_hits), w));
    gtk_box_append(GTK_BOX(foot), tool_button("_Remove All", "Remove every breakpoint", G_CALLBACK(on_bp_remove_all), w));

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), foot);
    return box;
}

static GtkWidget *build_watches(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    w->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->wt_entry, "address, symbol, x, #score, a(3), lo-hi");
    gtk_widget_set_hexpand(GTK_WIDGET(w->wt_entry), TRUE);
    g_signal_connect(w->wt_entry, "activate", G_CALLBACK(on_wt_entry_activate), w);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch:"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(w->wt_entry));
    gtk_box_append(GTK_BOX(form), tool_button("_Add", "Watch it", G_CALLBACK(on_wt_add), w));

    w->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 6);
    gtk_widget_set_margin_bottom(w, 6);
    gtk_widget_set_margin_start(w, 6);
    gtk_widget_set_margin_end(w, 6);
    return w;
}

/* The cartridge and its memory map, beside the controllers and the other I/O. */
static GtkWidget *build_cart(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = mono_view(&w->cart_text);
    GtkWidget *right = mono_view(&w->io_text);

    gtk_text_view_set_wrap_mode(w->cart_text, GTK_WRAP_NONE);
    gtk_text_view_set_wrap_mode(w->io_text, GTK_WRAP_NONE);
    gtk_box_append(GTK_BOX(box), labeled("Cartridge and memory map", left));
    gtk_box_append(GTK_BOX(box), labeled("Controllers and I/O", right));
    return padded(box);
}

static GtkWidget *build_sound(DbgWin *w)
{
    return padded(mono_view(&w->sound_text));
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->console_out);
    GtkEventController *keys;

    make_log(w->console_out);
    w->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->console_in,
        "Command (help, step, break, bpw, bpa, print, mem, poke, watch, find, video, disasm, cart, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->console_in), TRUE);
    g_signal_connect(w->console_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->console_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->console_in));
    set_text_at(w->console_out,
                "Intellivision debugger console (CP-1610 and STIC). Type 'help' for every command.\n"
                "Numbers: $5000, 0x5000, 5000h, 20480. Registers r0-r7 pc sp s z o c i d; [addr] is the word there.\n",
                TRUE);
    return padded(box);
}

/* The register strip under every tab: R0-R7 (R6 is the stack, growing up;
 * R7 the PC) and the flags S Z O C I are entries (Enter sets one while the
 * machine is stopped); D (SDBD pending) and the interrupt state are shown. */
static GtkWidget *build_regs(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    int i;

    for (i = 0; i < REG_COUNT; i++) {
        char name[16];
        g_snprintf(name, sizeof name, "R%d%s", i, i == 6 ? " (SP)" : i == 7 ? " (PC)" : "");
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 8);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), 5);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), 5);
        gtk_widget_add_css_class(GTK_WIDGET(w->reg[i]), "monospace");
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(INTVDEBUG_REG_R0 + i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(row), labeled(name, GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < FLAG_COUNT; i++) {
        w->flag[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->flag[i], 4);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->flag[i]), 2);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->flag[i]), 2);
        gtk_widget_add_css_class(GTK_WIDGET(w->flag[i]), "monospace");
        g_object_set_data(G_OBJECT(w->flag[i]), "reg", GINT_TO_POINTER(k_flags[i].reg));
        g_signal_connect(w->flag[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(row2), labeled(k_flags[i].name, GTK_WIDGET(w->flag[i])));
    }
    w->dflag = GTK_LABEL(gtk_label_new("D -"));
    gtk_widget_add_css_class(GTK_WIDGET(w->dflag), "monospace");
    gtk_widget_set_valign(GTK_WIDGET(w->dflag), GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->dflag),
        "Double-byte data: SDBD is pending for the next instruction (read only)");
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(w->dflag));
    w->intr = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->intr), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(w->intr), "monospace");
    gtk_label_set_xalign(w->intr, 0);
    gtk_label_set_ellipsize(w->intr, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->intr), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(w->intr), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(w->intr));
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), row2);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    return box;
}

static GtkWidget *build_hist(DbgWin *w, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&w->h_text[idx]);
    GtkWidget *clear = gtk_button_new_with_mnemonic("_Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    w->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_rec[idx]), "intv-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs a little slower)");
    g_signal_connect(w->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), w);
    w->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(w->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), w);
    w->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->h_range[idx], "range: lo-hi or a symbol");
    g_signal_connect(w->h_range[idx], "activate", G_CALLBACK(on_hist_changed), w);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), w);
    w->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->h_note[idx]), TRUE);
    gtk_label_set_xalign(w->h_note[idx], 1.0f);
    gtk_text_view_set_wrap_mode(w->h_text[idx], GTK_WRAP_NONE);
    w->h_seen[idx] = (uint64_t)-1;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->h_note[idx]));
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return box;
}

/* A tab from an environment variable (INTV_DEBUGGER_TAB, INTV_POPOUT): a
 * number, or a tab's name or the start of one (any case), or an older name
 * (cpu, stic, cart, io). For tests and captures. */
static int tab_from_env(const char *var)
{
    const char *tab = g_getenv(var);
    char *end;
    long n;
    size_t i;

    if (!tab || !*tab)
        return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < (long)G_N_ELEMENTS(k_tab_names) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    for (i = 0; i < G_N_ELEMENTS(k_tab_aliases); i++)
        if (g_ascii_strcasecmp(tab, k_tab_aliases[i].name) == 0)
            return k_tab_aliases[i].tab;
    return -1;
}

void intv_debugger_show(GtkWindow *parent, intvsession *session)
{
    static GtkWindow *singleton;
    DbgWin *w;
    GtkWidget *box, *scroller, *header, *bar, *mem_bar, *mem_scroller, *notebook,
              *overlay;
    GtkEventController *keys;
    int tab;

    if (singleton) {
        gtk_window_present(singleton);
        return;
    }

    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = intvdebug_get();
    w->symtab = intvsymtab_shared();
    w->srcmap = intvsrcmap_shared();
    w->mem_view_addr = 0x0000;
    w->sym_seen_gen = (unsigned)-1;   /* the first refresh always fills the list */

    w->win = GTK_WINDOW(adw_window_new());
    g_object_set_data(G_OBJECT(w->win), "dbgwin", w);
    g_object_weak_ref(G_OBJECT(w->win), dbgwin_free, w);
    singleton = w->win;
    g_object_add_weak_pointer(G_OBJECT(w->win), (gpointer *)&singleton);
    gtk_window_set_title(w->win, "Intellivision Debugger");
    gtk_window_set_default_size(w->win, 1080, 780);
    gtk_window_set_transient_for(w->win, parent);

    header = adw_header_bar_new();
    w->status = GTK_LABEL(gtk_label_new("Running"));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    GTK_WIDGET(w->status));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), intv_help_button(NULL));

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    w->run_pause = GTK_BUTTON(tool_button("_Pause", "Run or pause (F5)",
                                          G_CALLBACK(on_run_pause), w));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_pause));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Step", "One instruction, or on the BASIC tab one BASIC line (F7)",
                               G_CALLBACK(on_step), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Step _Over", "Over a subroutine call or GOSUB (F8)",
                               G_CALLBACK(on_step_over), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Step Ou_t",
                               "Until this routine returns (Shift+F8)",
                               G_CALLBACK(on_step_out), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Clear Breakpoints",
                               "Remove every breakpoint",
                               G_CALLBACK(on_clear_bp), w));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Break at:"));
    w->break_addr = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->break_addr),
                                "Symbol name or hex address, then Enter");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->break_addr), 12);
    g_signal_connect(w->break_addr, "activate",
                     G_CALLBACK(on_break_addr_activate), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->break_addr));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Load _Symbols…",
                               "Load an as1600 -s symbol file",
                               G_CALLBACK(on_load_symbols), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Clear S_ymbols",
                               "Drop loaded symbols, keeping the built-ins",
                               G_CALLBACK(on_clear_symbols), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Pop _Out",
                               "Move the tab that shows into a window of its own; Dock puts it back",
                               G_CALLBACK(on_pop_out), w));

    w->disasm = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->disasm, GTK_SELECTION_NONE);
    g_signal_connect(w->disasm, "row-activated",
                     G_CALLBACK(on_row_activated), w);
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                  GTK_WIDGET(w->disasm));
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    gtk_widget_set_margin_start(scroller, 6);
    gtk_widget_set_margin_end(scroller, 6);
    gtk_widget_set_margin_top(scroller, 6);
    gtk_widget_set_margin_bottom(scroller, 6);
    /* the wheel and the window size the list, not its 32 rows */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    mem_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(mem_bar, 6);
    gtk_widget_set_margin_bottom(mem_bar, 6);
    gtk_widget_set_margin_start(mem_bar, 6);
    gtk_widget_set_margin_end(mem_bar, 6);
    gtk_box_append(GTK_BOX(mem_bar), gtk_label_new("Address:"));
    w->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(w->mem_addr), "0000");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_addr), 6);
    g_signal_connect(w->mem_addr, "activate",
                     G_CALLBACK(on_mem_addr_activate), w);
    gtk_box_append(GTK_BOX(mem_bar), GTK_WIDGET(w->mem_addr));
    gtk_box_append(GTK_BOX(mem_bar), gtk_label_new("Poke:"));
    w->poke_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->poke_entry, "address word...  e.g. 0200 1234 5678");
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->poke_entry),
        "Writes the words from the address on, through the bus (stopped). "
        "The scratchpad, GROM, GRAM and ECS RAM hold 8 bits.");
    gtk_widget_set_hexpand(GTK_WIDGET(w->poke_entry), TRUE);
    g_signal_connect(w->poke_entry, "activate", G_CALLBACK(on_poke), w);
    gtk_box_append(GTK_BOX(mem_bar), GTK_WIDGET(w->poke_entry));
    gtk_box_append(GTK_BOX(mem_bar), tool_button("Po_ke", "Write the words", G_CALLBACK(on_poke), w));

    w->mem = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->mem, 0.0f);
    gtk_label_set_yalign(w->mem, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(w->mem), "monospace");
    mem_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mem_scroller),
                                  GTK_WIDGET(w->mem));
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(mem_scroller), 200);
    gtk_widget_set_margin_start(mem_scroller, 6);
    gtk_widget_set_margin_end(mem_scroller, 6);
    gtk_widget_set_margin_bottom(mem_scroller, 6);
    gtk_widget_set_hexpand(mem_scroller, TRUE);
    gtk_widget_set_vexpand(mem_scroller, TRUE);

    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    w->pages[TAB_CONSOLE] = build_console(w);
    w->pages[TAB_DISASM] = scroller;
    w->pages[TAB_BASIC] = build_basic(w);
    {
        GtkWidget *mem_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_append(GTK_BOX(mem_page), mem_bar);
        gtk_box_append(GTK_BOX(mem_page), build_search_bar(w));
        gtk_box_append(GTK_BOX(mem_page), mem_scroller);
        w->pages[TAB_MEMORY] = mem_page;
    }
    w->pages[TAB_BREAKS] = build_breaks(w);
    w->pages[TAB_WATCHES] = build_watches(w);
    w->pages[TAB_CART] = build_cart(w);
    w->pages[TAB_TRACE] = build_hist(w, 0);
    w->pages[TAB_HISTORY] = build_hist(w, 1);
    w->pages[TAB_VIDEO] = dbg_video_new(w->dbg);
    w->pages[TAB_SOUND] = build_sound(w);
    {
        GtkWidget *sym_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *sym_filter_bar =
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *sym_scroll = gtk_scrolled_window_new();

        gtk_widget_set_margin_top(sym_box, 6);
        gtk_widget_set_margin_bottom(sym_box, 6);
        gtk_widget_set_margin_start(sym_box, 6);
        gtk_widget_set_margin_end(sym_box, 6);

        gtk_box_append(GTK_BOX(sym_filter_bar), gtk_label_new("Filter:"));
        w->sym_filter = GTK_ENTRY(gtk_entry_new());
        g_signal_connect(w->sym_filter, "changed",
                         G_CALLBACK(on_sym_filter_changed), w);
        gtk_box_append(GTK_BOX(sym_filter_bar), GTK_WIDGET(w->sym_filter));
        w->sym_count = GTK_LABEL(gtk_label_new(""));
        gtk_box_append(GTK_BOX(sym_filter_bar), GTK_WIDGET(w->sym_count));

        w->symlist = GTK_LIST_BOX(gtk_list_box_new());
        gtk_list_box_set_selection_mode(w->symlist, GTK_SELECTION_NONE);
        g_signal_connect(w->symlist, "row-activated",
                         G_CALLBACK(on_sym_row_activated), w);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sym_scroll),
                                      GTK_WIDGET(w->symlist));
        gtk_widget_set_vexpand(sym_scroll, TRUE);

        gtk_box_append(GTK_BOX(sym_box), sym_filter_bar);
        gtk_box_append(GTK_BOX(sym_box), sym_scroll);
        w->pages[TAB_SYMBOLS] = sym_box;
    }
    w->fuji_page = build_fujinet(w);
    w->pages[TAB_FUJINET] = w->fuji_page;
    w->tabs = GTK_NOTEBOOK(notebook);
    for (tab = 0; tab < TAB_COUNT; tab++) {
        /* each page keeps its identity when it is moved around */
        g_object_set_data(G_OBJECT(w->pages[tab]), "tab-index", GINT_TO_POINTER(tab + 1));
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), w->pages[tab], gtk_label_new(k_tab_names[tab]));
    }
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
        intv_help_set_context(w->win, k_tab_contexts[cur > 0 && cur < (int)G_N_ELEMENTS(k_tab_contexts) ? cur : 0]);
    }
    tab = tab_from_env("INTV_DEBUGGER_TAB");
    if (tab >= 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), tab);
        intv_help_set_context(w->win, k_tab_contexts[tab]);
    }

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), build_regs(w));
    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, box);
    adw_window_set_content(ADW_WINDOW(w->win), overlay);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_closed), w);

    /* For captures and tests, like INTV_DEBUGGER_TAB: INTV_BASIC_IMPORT=<.bas
     * file or folder> imports it as Import File / Import Folder would, and
     * INTV_BASIC_BREAK=<name.bas>:<line> sets a breakpoint on that line. */
    {
        const char *imp = g_getenv("INTV_BASIC_IMPORT");
        const char *brk = g_getenv("INTV_BASIC_BREAK");
        if (imp && *imp)
            basic_after_import(w, imp, PICK_FILE);
        if (brk && strchr(brk, ':')) {
            g_autofree char *name = g_strndup(brk, (gsize)(strchr(brk, ':') - brk));
            int line = atoi(strchr(brk, ':') + 1), f;
            for (f = 0; f < intvsrcmap_file_count(w->srcmap); f++) {
                int addr = intvsrcmap_first_addr(w->srcmap, f, line);
                if (strcmp(intvsrcmap_file_name(w->srcmap, f), name) == 0 &&
                    addr >= 0 && !intvdebug_breakpoint_is_set(w->dbg, (uint16_t)addr))
                    intvdebug_breakpoint_toggle(w->dbg, (uint16_t)addr);
            }
        }
    }

    /* More captures: INTV_WATCH="x" adds a watch, INTV_WATCH_BREAK="x" a
     * write-when-changed breakpoint on it, INTV_MEM_GOTO="x" shows it in Memory. */
    {
        const char *wt = g_getenv("INTV_WATCH"), *wb = g_getenv("INTV_WATCH_BREAK"),
                   *mg = g_getenv("INTV_MEM_GOTO");
        uint16_t lo, hi;
        if (wt && *wt)
            intvwatch_add(wt);
        if (wb && *wb && intvsym_parse_range(w->symtab, wb, &lo, &hi, NULL)) {
            intvdebug_bp bp;
            memset(&bp, 0, sizeof(bp));
            bp.type = INTVDEBUG_BP_WRITE;
            bp.lo = lo;
            bp.hi = hi;
            g_strlcpy(bp.condition, "value != old", sizeof bp.condition);
            intvdebug_bp_add(w->dbg, &bp);
        }
        if (mg && *mg && intvsym_parse_range(w->symtab, mg, &lo, &hi, NULL))
            show_in_memory(w, lo, hi);
    }
    if (g_getenv("INTV_HISTORY"))   /* record from the start, for captures */
        intvdebug_history_enable(w->dbg, 1);

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(w->dbg, 1);
    intvdebug_pause(w->dbg);

    w->map_id = g_signal_connect_swapped(w->win, "map", G_CALLBACK(kick), w);
    w->unmap_id = g_signal_connect_swapped(w->win, "unmap",
                                           G_CALLBACK(update_tick), w);
    w->susp_id = g_signal_connect(w->win, "notify::suspended",
                                  G_CALLBACK(on_window_state), w);
    gtk_window_present(w->win);
    if (current_tab(w) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(w->console_in));
    tab = tab_from_env("INTV_POPOUT");   /* a tab in a window of its own, for captures */
    if (tab >= 0)
        pop_out_tab(w, tab);
}
