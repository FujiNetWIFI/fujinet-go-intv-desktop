/*
 * The Controllers window: the Intellivision II handsets (two, or four with
 * the ECS on) and the console's RESET drawn with cairo from the core's layout
 * (intvsession_controls_layout, hit-tested with intvsession_controls_hit),
 * from photos of the real things (DESIGN-intv.md): a warm grey slab, a black
 * keypad with red-outlined keys and white legends, a black disc in a cream
 * ring, four grey side tabs, a cable stub; under them the console's label
 * band, grille and the black ON/OFF + RESET block.
 *
 * Every press is the raw press and release of a gesture, not a button's
 * "clicked": a keypad key on the real hardware is held for as long as the
 * finger is down, and the machine samples its inputs once per frame, so a
 * value present only for the instant of a click would fall between two
 * frames. A part on screen is HELD for as long as the mouse button is down
 * (intvsession_pointer_set); the disc follows the pointer while it is dragged
 * (16 sectors from its centre) and is let go on release. Everything the
 * pointer holds is let go on mouse-up, on the gesture's cancel, on focus loss,
 * when the window hides or maps and when Map is armed.
 *
 * Lights come from the core's source table (intvsession_buttons_held,
 * intvsession_disc_held), never from the decoded pad byte: on real hardware a
 * keypad press also grounds disc and button lines, so lighting from the
 * machine's side would show phantom presses.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Not
 * resizable: the layout has the same height whether the ECS is on or not, so
 * only a restart with the ECS option changed changes its width.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (on the disc, the
 * sector under the click) to pick the target, then press a keyboard key OR a
 * gamepad button; Esc cancels. Clear unbinds the picked target; Defaults
 * restores the table; Show bindings paints each part's binding on it. While a
 * target is picked the other parts dim to 40%. The two system buttons under
 * the picture (Reset to CONFIG, Stop in debugger) fire their action, or in
 * Map pick it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../ecskbd/ecskbd_window.h"
#include "../help/help_window.h"
#include "../keysym_map.h"
#include "../window.h"

#include <math.h>
#include <string.h>

#define SCALE   19.0        /* pixels per layout unit (>= 17: keys stay >= 36 px) */
#define MARGIN  10.0
#define MAX_PADS 8
#define SECTORS 16
#define SECTOR_ANGLE (G_PI / 8.0)

#define HELD_RED   (((INTVSESSION_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_GREEN (((INTVSESSION_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_BLUE  ((INTVSESSION_HELD_RGB & 255) / 255.0)

enum { MAP_IDLE = 0, MAP_ARMED, MAP_PICKED };

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the IntvWindow: system actions go through it */
static intvsession *g_session;
static GtkWidget *g_area;

static const intvsession_control *g_layout;
static int g_count;
static int g_ecs = -1;          /* the ECS state the layout was built for */
static float g_lw, g_lh;
static double g_offset;         /* the picture is centred in the canvas */

static int g_map_state = MAP_IDLE;
static intvsession_key_mapping g_map_target;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_map_hint;
static GtkWidget *g_system_button[INTVSESSION_SYSACT_COUNT];
static guint g_capture_timer, g_info_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static gboolean g_sysact_down[INTVSESSION_SYSACT_COUNT];
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_drives[MAX_PADS];
static int g_pad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_syncing;
static gboolean g_show_bindings;

static int g_drag_idx = -1;     /* the layout part the drag started on, or -1 */
static int g_drag_disc = -1;    /* the sector the pointer holds on a drag of the disc */
static guint64 g_last_signature;

/* ---- helpers --------------------------------------------------------------- */

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void redraw(void)
{
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void release_pointer(void)
{
    if (g_session)
        intvsession_release_pointer(g_session);
    g_drag_idx = -1;
    g_drag_disc = -1;
    redraw();
}

static const intvsession_control *part(int idx)
{
    return idx >= 0 && idx < g_count ? &g_layout[idx] : NULL;
}

static int side_count(void)
{
    return g_ecs > 0 ? 4 : 2;
}

static double to_x(double units) { return g_offset + units * SCALE; }
static double to_y(double units) { return MARGIN + units * SCALE; }

static gboolean same_target(intvsession_key_mapping first, intvsession_key_mapping second)
{
    if (first.kind != second.kind)
        return FALSE;
    switch (first.kind) {
    case INTVSESSION_MAP_KEY:    return first.side == second.side && first.key == second.key;
    case INTVSESSION_MAP_DISC:   return first.side == second.side && first.direction == second.direction;
    case INTVSESSION_MAP_SYSACT: return first.sysact == second.sysact;
    default:                     return FALSE;
    }
}

static gboolean is_picked(intvsession_key_mapping target)
{
    return g_map_state == MAP_PICKED && same_target(g_map_target, target);
}

/* ---- gamepad capture and Map state ---------------------------------------- */

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    intvsession_gamepad_capture_cancel(g_session);
}

static void refresh_clear_button(void)
{
    gtk_widget_set_sensitive(g_clear_button, g_map_state == MAP_PICKED);
}

static void set_map_state(int state, intvsession_key_mapping target);

