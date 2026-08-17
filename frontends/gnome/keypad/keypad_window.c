/*
 * IntvKeypadWindow -- both hand controllers side by side: a 3x4 keypad grid
 * (1-9, Clear/0/Enter), three action buttons (top, lower-left, lower-right),
 * and a 16-position direction disc, each driving intvsession_pad_key/
 * intvsession_pad_disc directly.
 *
 * Every control is pressed with a raw GtkGestureClick (press/release), not
 * GtkButton's "clicked" -- a keypad digit is a real button on real
 * hardware, held for as long as the finger is down, and intvsession_pad_key
 * models exactly that (press/release, not a single pulse). The disc adds a
 * GtkEventControllerMotion so dragging around it while held recomputes the
 * direction continuously, the way rolling a thumb around a real disc does.
 *
 * FOCUS: this window forwards keyboard events to the session exactly like
 * the main window does (see forward_key below), rather than trying to
 * refuse focus outright -- GTK4 gives an application no reliable
 * cross-platform way to stop a toplevel from being focused on click, so
 * instead of fighting that, typing still drives the machine correctly
 * whichever window the window manager currently has focused.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#define _USE_MATH_DEFINES
#include <math.h>

#include "../keysym_map.h"

#define DISC_SIZE 150
#define DISC_DEADZONE_FRAC 0.22

typedef struct {
    GtkWidget *area;
    intvsession *session;
    intvsession_pad_side side;
    int direction;   /* -1 = centered, else 0-15 (0 = East, clockwise) */
    gboolean held;
} DiscState;

static GtkWindow *g_win;
static intvsession *g_session;

/* ---- disc ----------------------------------------------------------------
 * direction: matches intv_host.h's disc_codes numbering (0 = E, 4 = N,
 * 8 = W, 12 = S -- verified against the staged jzIntv tree's own
 * mapping.c, where the UP/DOWN/LEFT/RIGHT keys bind to PD0L_D_N/_S/_W/_E
 * respectively at exactly those indices).
 *
 * dy comes in as a screen-space delta (y increasing DOWNWARD), so a click
 * straight below centre has dy > 0. Standard atan2(dy,dx) treats increasing
 * y as counter-clockwise-toward-90-degrees in ordinary math convention,
 * which would place a downward click at the same angle a MATH-convention
 * upward click gets -- i.e. it comes out as North (index 4) instead of
 * South (index 12). Negating dy first (atan2(-dy,dx)) corrects for the
 * screen's flipped y-axis so a downward click actually yields South.
 *
 * Only ever returns one of the 8 EVEN positions (E/NE/N/NW/W/SW/S/SE),
 * matching intv_keymap.c's own DIR_* enum for the keyboard's arrow/IJKM
 * bindings and intv_disc_from_stick's own reasoning (core/src/
 * gamepad_sdl.c): the 8 odd half-step codes are unreachable from a
 * keyboard, and a mouse drag toward a cardinal direction sweeps through
 * several fine-grained angles on the way there, each of which used to be
 * written as its own disc position -- snapping to 8-way instead of 16-way
 * halves how many distinct (and spurious) positions a single drag can
 * pass through before settling. */
static int direction_from_point(double dx, double dy, double radius)
{
    double dist = sqrt(dx * dx + dy * dy);
    double angle_deg;
    int dir;

    if (dist < radius * DISC_DEADZONE_FRAC)
        return -1;

    angle_deg = atan2(-dy, dx) * 180.0 / M_PI;
    if (angle_deg < 0)
        angle_deg += 360.0;
    dir = (int)floor(angle_deg / 45.0 + 0.5) % 8;
    return dir * 2;
}

static void disc_set_direction(DiscState *d, int dir)
{
    if (d->direction == dir)
        return;
    d->direction = dir;
    intvsession_pad_disc(d->session, d->side, dir);
    gtk_widget_queue_draw(d->area);
}

static void disc_pressed(GtkGestureClick *g, int n_press, double x, double y,
                         gpointer user_data)
{
    DiscState *d = user_data;
    double r = DISC_SIZE / 2.0;
    (void)g;
    (void)n_press;
    d->held = TRUE;
    disc_set_direction(d, direction_from_point(x - r, y - r, r));
}

