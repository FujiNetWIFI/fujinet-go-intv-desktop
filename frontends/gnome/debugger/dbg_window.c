/*
 * The CP-1610/STIC debugger window (GTK4/libadwaita), modelled on the CoCo
 * port's own dbg_window.c (poll-based stop tracking: cocodebug_stop_serial
 * there, intvdebug_stop_serial here -- see that file's header for why this
 * is a poll and not a callback) with a STIC tab laid out like the MSX
 * port's VDP tab (nametable/pattern/sprite/palette pictures plus decoded
 * register text), retargeted to the STIC's own BACKTAB/MOB/GRAM-GROM-card/
 * palette views from intvstic.h.
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
#include "intvstic.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_WORDS_PER_ROW 8
#define MEM_ROWS 16
#define CARD_ROWS_PER_PAGE 8 /* 128 cards/page at CARDS_PER_ROW=16 */

typedef struct {
    GtkWindow *win;
    intvsession *session;
    intvdebug *dbg;
    intvsymtab *symtab;

    GtkLabel *status;
    GtkButton *run_pause;
    GtkListBox *disasm;
    GtkLabel *regs;

    GtkEntry *mem_addr;
    GtkLabel *mem;
    uint16_t mem_view_addr;
    GtkEntry *break_addr;

    GtkListBox *symlist;
    GtkEntry *sym_filter;
    GtkLabel *sym_count;
    unsigned sym_seen_gen;
    char sym_filter_text[64];

    GtkPicture *pic_backtab;
    GtkPicture *pic_mob[INTVSTIC_MOB_COUNT];
    GtkLabel *mob_info;
    GtkPicture *pic_cards;
    GtkLabel *cards_range;
    GtkPicture *pic_pal;
    GtkTextView *stic_state;
    int card_first;
    int card_total;

    guint tick_id;
    uint64_t seen_serial;
    int was_paused;
} DbgWin;

/* All intvstic_render_* outputs are RGBA8888, alpha always 0xFF except
 * intvstic_render_mob (which uses alpha for transparency) -- both are the
 * same memory layout GDK_MEMORY_R8G8B8A8 expects either way. */
static GdkTexture *texture_from_rgba(const uint8_t *rgba, int w, int h)
{
    GBytes *bytes = g_bytes_new(rgba, (gsize)w * h * 4);
    GdkTexture *tex =
        gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, bytes, (gsize)w * 4);
    g_bytes_unref(bytes);
    return tex;
}

static void set_picture(GtkPicture *pic, const uint8_t *rgba, int w, int h)
{
    GdkTexture *tex = texture_from_rgba(rgba, w, h);
    gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
    g_object_unref(tex);
}

/* ---- refreshers ----------------------------------------------------------- */

static void refresh_regs(DbgWin *w)
{
    intvdebug_regs r;
    g_autofree char *text = NULL;

    if (!intvdebug_regs_get(w->dbg, &r)) {
        gtk_label_set_text(w->regs, "");
        return;
    }
    text = g_strdup_printf(
        "R0 %04X   R1 %04X   R2 %04X   R3 %04X\n"
        "R4 %04X   R5 %04X   R6(SP) %04X   R7(PC) %04X\n\n"
        "S%d C%d O%d Z%d I%d   D%d",
        r.r[0], r.r[1], r.r[2], r.r[3], r.r[4], r.r[5], r.r[6], r.pc, r.S,
        r.C, r.O, r.Z, r.I, r.D);
    gtk_label_set_text(w->regs, text);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                             gpointer user_data)
{
    DbgWin *w = user_data;
    gpointer addr;
    (void)box;

    addr = g_object_get_data(G_OBJECT(row), "intv-addr");
    intvdebug_breakpoint_toggle(w->dbg, (uint16_t)GPOINTER_TO_UINT(addr));
    w->seen_serial = 0; /* redraw the marker immediately */
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
        g_autofree char *text = NULL;

        intvsym_annotate_line(w->symtab, l, symsuffix, sizeof(symsuffix));
        text = g_strdup_printf(
            "%c%c %04X  %-24s%s", (l->addr == r.pc) ? '>' : ' ',
            intvdebug_breakpoint_is_set(w->dbg, l->addr) ? '*' : ' ',
            l->addr, l->text, symsuffix);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "intv-addr",
                          GUINT_TO_POINTER((guint)l->addr));
        gtk_list_box_append(w->disasm, row);
    }
}

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
            if (idx < n)
                g_string_append_printf(text, " %04X", words[idx]);
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
    gtk_label_set_text(w->mem, text->str);
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
    w->seen_serial = 0;
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

