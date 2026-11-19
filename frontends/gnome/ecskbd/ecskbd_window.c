/*
 * IntvEcsKeyboardWindow -- the ECS keyboard drawn with cairo from the core's
 * layout (intvsession_ecs_layout, hit-tested with intvsession_ecs_hit), from
 * photos of the real one (DESIGN-intv.md section 4): a beige case with an
 * inset key plate, a dark label plate, ventilation ridges, a red pinstripe
 * along the front edge, black keys with their shifted legends above, a white
 * RTN, two SHIFT caps and a long SPACE bar.
 *
 * A click presses a key (intvsession_ecs_key_set) and holds it for as long as
 * the mouse button is down, like a finger: the machine samples the keyboard
 * matrix once per frame. SHIFT and CTL latch instead -- a mouse can't hold two
 * keys to chord a shifted letter -- so a click flips them and they stay lit
 * until clicked again or intvsession_ecs_keys_clear. A cap lights while
 * intvsession_ecs_key_held says its key is down, whoever pressed it: this
 * window, or the physical keyboard in ECS keyboard mode. Independent of the
 * "keyboard_mode" setting: the on-screen keys work whether or not the
 * physical keyboard is routed to the ECS, so opening this window never
 * silently changes it.
 *
 * FOCUS: typing while this window has the focus always goes to the ECS matrix.
 * Losing focus must not leave a key stuck down, so it releases them all.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ecskbd_window.h"

#include "../controllers/controllers_window.h"
#include "../help/help_window.h"
#include "../keysym_map.h"

#include <math.h>
#include <string.h>

#define SCALE   24.0        /* pixels per layout unit */
#define MARGIN  10.0

#define HELD_RED   (((INTVSESSION_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_GREEN (((INTVSESSION_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_BLUE  ((INTVSESSION_HELD_RGB & 255) / 255.0)

static GtkWindow *g_win;
static GtkWindow *g_parent;
static intvsession *g_session;
static GtkWidget *g_notice, *g_mode, *g_area;

static const intvsession_ecs_cap *g_layout;
static int g_count;
static float g_lw, g_lh;
static double g_offset;
static int g_down_idx = -1;         /* the cap the mouse holds, or -1 */
static guint64 g_last_signature;

/* ---- drawing helpers -------------------------------------------------------- */