static gboolean capture_tick(gpointer data)
{
    intvsession_pad_button pad_button;
    (void)data;
    if (g_map_state != MAP_PICKED) {
        g_capture_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (intvsession_gamepad_capture_poll(g_session, &pad_button)) {
        char stolen[128], message[240];
        stolen[0] = '\0';
        intvsession_target_set_button(g_session, g_map_target, pad_button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(message, sizeof message, "Bound %s (was %s)",
                       intvsession_pad_button_name(pad_button), stolen);
        else
            g_snprintf(message, sizeof message, "Bound %s",
                       intvsession_pad_button_name(pad_button));
        g_capture_timer = 0;
        set_map_state(MAP_ARMED, g_map_target);   /* remapping several in a row is normal */
        set_hint(message);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state, intvsession_key_mapping target)
{
    if (state != MAP_IDLE) {
        release_pointer();                       /* a part held into Map would never be released */
        intvsession_release_all(g_session);      /* Map swallows key-ups: drop keyboard holds too */
    }
    g_map_state = state;
    g_map_target = target;
    if (state == MAP_IDLE) {
        stop_capture();
        g_swallow_code = 0;
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "intv-accent");
        set_hint("Hold a key, a side button or the disc with the mouse. Click RESET to reset the game.");
    } else if (state == MAP_ARMED) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "intv-accent");
        set_hint("Click a control to remap (on the disc, a direction). Esc backs out");
    } else {
        char name[128], message[200];
        intvsession_target_name(target, name, sizeof name);
        g_snprintf(message, sizeof message, "Press a key or gamepad button for %s (Esc cancels)", name);
        set_hint(message);
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "intv-accent");
        stop_capture();
        intvsession_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_clear_button();
    redraw();
}

static void map_idle(void)
{
    intvsession_key_mapping none = { .kind = INTVSESSION_MAP_NONE };
    set_map_state(MAP_IDLE, none);
}

static void map_armed(void)
{
    intvsession_key_mapping none = { .kind = INTVSESSION_MAP_NONE };
    set_map_state(MAP_ARMED, none);
}

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