static void on_sym_filter_changed(GtkEditable *e, gpointer user_data)
{ (void)e; refresh_symbols((DbgWin *)user_data); }

static void refresh_stic(DbgWin *w)
{
    static intvstic_snapshot snap;
    static uint8_t backtab_rgba[INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4];
    static uint8_t mob_rgba[16 * 16 * 4];
    static uint8_t cards_rgba[INTVSTIC_CARDS_PER_ROW * 8 *
                              CARD_ROWS_PER_PAGE * 8 * 4];
    static uint8_t pal_rgba[INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4];
    char text[2048];
    GString *mobtext;
    int i, total, rows;

    intvstic_snapshot_get(w->dbg, &snap);

    intvstic_render_backtab(&snap, backtab_rgba);
    set_picture(w->pic_backtab, backtab_rgba, INTVSTIC_BACKTAB_W,
               INTVSTIC_BACKTAB_H);

    mobtext = g_string_new("## X    Y  CARD CLR SZx SZy VIS PRI GRAM COLL\n");
    for (i = 0; i < INTVSTIC_MOB_COUNT; i++) {
        intvstic_mob mob;
        intvstic_mob_get(&snap, i, &mob);
        intvstic_render_mob(&snap, i, mob_rgba);
        set_picture(w->pic_mob[i], mob_rgba, 16, 16);
        g_string_append_printf(
            mobtext, "%d  %3d  %3d  %3d  %2d  %dx  %dx  %d   %d   %d    %03X\n",
            i, mob.x, mob.y, mob.card, mob.color, mob.x_size, mob.y_stretch,
            mob.visible, mob.priority, mob.gram, mob.collision);
    }
    gtk_label_set_text(w->mob_info, mobtext->str);
    g_string_free(mobtext, TRUE);

    total = intvstic_card_count(&snap);
    w->card_total = total;
    rows = CARD_ROWS_PER_PAGE;
    if (w->card_first >= total)
        w->card_first = 0;
    intvstic_render_cards(&snap, w->card_first, rows, cards_rgba);
    set_picture(w->pic_cards, cards_rgba, INTVSTIC_CARDS_PER_ROW * 8,
               rows * 8);
    {
        int page = INTVSTIC_CARDS_PER_ROW * rows;
        int last_shown = (w->card_first + page < total ? w->card_first + page
                                                        : total) -
                        1;
        char range_text[64];
        snprintf(range_text, sizeof(range_text), "cards %d\xe2\x80\x93%d of %d",
                w->card_first, last_shown, total);
        gtk_label_set_text(w->cards_range, range_text);
    }

    intvstic_render_palette(w->dbg, pal_rgba);
    set_picture(w->pic_pal, pal_rgba, INTVSTIC_PAL_CELL * 16,
               INTVSTIC_PAL_CELL);

    intvstic_format_state(&snap, text, (int)sizeof(text));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(w->stic_state), text,
                             -1);
}

static void refresh_all(DbgWin *w)
{
    int paused = intvdebug_is_paused(w->dbg);
    gtk_button_set_label(w->run_pause, paused ? "_Run" : "_Pause");
    gtk_label_set_text(w->status, paused ? "Paused" : "Running");

    if (paused) {
        refresh_regs(w);
        refresh_disasm(w);
        refresh_mem(w);
        refresh_stic(w);
    } else {
        gtk_label_set_text(w->regs, "");
        gtk_list_box_remove_all(w->disasm);
        gtk_label_set_text(w->mem, "");
    }
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refresh_symbols's own comment). */
    refresh_symbols(w);
    w->was_paused = paused;
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    DbgWin *w = user_data;
    uint64_t serial;
    (void)widget;
    (void)clock;

    if (!w->dbg)
        return G_SOURCE_CONTINUE;

    serial = intvdebug_stop_serial(w->dbg);
    if (serial != w->seen_serial ||
        intvdebug_is_paused(w->dbg) != w->was_paused) {
        w->seen_serial = serial;
        refresh_all(w);
    }
    return G_SOURCE_CONTINUE;
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

static void on_step(GtkButton *b, gpointer u)
{ (void)b; intvdebug_step(((DbgWin *)u)->dbg); }
static void on_step_over(GtkButton *b, gpointer u)
{ (void)b; intvdebug_step_over(((DbgWin *)u)->dbg); }
static void on_step_out(GtkButton *b, gpointer u)
{ (void)b; intvdebug_step_out(((DbgWin *)u)->dbg); }
static void on_clear_bp(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvdebug_breakpoint_clear_all(w->dbg);
    w->seen_serial = 0;
}

/* Errors here go straight to the status label, same as on_load_symbols'
 * "Could not open symbol file" -- and, like that one, must NOT touch
 * seen_serial: doing so would force the very next tick_cb to overwrite the
 * message with the plain Paused/Running text before the user reads it. */
static void go_to_mem_addr(DbgWin *w)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->mem_addr));
    uint16_t addr;
    if (intvsym_parse_addr(w->symtab, text, &addr)) {
        w->mem_view_addr = addr;
        w->seen_serial = 0;
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "No such symbol or address: %s", text);
        gtk_label_set_text(w->status, msg);
    }
}