static void set_rgb(cairo_t *ctx, unsigned hex)
{
    cairo_set_source_rgb(ctx, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

static void set_rgba(cairo_t *ctx, unsigned hex, double alpha)
{
    cairo_set_source_rgba(ctx, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                          (hex & 255) / 255.0, alpha);
}

static void round_rect(cairo_t *ctx, double left, double top, double wide, double high, double radius)
{
    cairo_new_sub_path(ctx);
    cairo_arc(ctx, left + wide - radius, top + radius, radius, -G_PI / 2, 0);
    cairo_arc(ctx, left + wide - radius, top + high - radius, radius, 0, G_PI / 2);
    cairo_arc(ctx, left + radius, top + high - radius, radius, G_PI / 2, G_PI);
    cairo_arc(ctx, left + radius, top + radius, radius, G_PI, 3 * G_PI / 2);
    cairo_close_path(ctx);
}

/* Text centred on a point, shrunk to fit its width. */
static void text_fit(cairo_t *ctx, const char *text, double centre_x, double centre_y,
                     double max_wide, double size, cairo_font_slant_t slant,
                     cairo_font_weight_t weight)
{
    cairo_text_extents_t extents;
    cairo_select_font_face(ctx, "Sans", slant, weight);
    cairo_set_font_size(ctx, size);
    cairo_text_extents(ctx, text, &extents);
    if (extents.width > max_wide && extents.width > 0) {
        cairo_set_font_size(ctx, size * max_wide / extents.width);
        cairo_text_extents(ctx, text, &extents);
    }
    cairo_move_to(ctx, centre_x - extents.width / 2 - extents.x_bearing,
                  centre_y - extents.height / 2 - extents.y_bearing);
    cairo_show_text(ctx, text);
}

static double to_x(double units) { return g_offset + units * SCALE; }
static double to_y(double units) { return MARGIN + units * SCALE; }

/* ---- the case ------------------------------------------------------------ */

/* The label plate: white INTELLIVISION over COMPUTER KEYBOARD (`shifted` holds
 * the two lines, split at '|'). */
static void draw_label(cairo_t *ctx, const intvsession_ecs_cap *cap)
{
    const double left = to_x(cap->x), top = to_y(cap->y);
    const double wide = cap->w * SCALE, high = cap->h * SCALE;
    char *copy = g_strdup(cap->shifted ? cap->shifted : "");
    char *bar = strchr(copy, '|');

    set_rgb(ctx, INTVSESSION_ECS_LABEL_RGB);
    round_rect(ctx, left, top, wide, high, 0.25 * SCALE);
    cairo_fill(ctx);
    set_rgb(ctx, INTVSESSION_LEGEND_RGB);
    if (bar) {
        *bar = '\0';
        text_fit(ctx, copy, left + wide * 0.25, top + high * 0.30, wide * 0.40, 0.62 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        text_fit(ctx, bar + 1, left + wide * 0.25, top + high * 0.62, wide * 0.40, 0.48 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
    g_free(copy);
}

static void draw_vents(cairo_t *ctx, const intvsession_ecs_cap *cap)
{
    const double left = to_x(cap->x), top = to_y(cap->y);
    const double wide = cap->w * SCALE, high = cap->h * SCALE;
    int ridge;
    set_rgb(ctx, INTVSESSION_ECS_RIDGE_RGB);
    cairo_set_line_cap(ctx, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(ctx, 0.2 * SCALE);
    for (ridge = 0; ridge < 5; ridge++) {
        const double ridge_y = top + (ridge + 0.5) * high / 5;
        cairo_move_to(ctx, left + 0.2 * SCALE, ridge_y);
        cairo_line_to(ctx, left + wide - 0.2 * SCALE, ridge_y);
    }
    cairo_stroke(ctx);
    cairo_set_line_cap(ctx, CAIRO_LINE_CAP_BUTT);
}

static void draw_body(cairo_t *ctx, const intvsession_ecs_cap *cap)
{
    const double left = to_x(cap->x), top = to_y(cap->y);
    const double wide = cap->w * SCALE, high = cap->h * SCALE;
    const char *label = cap->label;

    if (!strcmp(label, "Case")) {
        set_rgba(ctx, 0x000000, 0.28);
        round_rect(ctx, left + 1, top + 3, wide, high, 0.8 * SCALE);
        cairo_fill(ctx);
        set_rgb(ctx, INTVSESSION_ECS_CASE_RGB);
        round_rect(ctx, left, top, wide, high, 0.8 * SCALE);
        cairo_fill_preserve(ctx);
        set_rgba(ctx, 0x000000, 0.35);
        cairo_set_line_width(ctx, 1);
        cairo_stroke(ctx);
    } else if (!strcmp(label, "Plate")) {
        set_rgb(ctx, INTVSESSION_ECS_PLATE_RGB);
        round_rect(ctx, left, top, wide, high, 0.5 * SCALE);
        cairo_fill_preserve(ctx);
        set_rgba(ctx, 0x000000, 0.18);
        cairo_set_line_width(ctx, 1);
        cairo_stroke(ctx);
    } else if (!strcmp(label, "Label")) {
        draw_label(ctx, cap);
    } else if (!strcmp(label, "Mattel")) {
        set_rgb(ctx, INTVSESSION_ECS_PINSTRIPE_RGB);     /* the Mattel red */
        cairo_save(ctx);
        cairo_translate(ctx, left + wide / 2, top + high / 2);
        cairo_rotate(ctx, -0.05);
        text_fit(ctx, cap->shifted ? cap->shifted : "", 0, 0, wide, 0.7 * SCALE,
                 CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
        cairo_restore(ctx);
    } else if (!strcmp(label, "Vents")) {
        draw_vents(ctx, cap);
    } else if (!strcmp(label, "Pinstripe")) {
        set_rgb(ctx, INTVSESSION_ECS_PINSTRIPE_RGB);
        cairo_rectangle(ctx, left, top, wide, high < 2 ? 2 : high);
        cairo_fill(ctx);
    } else if (!strcmp(label, "Cable")) {
        set_rgb(ctx, INTVSESSION_CABLE_RGB);
        round_rect(ctx, left, top - 0.1 * SCALE, wide, high + 0.1 * SCALE, 0.2 * SCALE);
        cairo_fill(ctx);
    }
}

/* An arrow drawn as a path, so it never depends on the font having one:
 * direction 0 up, 1 down, 2 left, 3 right, 4 the RTN's return arrow. */
static void draw_arrow(cairo_t *ctx, int direction, double centre_x, double centre_y, double size)
{
    static const double turn[4] = { -G_PI / 2, G_PI / 2, G_PI, 0 };
    const double reach = size / 2;

    cairo_save(ctx);
    cairo_translate(ctx, centre_x, centre_y);
    cairo_set_line_width(ctx, size * 0.16);
    cairo_set_line_cap(ctx, CAIRO_LINE_CAP_BUTT);
    if (direction == 4) {
        /* down from the right, along to the left, head pointing left */
        cairo_move_to(ctx, reach * 0.9, -reach * 0.7);
        cairo_line_to(ctx, reach * 0.9, reach * 0.3);
        cairo_line_to(ctx, -reach * 0.35, reach * 0.3);
        cairo_stroke(ctx);
        cairo_move_to(ctx, -reach, reach * 0.3);
        cairo_line_to(ctx, -reach * 0.3, -reach * 0.15);
        cairo_line_to(ctx, -reach * 0.3, reach * 0.75);
    } else {
        cairo_rotate(ctx, turn[direction]);
        cairo_move_to(ctx, -reach, 0);
        cairo_line_to(ctx, reach * 0.4, 0);
        cairo_stroke(ctx);
        cairo_move_to(ctx, reach, 0);
        cairo_line_to(ctx, reach * 0.1, -reach * 0.55);
        cairo_line_to(ctx, reach * 0.1, reach * 0.55);
    }
    cairo_close_path(ctx);
    cairo_fill(ctx);
    cairo_restore(ctx);
}

/* The label's arrow direction when it is one of the four arrow glyphs the
 * layout carries (U+2190..U+2193), else -1. */
static int arrow_of(const char *label)
{
    if ((unsigned char)label[0] != 0xE2 || (unsigned char)label[1] != 0x86)
        return -1;
    switch ((unsigned char)label[2]) {
    case 0x91: return 0;
    case 0x93: return 1;
    case 0x90: return 2;
    case 0x92: return 3;
    default:   return -1;
    }
}

/* ---- a key cap -------------------------------------------------------------- */

/* Black cap, white legends, the shifted one small above the main one. Held (or
 * latched) it is the solid held colour with black legends; the white RTN gets
 * the ring and a 40% fill instead. */
static void draw_cap(cairo_t *ctx, const intvsession_ecs_cap *cap, gboolean lit)
{
    const double left = to_x(cap->x), top = to_y(cap->y);
    const double wide = cap->w * SCALE, high = cap->h * SCALE, radius = 0.3 * SCALE;
    const gboolean white = (cap->flags & INTVSESSION_ECSCAP_WHITE) != 0;
    const gboolean word = strlen(cap->label) > 1 && (unsigned char)cap->label[0] < 0x80;
    const unsigned ink = white ? INTVSESSION_ECS_RTN_INK_RGB : INTVSESSION_LEGEND_RGB;

    set_rgba(ctx, 0x000000, 0.35);
    round_rect(ctx, left, top + 2, wide, high, radius);
    cairo_fill(ctx);
    if (lit && !white)
        set_rgb(ctx, INTVSESSION_HELD_RGB);
    else
        set_rgb(ctx, white ? INTVSESSION_ECS_RTN_RGB : INTVSESSION_ECS_KEY_RGB);
    round_rect(ctx, left, top, wide, high, radius);
    cairo_fill(ctx);
    set_rgba(ctx, 0xFFFFFF, 0.14);
    cairo_set_line_width(ctx, 1);
    round_rect(ctx, left + 0.5, top + 0.5, wide - 1, high - 1, radius);
    cairo_stroke(ctx);

    if (lit && !white)
        set_rgb(ctx, 0x000000);
    else
        set_rgb(ctx, ink);
    if (white) {
        draw_arrow(ctx, 4, left + wide / 2, top + high * 0.34, 0.7 * SCALE);
        text_fit(ctx, cap->label, left + wide / 2, top + high * 0.72, wide - 6, 0.5 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    } else if (cap->shifted) {
        if (!(lit && !white))
            set_rgba(ctx, ink, 0.78);
        text_fit(ctx, cap->shifted, left + wide / 2, top + high * 0.27, wide - 6, 0.6 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        if (lit)
            set_rgb(ctx, 0x000000);
        else
            set_rgb(ctx, ink);
        if (arrow_of(cap->label) >= 0)
            draw_arrow(ctx, arrow_of(cap->label), left + wide / 2, top + high * 0.68, 0.7 * SCALE);
        else
            text_fit(ctx, cap->label, left + wide / 2, top + high * 0.66, wide - 6, 0.85 * SCALE,
                     CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    } else {
        text_fit(ctx, cap->label, left + wide / 2, top + high / 2, wide - 6,
                 (word ? 0.6 : 0.85) * SCALE, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    if (lit) {
        set_rgb(ctx, INTVSESSION_HELD_RGB);
        if (white) {
            cairo_set_source_rgba(ctx, HELD_RED, HELD_GREEN, HELD_BLUE, 0.40);
            round_rect(ctx, left, top, wide, high, radius);
            cairo_fill(ctx);
            set_rgb(ctx, INTVSESSION_HELD_RGB);
        }
        cairo_set_line_width(ctx, 0.18 * SCALE);
        round_rect(ctx, left + 0.09 * SCALE, top + 0.09 * SCALE, wide - 0.18 * SCALE,
                   high - 0.18 * SCALE, radius);
        cairo_stroke(ctx);
    }
}

static void draw_all(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    int idx;
    (void)area; (void)height; (void)data;

    g_offset = (width - g_lw * SCALE) / 2;
    for (idx = 0; idx < g_count; idx++) {
        const intvsession_ecs_cap *cap = &g_layout[idx];
        if (cap->kind == INTVSESSION_ECS_BODY)
            draw_body(ctx, cap);
        else
            draw_cap(ctx, cap, intvsession_ecs_key_held(g_session, (intvsession_ecs_key)cap->key));
    }
}

/* ---- the pointer ------------------------------------------------------------ */

static void release_pointer(void)
{
    if (g_down_idx >= 0 && g_down_idx < g_count && g_session)
        intvsession_ecs_key_set(g_session, (intvsession_ecs_key)g_layout[g_down_idx].key, 0);
    g_down_idx = -1;
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void on_drag_begin(GtkGestureDrag *gesture, double pixel_x, double pixel_y, gpointer data)
{
    const int idx = intvsession_ecs_hit((float)((pixel_x - g_offset) / SCALE),
                                        (float)((pixel_y - MARGIN) / SCALE));
    const intvsession_ecs_cap *cap;
    (void)gesture; (void)data;
    if (idx < 0 || idx >= g_count)
        return;
    release_pointer();
    cap = &g_layout[idx];
    if (cap->flags & INTVSESSION_ECSCAP_LATCH) {
        /* SHIFT and CTL latch: a click flips them */
        intvsession_ecs_key_set(g_session, (intvsession_ecs_key)cap->key,
                                !intvsession_ecs_key_held(g_session, (intvsession_ecs_key)cap->key));
    } else {
        g_down_idx = idx;
        intvsession_ecs_key_set(g_session, (intvsession_ecs_key)cap->key, 1);
    }
    gtk_widget_queue_draw(g_area);
}

static void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data)
{
    (void)gesture; (void)offset_x; (void)offset_y; (void)data;
    release_pointer();
}

static void on_cancelled(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data)
{
    (void)gesture; (void)sequence; (void)data;
    release_pointer();
}

/* A redraw whenever a cap's light changes: a click, the physical keyboard, a
 * latched Shift cleared. */
static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    guint64 signature = 1469598103934665603ull;
    int idx;
    (void)widget; (void)clock; (void)data;
    for (idx = 0; idx < g_count; idx++)
        if (g_layout[idx].kind == INTVSESSION_ECS_KEYCAP)
            signature = signature * 1099511628211ull +
                        (guint64)(intvsession_ecs_key_held(g_session, (intvsession_ecs_key)g_layout[idx].key) ? 2 : 1);
    if (signature != g_last_signature) {
        g_last_signature = signature;
        gtk_widget_queue_draw(g_area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard passthrough (see file header) ----------------------------- */

static gboolean forward_key(GtkEventControllerKey *ctrl, guint keyval,
                            guint keycode, GdkModifierType state, int down)
{
    if (!g_session)
        return FALSE;
    /* the keys the main window keeps for itself */
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F10 || keyval == GDK_KEY_F11 ||
        keyval == GDK_KEY_F12) {
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_F9)
            intv_controllers_window_toggle(g_parent, g_session);
        else if (keyval == GDK_KEY_F10)
            intv_ecskbd_window_toggle(g_parent, g_session);
        else if (keyval == GDK_KEY_F11) {
            if (gtk_window_is_fullscreen(g_parent))
                gtk_window_unfullscreen(g_parent);
            else
                gtk_window_fullscreen(g_parent);
        } else {
            gtk_widget_activate_action(GTK_WIDGET(g_parent), "win.debugger", NULL);
        }
        return TRUE;
    }
    intvsession_ecs_key_event(g_session, keycode,
                              intv_keysym_from_key_event(ctrl, keyval, keycode, state),
                              intv_char_from_keyval(keyval), down);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data)
{
    (void)data;
    return forward_key(ctrl, keyval, keycode, state, 1);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer data)
{
    (void)data;
    forward_key(ctrl, keyval, keycode, state, 0);
}

/* Losing focus, or the window closing, must not leave a key stuck down in
 * the emulated matrix -- see intvsession_ecs_keys_clear's own comment. */
static void release_all(void)
{
    g_down_idx = -1;
    if (g_session)
        intvsession_ecs_keys_clear(g_session);
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer data)
{
    (void)ctrl; (void)data;
    release_all();
}

static void on_active_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    if (!gtk_window_is_active(GTK_WINDOW(object)))
        release_all();
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    (void)data;
    release_all();
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE; /* don't destroy -- this is a singleton, hide and reuse */
}

/* What the window says about the machine: ECS missing or off, and where the
 * physical keyboard goes. */
static void refresh_notices(void)
{
    gtk_widget_set_visible(g_notice, !intvsession_has_ecs_rom(g_session) ||
                                         intvsession_get_int(g_session, "ecs", INTVSESSION_HW_AUTO) ==
                                             INTVSESSION_HW_OFF);
    gtk_label_set_text(GTK_LABEL(g_mode),
        intvsession_get_int(g_session, "keyboard_mode", 0)
            ? "The keyboard types on the ECS (Preferences, Input)."
            : "The keyboard drives the hand controllers (Preferences, Input). Typing in this window always goes to the ECS.");
}

static void ensure_window(GtkWindow *parent, intvsession *session)
{
    GtkWidget *root, *header, *toolbar;
    GtkEventController *keys, *focus;
    GtkGesture *drag;

    if (g_win)
        return;

    g_session = session;
    g_parent = parent;
    g_win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(g_win, "ECS Keyboard (F10)");
    gtk_window_set_resizable(g_win, FALSE);
    gtk_window_set_transient_for(g_win, parent);
    gtk_window_set_modal(g_win, FALSE);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 6);
    gtk_widget_set_margin_bottom(root, 12);

    g_notice = gtk_label_new("ECS is not enabled -- see Preferences to turn it on.");
    gtk_widget_add_css_class(g_notice, "dim-label");
    gtk_box_append(GTK_BOX(root), g_notice);

    g_layout = intvsession_ecs_layout(&g_count, &g_lw, &g_lh);
    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), (int)(g_lw * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(g_lh * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_all, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_add_tick_callback(g_area, tick_cb, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_area);

    g_mode = gtk_label_new("");
    gtk_widget_add_css_class(g_mode, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(g_mode), TRUE);
    gtk_label_set_justify(GTK_LABEL(g_mode), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(g_mode), 80);
    gtk_box_append(GTK_BOX(root), g_mode);
    refresh_notices();

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), intv_help_button("ecskbd"));
    intv_help_set_context(GTK_WINDOW(g_win), "ecskbd");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_win), toolbar);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), focus);

    g_signal_connect(g_win, "notify::is-active", G_CALLBACK(on_active_changed), NULL);
    g_signal_connect(g_win, "close-request", G_CALLBACK(on_close_request), NULL);
}

void intv_ecskbd_window_toggle(GtkWindow *parent, intvsession *session)
{
    ensure_window(parent, session);
    g_session = session;
    if (gtk_widget_get_visible(GTK_WIDGET(g_win))) {
        gtk_widget_set_visible(GTK_WIDGET(g_win), FALSE);
        release_all();
    } else {
        refresh_notices();
        gtk_window_present(g_win);
    }
}

gboolean intv_ecskbd_window_is_visible(void)
{
    return g_win != NULL && gtk_widget_get_visible(GTK_WIDGET(g_win));
}