static void add_stop(cairo_pattern_t *pattern, double offset, unsigned hex)
{
    cairo_pattern_add_color_stop_rgb(pattern, offset, ((hex >> 16) & 255) / 255.0,
                                     ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0);
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

static void circle(cairo_t *ctx, double centre_x, double centre_y, double radius)
{
    cairo_new_sub_path(ctx);
    cairo_arc(ctx, centre_x, centre_y, radius, 0, 2 * G_PI);
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

/* What a part shows in Map mode and with Show bindings: its key and pad button
 * (a dash when there is none). */
static void binding_text(intvsession_key_mapping target, char *key, int key_size,
                         char *pad, int pad_size)
{
    const intvsession_binding binding = intvsession_target_binding_get(g_session, target);
    key[0] = pad[0] = '\0';
    if (binding.keysym)
        intvsession_keysym_name(binding.keysym, key, key_size);
    if (!key[0])
        g_strlcpy(key, "\xe2\x80\x94", (gsize)key_size);
    if (binding.button != INTVSESSION_PAD_BTN_NONE)
        g_strlcpy(pad, intvsession_pad_button_name(binding.button), (gsize)pad_size);
}

static gboolean bindings_on(void)
{
    return g_map_state != MAP_IDLE || g_show_bindings;
}

/* The held ring (0.18 u) and a 40% fill over a rounded part. */
static void held_ring(cairo_t *ctx, double left, double top, double wide, double high, double radius)
{
    cairo_set_source_rgba(ctx, HELD_RED, HELD_GREEN, HELD_BLUE, 0.40);
    round_rect(ctx, left, top, wide, high, radius);
    cairo_fill(ctx);
    set_rgb(ctx, INTVSESSION_HELD_RGB);
    cairo_set_line_width(ctx, 0.18 * SCALE);
    round_rect(ctx, left + 0.09 * SCALE, top + 0.09 * SCALE, wide - 0.18 * SCALE,
               high - 0.18 * SCALE, radius);
    cairo_stroke(ctx);
}

/* ---- captions and the bodies ------------------------------------------------ */

static void draw_caption(cairo_t *ctx, const intvsession_control *ctl, const GdkRGBA *foreground)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;
    if (ctl->side >= 0) {                      /* "Player 1" ... "ECS 2" */
        cairo_set_source_rgba(ctx, foreground->red, foreground->green, foreground->blue, 0.85);
        text_fit(ctx, ctl->label, left + wide / 2, top + high / 2, wide, 13,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        return;
    }
    cairo_select_font_face(ctx, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    if (!strcmp(ctl->label, "INTELLIVISION II")) {
        cairo_text_extents_t extents;
        set_rgb(ctx, INTVSESSION_LEGEND_RGB);
        cairo_set_font_size(ctx, 0.95 * SCALE);
        cairo_text_extents(ctx, ctl->label, &extents);
        cairo_move_to(ctx, left, top + high / 2 - extents.height / 2 - extents.y_bearing);
        cairo_show_text(ctx, ctl->label);
    } else {                                   /* MATTEL ELECTRONICS, right-aligned in red */
        cairo_text_extents_t extents;
        set_rgb(ctx, INTVSESSION_MATTEL_RED_RGB);
        cairo_set_font_size(ctx, 0.7 * SCALE);
        cairo_text_extents(ctx, ctl->label, &extents);
        cairo_move_to(ctx, left + wide - extents.width - extents.x_bearing,
                      top + high / 2 - extents.height / 2 - extents.y_bearing);
        cairo_show_text(ctx, ctl->label);
    }
}

/* The handset slab: warm grey with a lit top edge and a shaded one below. */
static void draw_handset(cairo_t *ctx, const intvsession_control *ctl)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE, radius = 0.8 * SCALE;
    cairo_pattern_t *pattern;

    set_rgba(ctx, 0x000000, 0.28);
    round_rect(ctx, left + 1, top + 3, wide, high, radius);
    cairo_fill(ctx);
    pattern = cairo_pattern_create_linear(0, top, 0, top + high);
    add_stop(pattern, 0.0, INTVSESSION_BEVEL_RGB);
    add_stop(pattern, 0.12, INTVSESSION_HANDSET_RGB);
    add_stop(pattern, 0.85, INTVSESSION_HANDSET_RGB);
    add_stop(pattern, 1.0, INTVSESSION_SHADE_RGB);
    cairo_set_source(ctx, pattern);
    round_rect(ctx, left, top, wide, high, radius);
    cairo_fill_preserve(ctx);
    cairo_pattern_destroy(pattern);
    set_rgba(ctx, INTVSESSION_SHADE_RGB, 0.9);
    cairo_set_line_width(ctx, 1);
    cairo_stroke(ctx);
}

/* The cream ring round the disc, and the black pad's lathe finish. */
static void draw_ring(cairo_t *ctx, const intvsession_control *ctl)
{
    const double radius = ctl->w * SCALE / 2;
    const double centre_x = to_x(ctl->x) + radius, centre_y = to_y(ctl->y) + radius;

    set_rgba(ctx, 0x000000, 0.25);
    circle(ctx, centre_x + 0.5, centre_y + 1.5, radius);
    cairo_fill(ctx);
    set_rgb(ctx, INTVSESSION_RING_RGB);
    circle(ctx, centre_x, centre_y, radius);
    cairo_fill_preserve(ctx);
    set_rgba(ctx, INTVSESSION_SHADE_RGB, 0.9);
    cairo_set_line_width(ctx, 1);
    cairo_stroke(ctx);
}

static void draw_grille(cairo_t *ctx, const intvsession_control *ctl)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;
    int slat;
    set_rgb(ctx, INTVSESSION_GRILLE_RGB);
    cairo_set_line_cap(ctx, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(ctx, 0.2 * SCALE);
    for (slat = 0; slat < 6; slat++) {
        const double slat_y = top + (slat + 0.5) * high / 6;
        cairo_move_to(ctx, left + 0.2 * SCALE, slat_y);
        cairo_line_to(ctx, left + wide - 0.2 * SCALE, slat_y);
    }
    cairo_stroke(ctx);
    cairo_set_line_cap(ctx, CAIRO_LINE_CAP_BUTT);
}

static void draw_body(cairo_t *ctx, const intvsession_control *ctl)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;
    const char *label = ctl->label;

    if (!strcmp(label, "Cable")) {
        set_rgb(ctx, INTVSESSION_CABLE_RGB);
        round_rect(ctx, left, top, wide, high, 0.3 * SCALE);
        cairo_fill(ctx);
    } else if (!strcmp(label, "Handset")) {
        draw_handset(ctx, ctl);
    } else if (!strcmp(label, "Keypad")) {
        set_rgb(ctx, INTVSESSION_KEYPAD_RGB);
        round_rect(ctx, left, top, wide, high, 0.4 * SCALE);
        cairo_fill(ctx);
    } else if (!strcmp(label, "Card")) {
        set_rgb(ctx, INTVSESSION_CARD_EDGE_RGB);
        cairo_set_line_width(ctx, 1.2);
        round_rect(ctx, left, top, wide, high, 0.5 * SCALE);
        cairo_stroke(ctx);
    } else if (!strcmp(label, "Ring")) {
        draw_ring(ctx, ctl);
    } else if (!strcmp(label, "Console")) {
        set_rgb(ctx, INTVSESSION_CONSOLE_RGB);
        round_rect(ctx, left, top, wide, high, 0.5 * SCALE);
        cairo_fill_preserve(ctx);
        set_rgba(ctx, INTVSESSION_SHADE_RGB, 0.9);
        cairo_set_line_width(ctx, 1);
        cairo_stroke(ctx);
    } else if (!strcmp(label, "Band")) {
        set_rgb(ctx, INTVSESSION_BAND_RGB);
        round_rect(ctx, left, top, wide, high, 0.25 * SCALE);
        cairo_fill(ctx);
    } else if (!strcmp(label, "RedLine")) {
        set_rgb(ctx, INTVSESSION_MATTEL_RED_RGB);
        cairo_rectangle(ctx, left, top, wide, high < 2 ? 2 : high);
        cairo_fill(ctx);
    } else if (!strcmp(label, "Grille")) {
        draw_grille(ctx, ctl);
    } else if (!strcmp(label, "Block")) {
        set_rgb(ctx, INTVSESSION_BLOCK_RGB);
        round_rect(ctx, left, top, wide, high, 0.3 * SCALE);
        cairo_fill(ctx);
    } else if (!strcmp(label, "OnOff")) {
        /* the dimmed half: nothing to press */
        set_rgba(ctx, INTVSESSION_BLOCK_LEGEND_RGB, 0.55);
        text_fit(ctx, "ON/OFF", left + wide / 2, top + high / 2, wide - 8, 0.62 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        set_rgba(ctx, 0x000000, 0.5);
        cairo_set_line_width(ctx, 1);
        cairo_move_to(ctx, left + wide + 0.1 * SCALE, top - 0.05 * SCALE);
        cairo_line_to(ctx, left + wide + 0.1 * SCALE, top + high + 0.05 * SCALE);
        cairo_stroke(ctx);
    } else if (!strcmp(label, "Led")) {
        set_rgb(ctx, INTVSESSION_LED_RGB);
        circle(ctx, left + wide / 2, top + high / 2, wide / 2);
        cairo_fill(ctx);
        set_rgba(ctx, 0xFFFFFF, 0.35);
        circle(ctx, left + wide * 0.4, top + high * 0.38, wide * 0.14);
        cairo_fill(ctx);
    }
}

/* ---- parts ------------------------------------------------------------------ */

/* The binding text under a legend: the key, and the pad button under it when
 * the part is big enough. */
static void draw_binding(cairo_t *ctx, intvsession_key_mapping target, double centre_x,
                         double top, double wide, double high, double key_size, gboolean on_held)
{
    char key[64], pad[64];
    binding_text(target, key, sizeof key, pad, sizeof pad);
    set_rgb(ctx, on_held ? 0x000000 : INTVSESSION_HELD_RGB);
    text_fit(ctx, key, centre_x, top + high * (pad[0] ? 0.62 : 0.76), wide, key_size,
             CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    if (pad[0]) {
        set_rgb(ctx, on_held ? 0x1A1A1A : 0xB5B5BC);
        text_fit(ctx, pad, centre_x, top + high * 0.88, wide, key_size * 0.85,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
}

/* A keypad key: black face, red outline, white legend. Held (or picked) it is
 * the solid held colour with the legend turned black. */
static void draw_keypad_key(cairo_t *ctx, const intvsession_control *ctl, gboolean lit, double alpha)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;
    const double outline = 0.14 * SCALE;
    const gboolean words = ctl->label[1] != '\0';     /* "clear", "enter" */
    const intvsession_key_mapping target = intvsession_control_target(ctl);
    const gboolean binding = bindings_on();

    cairo_push_group(ctx);
    set_rgb(ctx, lit ? INTVSESSION_HELD_RGB : INTVSESSION_KEYFACE_RGB);
    round_rect(ctx, left, top, wide, high, 0.4 * SCALE);
    cairo_fill(ctx);
    set_rgb(ctx, lit ? INTVSESSION_HELD_RGB : INTVSESSION_KEYRED_RGB);
    cairo_set_line_width(ctx, lit ? 0.18 * SCALE : outline);
    round_rect(ctx, left + outline / 2, top + outline / 2, wide - outline, high - outline,
               0.4 * SCALE - outline / 2);
    cairo_stroke(ctx);
    if (lit) {
        set_rgb(ctx, INTVSESSION_KEYPAD_RGB);
        cairo_set_line_width(ctx, 1);
        round_rect(ctx, left + 0.5, top + 0.5, wide - 1, high - 1, 0.4 * SCALE);
        cairo_stroke(ctx);
    }
    if (lit)
        set_rgb(ctx, 0x000000);
    else
        set_rgb(ctx, INTVSESSION_LEGEND_RGB);
    if (binding) {
        text_fit(ctx, ctl->label, left + wide / 2, top + high * 0.32, wide - 8,
                 words ? 9 : 11, CAIRO_FONT_SLANT_NORMAL,
                 words ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
        draw_binding(ctx, target, left + wide / 2, top, wide - 6, high, 8, lit);
    } else {
        text_fit(ctx, ctl->label, left + wide / 2, top + high / 2, wide - 6,
                 words ? 0.62 * SCALE : 0.95 * SCALE, CAIRO_FONT_SLANT_NORMAL,
                 words ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, alpha);
}

/* A side tab: a plain grey tab with a faint T, L or R; held it gets the ring. */
static void draw_tab(cairo_t *ctx, const intvsession_control *ctl, gboolean lit, double alpha)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;
    const double radius = 0.3 * SCALE;

    cairo_push_group(ctx);
    set_rgb(ctx, INTVSESSION_TAB_RGB);
    round_rect(ctx, left, top, wide, high, radius);
    cairo_fill_preserve(ctx);
    set_rgb(ctx, INTVSESSION_TAB_EDGE_RGB);
    cairo_set_line_width(ctx, 1);
    cairo_stroke(ctx);
    if (bindings_on()) {
        set_rgb(ctx, INTVSESSION_TAB_LEGEND_RGB);
        text_fit(ctx, ctl->label, left + wide / 2, top + high * 0.25, wide, 9,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        {
            char key[64], pad[64];
            binding_text(intvsession_control_target(ctl), key, sizeof key, pad, sizeof pad);
            set_rgb(ctx, INTVSESSION_ACCENT_RGB);
            text_fit(ctx, key, left + wide / 2, top + high * 0.68, wide - 2, 7.5,
                     CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        }
    } else {
        set_rgb(ctx, INTVSESSION_TAB_LEGEND_RGB);
        text_fit(ctx, ctl->label, left + wide / 2, top + high / 2, wide, 10,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    if (lit)
        held_ring(ctx, left, top, wide, high, radius);
    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, alpha);
}

/* One of the disc's 16 sectors, from 22% to 100% of the radius. Sector n is
 * centred on n * 22.5 degrees counter-clockwise from East. */
static void sector_wedge(cairo_t *ctx, double centre_x, double centre_y, double radius, int sector)
{
    const double centre_angle = -sector * SECTOR_ANGLE;
    const double half = SECTOR_ANGLE / 2;
    cairo_new_sub_path(ctx);
    cairo_arc(ctx, centre_x, centre_y, radius, centre_angle - half, centre_angle + half);
    cairo_arc_negative(ctx, centre_x, centre_y, radius * 0.22, centre_angle + half, centre_angle - half);
    cairo_close_path(ctx);
}

/* The disc: a black pad with a faint lathe finish; the held sector is a green
 * wedge, a picked one a stronger one. In Map the bindings sit in the sectors. */
static void draw_disc(cairo_t *ctx, const intvsession_control *ctl, double alpha)
{
    const double radius = ctl->w * SCALE / 2;
    const double centre_x = to_x(ctl->x) + radius, centre_y = to_y(ctl->y) + radius;
    const int held = intvsession_disc_held(g_session, ctl->side);
    int sector;

    cairo_push_group(ctx);
    set_rgb(ctx, INTVSESSION_DISC_RGB);
    circle(ctx, centre_x, centre_y, radius);
    cairo_fill(ctx);
    /* the lathe finish, and the sector spokes very faint */
    set_rgb(ctx, INTVSESSION_DISC_RING_RGB);
    cairo_set_line_width(ctx, 1);
    for (sector = 1; sector <= 3; sector++) {
        circle(ctx, centre_x, centre_y, radius * sector * 0.25);
        cairo_stroke(ctx);
    }
    set_rgba(ctx, 0xFFFFFF, 0.07);
    for (sector = 0; sector < SECTORS; sector++) {
        const double angle = -(sector + 0.5) * SECTOR_ANGLE;
        cairo_move_to(ctx, centre_x + cos(angle) * radius * 0.22, centre_y + sin(angle) * radius * 0.22);
        cairo_line_to(ctx, centre_x + cos(angle) * radius, centre_y + sin(angle) * radius);
    }
    cairo_stroke(ctx);
    set_rgb(ctx, INTVSESSION_DISC_RING_RGB);
    circle(ctx, centre_x, centre_y, radius * 0.22);
    cairo_stroke(ctx);

    if (held >= 0) {
        cairo_set_source_rgba(ctx, HELD_RED, HELD_GREEN, HELD_BLUE, 0.7);
        sector_wedge(ctx, centre_x, centre_y, radius, held);
        cairo_fill_preserve(ctx);
        set_rgb(ctx, INTVSESSION_HELD_RGB);
        cairo_set_line_width(ctx, 1.5);
        cairo_stroke(ctx);
    }
    if (g_map_state == MAP_PICKED && g_map_target.kind == INTVSESSION_MAP_DISC &&
        (int)g_map_target.side == ctl->side) {
        cairo_set_source_rgba(ctx, HELD_RED, HELD_GREEN, HELD_BLUE, 0.7);
        sector_wedge(ctx, centre_x, centre_y, radius, g_map_target.direction);
        cairo_fill_preserve(ctx);
        set_rgb(ctx, INTVSESSION_HELD_RGB);
        cairo_set_line_width(ctx, 2);
        cairo_stroke(ctx);
    }
    if (bindings_on()) {
        for (sector = 0; sector < SECTORS; sector++) {
            const intvsession_binding binding = intvsession_target_binding_get(
                g_session, intvsession_target_disc((intvsession_pad_side)ctl->side, sector));
            char key[64];
            double angle;
            if (!binding.keysym)
                continue;
            key[0] = '\0';
            intvsession_keysym_name(binding.keysym, key, sizeof key);
            if (!key[0])
                continue;
            angle = -sector * SECTOR_ANGLE;
            set_rgb(ctx, sector == held ? 0x000000 : INTVSESSION_HELD_RGB);
            text_fit(ctx, key, centre_x + cos(angle) * radius * 0.66,
                     centre_y + sin(angle) * radius * 0.66, 14, 6.5,
                     CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        }
    }
    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, alpha);
}

/* The console's RESET half: dim legend on the black block; pressed or picked
 * it gets the held ring. */
static void draw_reset(cairo_t *ctx, const intvsession_control *ctl, gboolean lit, double alpha)
{
    const double left = to_x(ctl->x), top = to_y(ctl->y);
    const double wide = ctl->w * SCALE, high = ctl->h * SCALE;

    cairo_push_group(ctx);
    set_rgb(ctx, 0x3F4347);
    round_rect(ctx, left, top, wide, high, 0.25 * SCALE);
    cairo_fill(ctx);
    set_rgb(ctx, INTVSESSION_BLOCK_LEGEND_RGB);
    if (bindings_on()) {
        text_fit(ctx, ctl->label, left + wide / 2, top + high * 0.3, wide - 6, 8,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        draw_binding(ctx, intvsession_control_target(ctl), left + wide / 2, top, wide - 6, high, 7.5, FALSE);
    } else {
        text_fit(ctx, ctl->label, left + wide / 2, top + high / 2, wide - 8, 0.62 * SCALE,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    if (lit)
        held_ring(ctx, left, top, wide, high, 0.25 * SCALE);
    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, alpha);
}

static void draw_all(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    GdkRGBA foreground;
    int idx;
    (void)data; (void)height;

    gtk_widget_get_color(GTK_WIDGET(area), &foreground);
    g_offset = (width - g_lw * SCALE) / 2;
    for (idx = 0; idx < g_count; idx++) {
        const intvsession_control *ctl = &g_layout[idx];
        const intvsession_key_mapping target = intvsession_control_target(ctl);
        const gboolean pickable = ctl->kind == INTVSESSION_CTL_KEY ||
                                  ctl->kind == INTVSESSION_CTL_ACTION ||
                                  ctl->kind == INTVSESSION_CTL_BUTTON;
        const gboolean picked = pickable && is_picked(target);
        gboolean lit = picked;
        double alpha = g_map_state == MAP_PICKED && !picked ? 0.4 : 1.0;

        switch (ctl->kind) {
        case INTVSESSION_CTL_CAPTION:
            draw_caption(ctx, ctl, &foreground);
            break;
        case INTVSESSION_CTL_BODY:
            draw_body(ctx, ctl);
            break;
        case INTVSESSION_CTL_KEY:
            lit = lit || ((intvsession_buttons_held(g_session, (intvsession_pad_side)ctl->side) >> ctl->target) & 1);
            draw_keypad_key(ctx, ctl, lit, alpha);
            break;
        case INTVSESSION_CTL_ACTION:
            lit = lit || ((intvsession_buttons_held(g_session, (intvsession_pad_side)ctl->side) >> ctl->target) & 1);
            draw_tab(ctx, ctl, lit, alpha);
            break;
        case INTVSESSION_CTL_DISC:
            alpha = g_map_state == MAP_PICKED &&
                    !(g_map_target.kind == INTVSESSION_MAP_DISC && (int)g_map_target.side == ctl->side)
                    ? 0.4 : 1.0;
            draw_disc(ctx, ctl, alpha);
            break;
        case INTVSESSION_CTL_BUTTON:
            lit = lit || g_drag_idx == idx;
            draw_reset(ctx, ctl, lit, alpha);
            break;
        }
    }
}

/* ---- sizing ---------------------------------------------------------------- */

/* The layout for the ECS state now. Its height is the same either way, so
 * only the width changes (and only after a restart). */
static void relayout(void)
{
    g_ecs = intvsession_ecs_on(g_session) ? 1 : 0;
    g_layout = intvsession_controls_layout(g_ecs, &g_count, &g_lw, &g_lh);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), (int)(g_lw * SCALE + 2 * MARGIN));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(g_lh * SCALE + 2 * MARGIN));
}

/* A signature of everything that decides how the canvas looks. */
static guint64 paint_signature(void)
{
    guint64 signature = (guint64)g_ecs + 1 + (guint64)(g_drag_idx + 1) * 7;
    int side;
    for (side = 0; side < side_count(); side++) {
        signature = signature * 1099511628211ull +
                    intvsession_buttons_held(g_session, (intvsession_pad_side)side);
        signature = signature * 1099511628211ull +
                    (guint64)(intvsession_disc_held(g_session, (intvsession_pad_side)side) + 1);
    }
    return signature;
}

/* Every frame while the window is on screen: follow what the machine sees held
 * (keyboard, gamepads and this window together), and the layout when a
 * restart changed the ECS option. */
static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    guint64 signature;
    (void)widget; (void)clock; (void)data;
    if ((intvsession_ecs_on(g_session) ? 1 : 0) != g_ecs) {
        release_pointer();
        relayout();
        g_pad_generation = (unsigned)-1;
    }
    signature = paint_signature();
    if (signature != g_last_signature) {
        g_last_signature = signature;
        redraw();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pointer -------------------------------------------------------------- */

static int disc_sector_at(const intvsession_control *ctl, double pixel_x, double pixel_y)
{
    return intvsession_controls_disc_dir(ctl, (float)((pixel_x - g_offset) / SCALE),
                                         (float)((pixel_y - MARGIN) / SCALE));
}

static void on_drag_begin(GtkGestureDrag *gesture, double pixel_x, double pixel_y, gpointer data)
{
    const int idx = intvsession_controls_hit(g_ecs, (float)((pixel_x - g_offset) / SCALE),
                                             (float)((pixel_y - MARGIN) / SCALE));
    const intvsession_control *ctl = part(idx);
    (void)gesture; (void)data;
    if (!ctl)
        return;
    if (g_map_state != MAP_IDLE) {
        if (ctl->kind == INTVSESSION_CTL_DISC) {
            const int sector = disc_sector_at(ctl, pixel_x, pixel_y);
            if (sector >= 0)
                set_map_state(MAP_PICKED,
                              intvsession_target_disc((intvsession_pad_side)ctl->side, sector));
        } else {
            const intvsession_key_mapping target = intvsession_control_target(ctl);
            if (target.kind != INTVSESSION_MAP_NONE)
                set_map_state(MAP_PICKED, target);
        }
        return;
    }
    release_pointer();
    g_drag_idx = idx;
    if (ctl->kind == INTVSESSION_CTL_DISC) {
        g_drag_disc = disc_sector_at(ctl, pixel_x, pixel_y);
        intvsession_pointer_set(g_session, (intvsession_pad_side)ctl->side, -1, g_drag_disc);
    } else if (ctl->kind == INTVSESSION_CTL_BUTTON) {
        intv_window_sysaction(INTV_WINDOW(g_parent), (intvsession_sysaction)ctl->target);
    } else {
        intvsession_pointer_set(g_session, (intvsession_pad_side)ctl->side, ctl->target, -1);
    }
    redraw();
}

static void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data)
{
    const intvsession_control *ctl = part(g_drag_idx);
    double start_x, start_y;
    int sector;
    (void)data;
    if (!ctl || ctl->kind != INTVSESSION_CTL_DISC || g_map_state != MAP_IDLE)
        return;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    sector = disc_sector_at(ctl, start_x + offset_x, start_y + offset_y);
    if (sector != g_drag_disc) {
        g_drag_disc = sector;
        intvsession_pointer_set(g_session, (intvsession_pad_side)ctl->side, -1, sector);
    }
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

/* ---- gamepads ------------------------------------------------------------- */

static const char *const k_side_names[4] = { "Left", "Right", "ECS Left", "ECS Right" };

static void drives_text(int idx, char *text, int size)
{
    const int effective = intvsession_gamepad_effective_side(g_session, idx);
    if (effective >= 0 && effective < 4)
        g_snprintf(text, (gsize)size, "drives %s", k_side_names[effective]);
    else
        g_snprintf(text, (gsize)size, "drives nothing");
}

static void refresh_pad_sides(void)
{
    int idx;
    for (idx = 0; idx < g_pad_rows; idx++) {
        char text[40];
        drives_text(idx, text, sizeof text);
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_drives[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_drives[idx]), text);
    }
}

static void on_assign_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    const int idx = GPOINTER_TO_INT(user_data);
    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    (void)pspec;
    if (g_syncing)
        return;
    /* 0 = automatic (connection order), then Left, Right and the ECS pair */
    intvsession_gamepad_assign(g_session, idx, selected == 0 ? -1 : (int)selected - 1);
    refresh_pad_sides();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter): one
 * row per pad, its name, the side it drives and the side it is set to. */
static void refresh_pads(void)
{
    const char *choices[6] = { "Automatic", "Left", "Right", NULL, NULL, NULL };
    GtkWidget *child;
    int count, idx;

    if (g_ecs > 0) {
        choices[3] = "ECS Left";
        choices[4] = "ECS Right";
    }
    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_rows = 0;

    count = intvsession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dropdown, *drives;
        char text[128], drives_label[40];
        const int assign = intvsession_gamepad_assignment(g_session, idx);

        intvsession_gamepad_name(g_session, idx, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 22);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_widget_set_tooltip_text(name, text);
        dropdown = gtk_drop_down_new_from_strings(choices);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown),
                                   assign < 0 || assign + 1 >= (g_ecs > 0 ? 5 : 3) ? 0 : (guint)assign + 1);
        g_syncing = FALSE;
        gtk_widget_set_focus_on_click(dropdown, FALSE);
        g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(idx));
        drives_text(idx, drives_label, sizeof drives_label);
        drives = gtk_label_new(drives_label);
        gtk_widget_add_css_class(drives, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(drives), 16);
        gtk_label_set_xalign(GTK_LABEL(drives), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), drives);
        gtk_box_append(GTK_BOX(row), dropdown);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_pad_rows < MAX_PADS)
            g_pad_drives[g_pad_rows++] = drives;
    }
}