static void on_mem_addr_activate(GtkEntry *entry, gpointer user_data)
{ (void)entry; go_to_mem_addr(user_data); }

static void on_break_addr_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    uint16_t addr;
    if (intvsym_parse_addr(w->symtab, text, &addr)) {
        intvdebug_breakpoint_toggle(w->dbg, addr);
        w->seen_serial = 0; /* redraw the marker immediately */
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "No such symbol or address: %s", text);
        gtk_label_set_text(w->status, msg);
    }
}

static void on_cards_page(GtkButton *btn, gpointer user_data)
{
    DbgWin *w = user_data;
    int delta =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "delta"));
    int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
    int last_page = w->card_total > 0
                       ? ((w->card_total - 1) / page) * page
                       : 0;
    int next = w->card_first + delta * page;
    if (next < 0)
        next = 0;
    if (next > last_page)
        next = last_page;
    w->card_first = next;
    w->seen_serial = 0;
    if (intvdebug_is_paused(w->dbg))
        refresh_stic(w);
}

static void load_symbols_done(GObject *source, GAsyncResult *result,
                              gpointer user_data)
{
    DbgWin *w = user_data;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = NULL;
    int n;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    n = intvsymtab_load(w->symtab, path);
    if (n < 0) {
        gtk_label_set_text(w->status, "Could not open symbol file");
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
    w->seen_serial = 0;
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
    gtk_file_dialog_open(dialog, w->win, NULL, load_symbols_done, w);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void on_clear_symbols(GtkButton *b, gpointer u)
{
    DbgWin *w = u;
    (void)b;
    intvsymtab_clear_user(w->symtab);
    w->seen_serial = 0;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)c;
    (void)keycode;
    switch (keyval) {
    case GDK_KEY_F5:
        on_run_pause(NULL, w);
        return TRUE;
    case GDK_KEY_F7:
        intvdebug_step(w->dbg);
        return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK)
            intvdebug_step_out(w->dbg);
        else
            intvdebug_step_over(w->dbg);
        return TRUE;
    default:
        return FALSE;
    }
}

static void on_closed(GtkWindow *window, gpointer user_data)
{
    DbgWin *w = user_data;
    (void)window;

    if (w->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w->win), w->tick_id);
        w->tick_id = 0;
    }
    /* Releasing costs the machine nothing further (see intvdebug.h) and
     * leaving it engaged+paused would look like the emulator had hung. */
    intvdebug_set_engaged(w->dbg, 0);
    /* w->symtab is intvsymtab_shared() -- process-wide, not owned by this
     * window -- so it is deliberately NOT destroyed here; see this file's
     * header comment. */
    g_free(w);
}