static void disc_released(GtkGestureClick *g, int n_press, double x, double y,
                          gpointer user_data)
{
    DiscState *d = user_data;
    (void)g;
    (void)n_press;
    (void)x;
    (void)y;
    d->held = FALSE;
    disc_set_direction(d, -1);
}

static void disc_motion(GtkEventControllerMotion *c, double x, double y,
                        gpointer user_data)
{
    DiscState *d = user_data;
    double r = DISC_SIZE / 2.0;
    (void)c;
    if (!d->held)
        return;
    disc_set_direction(d, direction_from_point(x - r, y - r, r));
}

static void disc_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                      gpointer user_data)
{
    DiscState *d = user_data;
    double cx = w / 2.0, cy = h / 2.0, r = (w < h ? w : h) / 2.0 - 4;
    int i;
    (void)area;

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);

    /* 8 wedges, one per reachable direction code (0,2,4,...,14), each
     * CENTERED on its compass direction and spanning exactly the 45-degree
     * sector that direction_from_point maps to it. direction_from_point
     * works in math convention (CCW, up = +90deg); cairo angles grow
     * CLOCKWISE in this y-down surface, so math angle `a` corresponds to
     * device angle `-a` here -- the radial endpoints are computed directly
     * as (cx + r*cos(a), cy - r*sin(a)) rather than handed to cairo_arc,
     * which would otherwise draw the mirror image. */
    for (i = 0; i < 8; i++) {
        int dir = i * 2;
        double a0 = (i * 45.0 - 22.5) * M_PI / 180.0;
        double a1 = (i * 45.0 + 22.5) * M_PI / 180.0;
        if (dir == d->direction) {
            cairo_set_source_rgb(cr, 0.30, 0.55, 0.90);
            cairo_move_to(cr, cx, cy);
            cairo_line_to(cr, cx + r * cos(a0), cy - r * sin(a0));
            cairo_arc_negative(cr, cx, cy, r, -a0, -a1);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.5);
    cairo_set_line_width(cr, 1.5);
    for (i = 0; i < 8; i++) {
        double a = (i * 45.0 - 22.5) * M_PI / 180.0;
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, cx + r * cos(a), cy - r * sin(a));
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, cx, cy, r * DISC_DEADZONE_FRAC, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.28);
    cairo_fill(cr);
}

static GtkWidget *make_disc(intvsession *session, intvsession_pad_side side)
{
    DiscState *d = g_new0(DiscState, 1);
    GtkGesture *click;
    GtkEventController *motion;

    d->session = session;
    d->side = side;
    d->direction = -1;
    d->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(d->area, DISC_SIZE, DISC_SIZE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), disc_draw, d,
                                   NULL);
    g_object_set_data_full(G_OBJECT(d->area), "disc-state", d, g_free);

    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(disc_pressed), d);
    g_signal_connect(click, "released", G_CALLBACK(disc_released), d);
    gtk_widget_add_controller(d->area, GTK_EVENT_CONTROLLER(click));

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(disc_motion), d);
    gtk_widget_add_controller(d->area, motion);

    return d->area;
}

/* ---- keys ------------------------------------------------------------- */

typedef struct {
    intvsession *session;
    intvsession_pad_side side;
    intvsession_key key;
} KeyBinding;

static void key_binding_free(gpointer p) { g_free(p); }

static void key_pressed(GtkGestureClick *g, int n_press, double x, double y,
                        gpointer user_data)
{
    KeyBinding *kb = user_data;
    (void)g;
    (void)n_press;
    (void)x;
    (void)y;
    intvsession_pad_key(kb->session, kb->side, kb->key, 1);
}

static void key_released(GtkGestureClick *g, int n_press, double x, double y,
                         gpointer user_data)
{
    KeyBinding *kb = user_data;
    (void)g;
    (void)n_press;
    (void)x;
    (void)y;
    intvsession_pad_key(kb->session, kb->side, kb->key, 0);
}

static GtkWidget *make_key(const char *label, intvsession *session,
                           intvsession_pad_side side, intvsession_key key)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    KeyBinding *kb = g_new0(KeyBinding, 1);
    GtkGesture *click;

    kb->session = session;
    kb->side = side;
    kb->key = key;
    g_object_set_data_full(G_OBJECT(btn), "key-binding", kb,
                           key_binding_free);

    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(key_pressed), kb);
    g_signal_connect(click, "released", G_CALLBACK(key_released), kb);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));

    gtk_widget_set_size_request(btn, 44, 40);
    return btn;
}