/* Every 250 ms while the window is on screen: hot-plugged pads and what AUTO
 * resolved to. */
static gboolean info_tick(gpointer data)
{
    unsigned generation;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    generation = intvsession_gamepad_generation(g_session);
    if (generation != g_pad_generation) {
        g_pad_generation = generation;
        refresh_pads();
    } else {
        refresh_pad_sides();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

/* The keys the main window keeps for itself, reached from here as well. */
static gboolean window_function_key(guint keyval, GdkModifierType state, gboolean down)
{
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F10 || keyval == GDK_KEY_F11 ||
        keyval == GDK_KEY_F12) {
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_F9) {
            map_idle();
            release_pointer();
            gtk_widget_set_visible(g_window, FALSE);
        } else if (keyval == GDK_KEY_F10) {
            intv_ecskbd_window_toggle(g_parent, g_session);
        } else if (keyval == GDK_KEY_F11) {
            if (gtk_window_is_fullscreen(g_parent))
                gtk_window_unfullscreen(g_parent);
            else
                gtk_window_fullscreen(g_parent);
        } else {
            gtk_widget_activate_action(GTK_WIDGET(g_parent), "win.debugger", NULL);
        }
        return TRUE;
    }
    if ((keyval == GDK_KEY_r || keyval == GDK_KEY_R) && (state & GDK_CONTROL_MASK)) {
        if (down)
            gtk_widget_activate_action(GTK_WIDGET(g_parent), "win.reset-config", NULL);
        return TRUE;
    }
    return FALSE;
}