/* ---- construction ----------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tooltip,
                              GCallback cb, DbgWin *w)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, w);
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
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

void intv_debugger_show(GtkWindow *parent, intvsession *session)
{
    static GtkWindow *singleton;
    DbgWin *w;
    GtkWidget *box, *panes, *scroller, *regbox, *header, *bar, *mem_bar,
              *mem_scroller, *notebook, *stic_grid, *stic_scroll, *mob_grid,
              *mob_box, *cards_bar;
    GtkEventController *keys;
    int i;

    if (singleton) {
        gtk_window_present(singleton);
        return;
    }

    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = intvdebug_get();
    w->symtab = intvsymtab_shared();
    w->mem_view_addr = 0x0000;

    w->win = GTK_WINDOW(adw_window_new());
    singleton = w->win;
    g_object_add_weak_pointer(G_OBJECT(w->win), (gpointer *)&singleton);
    gtk_window_set_title(w->win, "Intellivision Debugger");
    gtk_window_set_default_size(w->win, 1080, 780);
    gtk_window_set_transient_for(w->win, parent);

    header = adw_header_bar_new();
    w->status = GTK_LABEL(gtk_label_new("Running"));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    GTK_WIDGET(w->status));

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    w->run_pause = GTK_BUTTON(tool_button("_Pause", "Run or pause (F5)",
                                          G_CALLBACK(on_run_pause), w));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_pause));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Step", "One instruction (F7)",
                               G_CALLBACK(on_step), w));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Step _Over", "Over a subroutine call (F8)",
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

    w->disasm = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->disasm, GTK_SELECTION_NONE);
    g_signal_connect(w->disasm, "row-activated",
                     G_CALLBACK(on_row_activated), w);
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                  GTK_WIDGET(w->disasm));
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    w->regs = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->regs, 0.0f);
    gtk_label_set_yalign(w->regs, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(w->regs), "monospace");
    regbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(regbox, 12);
    gtk_widget_set_margin_end(regbox, 12);
    gtk_widget_set_margin_top(regbox, 6);
    gtk_box_append(GTK_BOX(regbox), labeled("Registers", GTK_WIDGET(w->regs)));

    panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), scroller);
    gtk_box_append(GTK_BOX(panes), regbox);

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

    /* ---- STIC page ---- */
    stic_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(stic_grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(stic_grid), 8);
    gtk_widget_set_margin_start(stic_grid, 8);
    gtk_widget_set_margin_top(stic_grid, 8);

    w->pic_backtab = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_backtab),
                                INTVSTIC_BACKTAB_W * 2, INTVSTIC_BACKTAB_H * 2);
    gtk_grid_attach(GTK_GRID(stic_grid),
                    labeled("BACKTAB", GTK_WIDGET(w->pic_backtab)), 0, 0, 1,
                    1);

    mob_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(mob_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(mob_grid), 4);
    for (i = 0; i < INTVSTIC_MOB_COUNT; i++) {
        w->pic_mob[i] = GTK_PICTURE(gtk_picture_new());
        gtk_widget_set_size_request(GTK_WIDGET(w->pic_mob[i]), 32, 32);
        gtk_grid_attach(GTK_GRID(mob_grid), GTK_WIDGET(w->pic_mob[i]),
                        i % 4, i / 4, 1, 1);
    }
    w->mob_info = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->mob_info, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(w->mob_info), "monospace");
    mob_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(mob_box), mob_grid);
    gtk_box_append(GTK_BOX(mob_box), GTK_WIDGET(w->mob_info));
    gtk_grid_attach(GTK_GRID(stic_grid), labeled("MOBs", mob_box), 1, 0, 1,
                    1);

    w->pic_cards = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(
        GTK_WIDGET(w->pic_cards), INTVSTIC_CARDS_PER_ROW * 8 * 2,
        CARD_ROWS_PER_PAGE * 8 * 2);
    cards_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    {
        GtkWidget *prev = gtk_button_new_with_label("\xe2\x97\x80");
        GtkWidget *next = gtk_button_new_with_label("\xe2\x96\xb6");
        g_object_set_data(G_OBJECT(prev), "delta", GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(next), "delta", GINT_TO_POINTER(1));
        g_signal_connect(prev, "clicked", G_CALLBACK(on_cards_page), w);
        g_signal_connect(next, "clicked", G_CALLBACK(on_cards_page), w);
        gtk_box_append(GTK_BOX(cards_bar), prev);
        gtk_box_append(GTK_BOX(cards_bar), next);
        w->cards_range = GTK_LABEL(gtk_label_new(""));
        gtk_box_append(GTK_BOX(cards_bar), GTK_WIDGET(w->cards_range));
    }
    {
        GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(cbox), GTK_WIDGET(w->pic_cards));
        gtk_box_append(GTK_BOX(cbox), cards_bar);
        gtk_grid_attach(GTK_GRID(stic_grid),
                        labeled("GRAM/GROM cards", cbox), 0, 1, 1, 1);
    }

    w->pic_pal = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_pal),
                                INTVSTIC_PAL_CELL * 16, INTVSTIC_PAL_CELL);
    gtk_grid_attach(GTK_GRID(stic_grid),
                    labeled("Palette", GTK_WIDGET(w->pic_pal)), 1, 1, 1, 1);

    stic_scroll = mono_view(&w->stic_state);
    gtk_widget_set_size_request(stic_scroll, 480, 220);
    gtk_grid_attach(GTK_GRID(stic_grid),
                    labeled("Registers & mode", stic_scroll), 0, 2, 2, 1);

    notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), panes,
                             gtk_label_new("CPU"));
    {
        GtkWidget *outer_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_scroll),
                                      stic_grid);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), outer_scroll,
                                 gtk_label_new("STIC"));
    }
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
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sym_box,
                                 gtk_label_new("Symbols"));
    }

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), mem_bar);
    gtk_box_append(GTK_BOX(box), mem_scroller);
    adw_window_set_content(ADW_WINDOW(w->win), box);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_closed), w);

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(w->dbg, 1);
    intvdebug_pause(w->dbg);

    w->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(w->win), tick_cb, w, NULL);
    gtk_window_present(w->win);
}