/* ---- one controller panel ------------------------------------------------ */

static GtkWidget *build_controller(intvsession *session,
                                   intvsession_pad_side side,
                                   const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *label = gtk_label_new(title);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const struct { const char *label; intvsession_key key; }
        digits[12] = {
            {"1", INTVSESSION_KEY_1}, {"2", INTVSESSION_KEY_2},
            {"3", INTVSESSION_KEY_3}, {"4", INTVSESSION_KEY_4},
            {"5", INTVSESSION_KEY_5}, {"6", INTVSESSION_KEY_6},
            {"7", INTVSESSION_KEY_7}, {"8", INTVSESSION_KEY_8},
            {"9", INTVSESSION_KEY_9}, {"Clear", INTVSESSION_KEY_CLEAR},
            {"0", INTVSESSION_KEY_0}, {"Enter", INTVSESSION_KEY_ENTER},
        };
    int i;

    gtk_widget_add_css_class(label, "heading");
    gtk_box_append(GTK_BOX(box), label);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    for (i = 0; i < 12; i++) {
        GtkWidget *k = make_key(digits[i].label, session, side, digits[i].key);
        gtk_grid_attach(GTK_GRID(grid), k, i % 3, i / 3, 1, 1);
    }
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), grid);

    gtk_box_append(GTK_BOX(action_row),
                   make_key("Top", session, side, INTVSESSION_ACTION_TOP));
    gtk_box_append(GTK_BOX(action_row),
                   make_key("L-Lower", session, side,
                           INTVSESSION_ACTION_LOWER_LEFT));
    gtk_box_append(GTK_BOX(action_row),
                   make_key("R-Lower", session, side,
                           INTVSESSION_ACTION_LOWER_RIGHT));
    gtk_widget_set_halign(action_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), action_row);

    {
        GtkWidget *disc = make_disc(session, side);
        gtk_widget_set_halign(disc, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), disc);
    }

    return box;
}

/* ---- keyboard passthrough (see file header) ----------------------------- */

static gboolean forward_key(guint keyval, GdkModifierType state, int down)
{
    intvsession_key_mapping m;
    (void)state;
    if (!g_session)
        return FALSE;
    m = intvsession_key_from_keysym(intv_keysym_from_gdk(keyval));
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(g_session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(g_session, m.side, down ? m.direction : -1);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    (void)c;
    (void)keycode;
    (void)user_data;
    return forward_key(keyval, state, 1);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)c;
    (void)keycode;
    (void)user_data;
    forward_key(keyval, state, 0);
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data)
{
    (void)user_data;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE; /* don't destroy -- this is a singleton, hide and reuse */
}

static void ensure_window(GtkWindow *parent, intvsession *session)
{
    GtkWidget *root, *header, *tb;
    GtkEventController *keys;

    if (g_win)
        return;

    g_session = session;
    g_win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(g_win, "Keypad");
    gtk_window_set_default_size(g_win, 560, 360);
    gtk_window_set_resizable(g_win, FALSE);
    gtk_window_set_transient_for(g_win, parent);
    /* Not modal: the main window keeps taking input while this is open. */
    gtk_window_set_modal(g_win, FALSE);

    root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_halign(root, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root),
                   build_controller(session, INTVSESSION_PAD_LEFT,
                                   "Left Controller"));
    gtk_box_append(GTK_BOX(root),
                   build_controller(session, INTVSESSION_PAD_RIGHT,
                                   "Right Controller"));

    header = adw_header_bar_new();
    tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), root);
    adw_window_set_content(ADW_WINDOW(g_win), tb);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), keys);

    g_signal_connect(g_win, "close-request",
                     G_CALLBACK(on_close_request), NULL);
}

void intv_keypad_window_toggle(GtkWindow *parent, intvsession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(GTK_WIDGET(g_win)))
        gtk_widget_set_visible(GTK_WIDGET(g_win), FALSE);
    else
        gtk_window_present(g_win);
}

gboolean intv_keypad_window_is_visible(void)
{
    return g_win != NULL && gtk_widget_get_visible(GTK_WIDGET(g_win));
}