/* Map's share of a key. */
static gboolean map_key(guint keycode, uint32_t keysym, gboolean down, guint keyval)
{
    if (g_swallow_code && keycode == g_swallow_code) {
        if (!down)
            g_swallow_code = 0;
        return TRUE;
    }
    if (!down)
        return TRUE;
    if (keyval == GDK_KEY_Escape) {
        g_swallow_code = keycode;
        if (g_map_state == MAP_PICKED)
            map_armed();
        else
            map_idle();
        return TRUE;
    }
    if (g_map_state != MAP_PICKED)
        return TRUE;               /* armed, nothing picked: keys do nothing */
    if (!keysym) {
        set_hint("No key was reported for that press. Try another, or press Done to leave Map.");
        return TRUE;
    }
    if (intvsession_key_reserved(keysym)) {
        set_hint("F9, F10, F11 and F12 can't be bound");
        g_swallow_code = keycode;
        return TRUE;
    }
    {
        char stolen[128], message[260], name[64];
        stolen[0] = '\0';
        intvsession_target_set_key(g_session, g_map_target, keysym, stolen, sizeof stolen);
        name[0] = '\0';
        intvsession_keysym_name(keysym, name, sizeof name);
        g_swallow_code = keycode;
        map_armed();               /* back to armed */
        if (stolen[0])
            g_snprintf(message, sizeof message, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(message, sizeof message, "Bound %s", name);
        set_hint(message);
    }
    return TRUE;
}

/* Typed keys that Map didn't take: exactly what the main window does with
 * them. */
static gboolean forward_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                            GdkModifierType state, gboolean down)
{
    const uint32_t keysym = intv_keysym_from_key_event(ctrl, keyval, keycode, state);
    const intvsession_key_mapping mapping = intvsession_key_from_keysym_bound(g_session, keysym);

    if (mapping.kind == INTVSESSION_MAP_SYSACT) {
        if (down) {
            if (!g_sysact_down[mapping.sysact]) {
                g_sysact_down[mapping.sysact] = TRUE;
                intv_window_sysaction(INTV_WINDOW(g_parent), mapping.sysact);
            }
        } else {
            g_sysact_down[mapping.sysact] = FALSE;
        }
        return TRUE;
    }
    /* "ECS Keyboard" input mode steals the host keyboard for the ECS here
     * exactly as in the main window */
    if (intvsession_get_int(g_session, "keyboard_mode", 0)) {
        intvsession_ecs_key_event(g_session, keycode, keysym, intv_char_from_keyval(keyval), down);
        return TRUE;
    }
    if (mapping.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(g_session, mapping.side, mapping.key, down);
    else if (mapping.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(g_session, mapping.side, down ? mapping.direction : -1);
    return TRUE;
}

static gboolean on_key(GtkEventControllerKey *ctrl, guint keyval, guint code,
                       GdkModifierType state, gboolean down)
{
    /* an open drop-down (a gamepad's side) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;
    if (!g_parent)
        return FALSE;
    if (window_function_key(keyval, state, down))
        return TRUE;
    if (g_map_state != MAP_IDLE) {
        if (state & GDK_CONTROL_MASK)
            return FALSE;
        return map_key(code, intv_keysym_from_key_event(ctrl, keyval, code, state), down, keyval);
    }
    return forward_key(ctrl, keyval, code, state, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint code,
                               GdkModifierType state, gpointer data)
{
    (void)data;
    return on_key(ctrl, keyval, code, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint code,
                            GdkModifierType state, gpointer data)
{
    (void)data;
    on_key(ctrl, keyval, code, state, FALSE);
}

/* Keys and mouse buttons held into a focus change must not stay down: the
 * release goes to another window. */
static void release_everything(void)
{
    release_pointer();
    intvsession_release_all(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
    g_swallow_code = 0;
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer data)
{
    (void)ctrl; (void)data;
    release_everything();
}

static void on_active_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    if (!gtk_window_is_active(GTK_WINDOW(object)))
        release_everything();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (g_map_state == MAP_IDLE)
        map_armed();
    else
        map_idle();
}

static void on_clear_clicked(GtkButton *button, gpointer data)
{
    char name[128], message[200];
    (void)button; (void)data;
    if (g_map_state != MAP_PICKED)
        return;
    intvsession_binding_clear(g_session, g_map_target);
    intvsession_target_name(g_map_target, name, sizeof name);
    g_snprintf(message, sizeof message, "Cleared %s", name);
    set_hint(message);
    redraw();
}

static void on_defaults_clicked(GtkButton *button, gpointer data)
{
    int idx;
    const int count = intvsession_gamepad_count(g_session);
    (void)button; (void)data;
    intvsession_bindings_reset(g_session);
    for (idx = 0; idx < count; idx++)
        intvsession_gamepad_assign(g_session, idx, -1);
    refresh_pads();
    set_hint("Back to the defaults");
    redraw();
}

static void on_show_toggled(GtkCheckButton *button, gpointer data)
{
    (void)data;
    g_show_bindings = gtk_check_button_get_active(button);
    intvsession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
    redraw();
}

/* The system buttons under the picture fire their action; in Map they pick it. */
static void on_system_clicked(GtkButton *button, gpointer data)
{
    const intvsession_sysaction action = (intvsession_sysaction)GPOINTER_TO_INT(data);
    (void)button;
    if (g_map_state != MAP_IDLE) {
        set_map_state(MAP_PICKED, intvsession_target_sysaction(action));
        return;
    }
    intv_window_sysaction(INTV_WINDOW(g_parent), action);
}

static void hide_window(void)
{
    map_idle();
    release_pointer();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer data)
{
    (void)window; (void)data;
    hide_window();
    return TRUE;
}

/* The info timer runs only while the window is on screen: closing only hides
 * it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    release_pointer();
    if (!g_info_timer)
        g_info_timer = g_timeout_add(250, info_tick, NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    release_pointer();
    if (g_info_timer)
        g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (g_capture_timer)
        g_source_remove(g_capture_timer);
    if (g_info_timer)
        g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_window = NULL;
    g_area = NULL;
    g_pad_rows = 0;
    g_map_state = MAP_IDLE;
    g_ecs = -1;
}

static GtkWidget *plain_button(const char *label, GCallback callback, gpointer data)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(button, FALSE);
    g_signal_connect(button, "clicked", callback, data);
    return button;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *caption, *systemrow, *maprow, *toolbar, *header, *padtitle, *padscroll;
    GtkEventController *keys, *focus;
    GtkGesture *drag;
    int action;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers (F9)");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 6);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_all, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    gtk_widget_add_tick_callback(g_area, tick_cb, NULL, NULL);
    gtk_box_append(GTK_BOX(root), g_area);
    relayout();

    /* the caption row: the buttons are crossed by design */
    caption = gtk_label_new("The action buttons are crossed by design: the right-hand modifier "
                            "keys (Shift, Ctrl, Alt) drive the left controller.");
    gtk_widget_add_css_class(caption, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(caption), TRUE);
    gtk_label_set_justify(GTK_LABEL(caption), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(caption), 70);
    gtk_box_append(GTK_BOX(root), caption);

    /* Reset to CONFIG and Stop in debugger are not on the console: plain buttons */
    systemrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(systemrow, GTK_ALIGN_CENTER);
    {
        GtkWidget *label = gtk_label_new("System buttons");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(systemrow), label);
    }
    for (action = INTVSESSION_SYSACT_RESET_CONFIG; action < INTVSESSION_SYSACT_COUNT; action++) {
        g_system_button[action] = plain_button(
            intvsession_sysaction_name((intvsession_sysaction)action),
            G_CALLBACK(on_system_clicked), GINT_TO_POINTER(action));
        gtk_box_append(GTK_BOX(systemrow), g_system_button[action]);
    }
    gtk_box_append(GTK_BOX(root), systemrow);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(maprow, GTK_ALIGN_CENTER);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked), NULL);
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked), NULL);
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = intvsession_get_int(g_session, "controllers_show_bindings", 0) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 80);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0.5);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    /* two pad rows tall whatever is plugged in (more scroll), so a pad coming
     * or going never resizes the window */
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), intv_help_button("controllers"));
    intv_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    map_idle();
}

void intv_controllers_window_show(GtkWindow *parent, intvsession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;
    if ((intvsession_ecs_on(g_session) ? 1 : 0) != g_ecs)
        relayout();
    g_pad_generation = intvsession_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void intv_controllers_window_toggle(GtkWindow *parent, intvsession *session)
{
    g_session = session;
    if (g_window && gtk_widget_get_visible(g_window))
        hide_window();
    else
        intv_controllers_window_show(parent, session);
}

gboolean intv_controllers_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

void intv_controllers_window_refresh(void)
{
    if (!g_window)
        return;
    g_pad_generation = (unsigned)-1;
    redraw();
}
