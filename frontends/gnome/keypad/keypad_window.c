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
 * The disc offers all 16 positions the hardware has, marks all 16 sectors,
 * and lights exactly the one under the pointer. Hit testing is
 * intvsession_disc_from_point (core/src/disc_geom.c), shared with the KDE,
 * macOS and Windows keypad windows; the drawing below follows the same
 * step-by-step recipe they do, so the four discs look and behave alike.
 *
 * FOCUS: this window forwards keyboard events to the session exactly like
 * the main window does (see forward_key below), rather than trying to
 * refuse focus outright -- GTK4 gives an application no reliable
 * cross-platform way to stop a toplevel from being focused on click, so
 * instead of fighting that, typing still drives the machine correctly
 * whichever window the window manager currently has focused.
 *
 * MAP MODE: a bottom row (map_select_target and friends, near the end of
 * this file) lets a click on any of the 15 digit/action buttons above, any
 * of the 16 disc segments, OR either of the System row's two buttons
 * (Reset Game / Reset to CONFIG, build_system_row -- machine-global system
 * actions, not per-side), be rebound to a different keyboard key or gamepad
 * button, through core/src/bindings.c's remappable layer -- see
 * intvsession.h's own "remappable bindings" section for the contract. While
 * armed, the same GtkGestureClick press/release pair (or the System row's
 * plain "clicked", see build_system_row) that would normally inject a pad
 * key/disc direction/system action instead picks the map target
 * (key_pressed/key_released, disc_pressed/disc_released, and
 * on_sysact_clicked all check g_map_state first), and forward_key's own
 * keyboard capture (also gated on g_map_state) intercepts the next
 * keystroke instead of forwarding it to the machine. A gamepad button press
 * is polled for on a short GLib timeout (g_gamepad_poll below) since
 * core/src/gamepad_sdl.c's capture result lands on its own background
 * thread, not this one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#define _USE_MATH_DEFINES
#include <math.h>

#include "../keysym_map.h"

#define DISC_SIZE 150

/* Segments the highlighted sector's arc edge is drawn with -- see
 * disc_draw. */
#define DISC_WEDGE_STEPS 6

typedef struct {
    GtkWidget *area;
    intvsession *session;
    intvsession_pad_side side;
    int direction;   /* -1 = centered, else 0-15 (0 = East, clockwise) */
    int map_target;  /* -1 = not the map target, else 0-15: the sector Map
                      * mode has armed on THIS disc (see disc_set_map_target
                      * below) -- distinct from `direction`, which is the
                      * live position actually being sent to the machine. */
    gboolean held;
} DiscState;

static GtkWindow *g_win;
static intvsession *g_session;

/* ---- Map mode (see this file's own header) -------------------------------
 * g_key_buttons[side][key] lets map_select_target/map_complete_* find and
 * highlight the button for whichever (side,key) is currently the map
 * target, indexed directly by intvsession_pad_side/intvsession_key -- only
 * INTVSESSION_PAD_LEFT/_RIGHT are ever populated here, the two panels this
 * window builds. g_discs[side] is the disc widget's own DiscState* for the
 * same two sides, populated by make_disc below -- a direct pointer rather
 * than another g_object_get_data lookup, safe because the widget (and so
 * the state g_object_set_data_full ties its lifetime to) lives for the
 * whole process: this window is a singleton that hides rather than
 * destroys itself. */
typedef enum {
    INTV_MAP_IDLE = 0,        /* normal play */
    INTV_MAP_PICK_TARGET,     /* Map armed, waiting for a keypad button click */
    INTV_MAP_WAIT_INPUT,      /* target picked, waiting for a key/pad press */
} IntvMapState;

static IntvMapState g_map_state = INTV_MAP_IDLE;
static intvsession_key_mapping g_map_target;
static GtkWidget *g_map_btn;
static GtkWidget *g_status_label;
static GtkWidget *g_key_buttons[2][INTVSESSION_KEY_COUNT];
static GtkWidget *g_sysact_buttons[INTVSESSION_SYSACT_COUNT];
static DiscState *g_discs[2];
static guint g_gamepad_poll_id;

/* Highlights (or clears) whichever target g_map_target currently names,
 * MAP_KEY/MAP_DISC/MAP_SYSACT alike -- every set_highlighted(g_key_buttons
 * [...]) call site below that used to be scoped to a (side,key) pair now
 * goes through this instead, so Map mode doesn't need parallel code paths
 * per kind. */
static void map_highlight(gboolean on)
{
    if (g_map_target.kind == INTVSESSION_MAP_KEY) {
        GtkWidget *w = g_key_buttons[g_map_target.side][g_map_target.key];
        if (w) {
            if (on)
                gtk_widget_add_css_class(w, "suggested-action");
            else
                gtk_widget_remove_css_class(w, "suggested-action");
        }
    } else if (g_map_target.kind == INTVSESSION_MAP_DISC) {
        DiscState *d = g_discs[g_map_target.side];
        if (d) {
            d->map_target = on ? g_map_target.direction : -1;
            gtk_widget_queue_draw(d->area);
        }
    } else if (g_map_target.kind == INTVSESSION_MAP_SYSACT) {
        GtkWidget *w = g_sysact_buttons[g_map_target.sysact];
        if (w) {
            if (on)
                gtk_widget_add_css_class(w, "suggested-action");
            else
                gtk_widget_remove_css_class(w, "suggested-action");
        }
    }
}

/* "suggested-action" is libadwaita's own accent style class (used elsewhere
 * for e.g. dialog default buttons) -- reused here rather than defining a
 * custom CSS provider just for this one highlight. */
static void set_highlighted(GtkWidget *w, gboolean on)
{
    if (!w)
        return;
    if (on)
        gtk_widget_add_css_class(w, "suggested-action");
    else
        gtk_widget_remove_css_class(w, "suggested-action");
}

static void map_set_status(const char *text)
{
    if (g_status_label)
        gtk_label_set_text(GTK_LABEL(g_status_label), text ? text : "");
}

static gboolean gamepad_poll_tick(gpointer user_data)
{
    intvsession_pad_button button;
    (void)user_data;

    if (g_map_state != INTV_MAP_WAIT_INPUT)
        return G_SOURCE_REMOVE;
    if (!g_session || !intvsession_gamepad_capture_poll(g_session, &button))
        return G_SOURCE_CONTINUE;

    {
        char stolen[128], target[128], status[256];
        intvsession_target_set_button(g_session, g_map_target, button, stolen,
                                      sizeof(stolen));
        intvsession_target_name(g_map_target, target, sizeof(target));
        if (stolen[0])
            g_snprintf(status, sizeof(status),
                      "%s mapped to Gamepad %s (taken from %s)", target,
                      intvsession_pad_button_name(button), stolen);
        else
            g_snprintf(status, sizeof(status), "%s mapped to Gamepad %s",
                      target, intvsession_pad_button_name(button));
        map_highlight(FALSE);
        set_highlighted(g_map_btn, FALSE);
        g_map_state = INTV_MAP_IDLE;
        map_set_status(status);
    }
    g_gamepad_poll_id = 0;
    return G_SOURCE_REMOVE;
}

/* ~15Hz -- fast enough that a press feels immediate, cheap enough to leave
 * running for as long as a Map sequence is waiting on input. */
static void ensure_gamepad_poll(void)
{
    if (!g_gamepad_poll_id)
        g_gamepad_poll_id = g_timeout_add(66, gamepad_poll_tick, NULL);
}

/* Back to INTV_MAP_IDLE from any state, undoing whatever highlight is
 * showing and disarming gamepad capture -- shared by Map-to-abort, Reset,
 * and the window being hidden/closed mid-sequence. */
static void map_reset_ui(void)
{
    if (g_map_state == INTV_MAP_WAIT_INPUT)
        map_highlight(FALSE);
    set_highlighted(g_map_btn, FALSE);
    g_map_state = INTV_MAP_IDLE;
    map_set_status("");
    if (g_session)
        intvsession_gamepad_capture_cancel(g_session);
    if (g_gamepad_poll_id) {
        g_source_remove(g_gamepad_poll_id);
        g_gamepad_poll_id = 0;
    }
}

static void map_select_target(intvsession_key_mapping target)
{
    char desc[128], status[256];
    intvsession_binding b = intvsession_target_binding_get(g_session, target);

    g_map_target = target;
    g_map_state = INTV_MAP_WAIT_INPUT;
    map_highlight(TRUE);
    intvsession_binding_describe(b, desc, sizeof(desc));
    g_snprintf(status, sizeof(status),
              "Currently mapped to %s. Press target key or gamepad "
              "button, or press MAP to abort.",
              desc);
    map_set_status(status);
    intvsession_gamepad_capture_begin(g_session);
    ensure_gamepad_poll();
}

/* Called from forward_key when a keystroke arrives during
 * INTV_MAP_WAIT_INPUT -- completes the keyboard half of the pair the table
 * above (bindings.c) header describes. */
static void map_complete_key(uint32_t keysym)
{
    char stolen[128], namebuf[64], target[128], status[256];

    intvsession_target_set_key(g_session, g_map_target, keysym, stolen,
                               sizeof(stolen));
    intvsession_keysym_name(keysym, namebuf, sizeof(namebuf));
    intvsession_target_name(g_map_target, target, sizeof(target));
    if (stolen[0])
        g_snprintf(status, sizeof(status), "%s mapped to %s (taken from %s)",
                  target, namebuf, stolen);
    else
        g_snprintf(status, sizeof(status), "%s mapped to %s", target, namebuf);

    map_highlight(FALSE);
    set_highlighted(g_map_btn, FALSE);
    g_map_state = INTV_MAP_IDLE;
    intvsession_gamepad_capture_cancel(g_session);
    if (g_gamepad_poll_id) {
        g_source_remove(g_gamepad_poll_id);
        g_gamepad_poll_id = 0;
    }
    map_set_status(status);
}

static void on_map_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    if (g_map_state != INTV_MAP_IDLE) {
        map_reset_ui();
        return;
    }
    g_map_state = INTV_MAP_PICK_TARGET;
    set_highlighted(g_map_btn, TRUE);
    map_set_status("Press button to map");
}

static void on_reset_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    map_reset_ui();
    if (g_session) {
        intvsession_bindings_reset(g_session);
        map_set_status("Bindings reset to default");
    }
}

/* ---- disc ----------------------------------------------------------------
 * Hit testing lives in the core (intvsession_disc_from_point) so all four
 * frontends' discs agree on which sector a click belongs to, down to the
 * boundary; this file only has to hand it a screen-space offset from the
 * disc's centre and draw the answer. See intvsession.h for the numbering
 * (0 = E, 4 = N, 8 = W, 12 = S, odd codes the half-steps between) and for
 * why the pointer resolves all 16 positions where an analog stick resolves
 * only 8.
 *
 * Angles below are ordinary compass/math degrees (0 = East, growing
 * counter-clockwise) and are converted to this y-DOWN cairo surface at the
 * point of use, by disc_point: cos for x, MINUS sin for y. */
static void disc_point(double cx, double cy, double radius, double deg,
                       double *x, double *y)
{
    const double a = deg * M_PI / 180.0;
    *x = cx + radius * cos(a);
    *y = cy - radius * sin(a);
}

static void disc_set_direction(DiscState *d, int dir)
{
    if (d->direction == dir)
        return;
    d->direction = dir;
    intvsession_pad_disc(d->session, d->side, dir);
    gtk_widget_queue_draw(d->area);
}

/* Map mode's disc half: the same PICK_TARGET/WAIT_INPUT guard the keypad
 * buttons' key_pressed/key_released have (see this file's own header). A
 * click in the dead hub (intvsession_disc_from_point returning -1) picks
 * nothing and leaves Map mode armed, the same as clicking the gap between
 * two keypad buttons would. Never sets `held`, so disc_motion's own guard
 * below is only a defensive backstop, not load-bearing. */
static void disc_pressed(GtkGestureClick *g, int n_press, double x, double y,
                         gpointer user_data)
{
    DiscState *d = user_data;
    double r = DISC_SIZE / 2.0;
    (void)g;
    (void)n_press;
    if (g_map_state == INTV_MAP_PICK_TARGET) {
        int dir = intvsession_disc_from_point(x - r, y - r, r);
        if (dir >= 0)
            map_select_target(intvsession_target_disc(d->side, dir));
        return;
    }
    if (g_map_state == INTV_MAP_WAIT_INPUT)
        return;
    d->held = TRUE;
    disc_set_direction(d, intvsession_disc_from_point(x - r, y - r, r));
}

static void disc_released(GtkGestureClick *g, int n_press, double x, double y,
                          gpointer user_data)
{
    DiscState *d = user_data;
    (void)g;
    (void)n_press;
    (void)x;
    (void)y;
    if (g_map_state != INTV_MAP_IDLE)
        return;
    d->held = FALSE;
    disc_set_direction(d, -1);
}

static void disc_motion(GtkEventControllerMotion *c, double x, double y,
                        gpointer user_data)
{
    DiscState *d = user_data;
    double r = DISC_SIZE / 2.0;
    (void)c;
    if (g_map_state != INTV_MAP_IDLE)
        return;
    if (!d->held)
        return;
    disc_set_direction(d, intvsession_disc_from_point(x - r, y - r, r));
}

/* Fills the 22.5-degree wedge centred on `dir` (0-15) -- shared by the live
 * direction and the Map-mode target, which are drawn in different colours
 * at different points in disc_draw below. Drawn as a filled polygon sampled
 * along the arc rather than with cairo_arc: every toolkit in this project
 * has its own answer to which way an arc sweeps in a y-down surface
 * (cairo's grows clockwise here, Qt's counter-clockwise, GDI's is a
 * documentation argument), and each of those answers has been wrong in one
 * of these four files at some point. A polygon has no such convention. At
 * r ~= 71px a 3.75-degree chord bulges 0.04px inside the true arc --
 * invisible. Caller has already set the source colour. */
static void disc_fill_wedge(cairo_t *cr, double cx, double cy, double r,
                            int dir)
{
    const double centre_deg = dir * INTVSESSION_DISC_SECTOR_DEG;
    double x, y;
    int i;

    cairo_move_to(cr, cx, cy);
    for (i = 0; i <= DISC_WEDGE_STEPS; i++) {
        double a = centre_deg - INTVSESSION_DISC_SECTOR_DEG / 2.0 +
                   INTVSESSION_DISC_SECTOR_DEG * i / DISC_WEDGE_STEPS;
        disc_point(cx, cy, r, a, &x, &y);
        cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void disc_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                      gpointer user_data)
{
    DiscState *d = user_data;
    double cx = w / 2.0, cy = h / 2.0, r = (w < h ? w : h) / 2.0 - 4;
    double hub = r * INTVSESSION_DISC_DEADZONE_FRAC;
    double x, y;
    int i;
    (void)area;

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);

    /* The Map-mode target sector, if this disc has one armed -- drawn
     * before the live wedge so the live wedge (which Map mode's own press
     * guard prevents from ever landing on the same sector at the same
     * time, but the ordering is cheap insurance either way) stays on top. */
    if (d->map_target >= 0) {
        cairo_set_source_rgb(cr, 0.95, 0.65, 0.15);
        disc_fill_wedge(cr, cx, cy, r, d->map_target);
    }

    /* The held sector: exactly the 22.5 degrees centred on the direction
     * intvsession_disc_from_point returned, so the lit wedge is always the
     * one bounded by the two spokes the pointer is between -- never wider,
     * never mirrored. */
    if (d->direction >= 0) {
        cairo_set_source_rgb(cr, 0.30, 0.55, 0.90);
        disc_fill_wedge(cr, cx, cy, r, d->direction);
    }

    /* One spoke per sector boundary -- 16 of them, at the half-way angles
     * BETWEEN the 16 positions, so each position gets a visible sector of
     * its own to aim at. */
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.5);
    cairo_set_line_width(cr, 1.5);
    for (i = 0; i < INTVSESSION_DISC_POSITIONS; i++) {
        double a = i * INTVSESSION_DISC_SECTOR_DEG -
                   INTVSESSION_DISC_SECTOR_DEG / 2.0;
        disc_point(cx, cy, hub, a, &x, &y);
        cairo_move_to(cr, x, y);
        disc_point(cx, cy, r, a, &x, &y);
        cairo_line_to(cr, x, y);
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, cx, cy, hub, 0, 2 * M_PI);
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
    d->map_target = -1;
    d->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(d->area, DISC_SIZE, DISC_SIZE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), disc_draw, d,
                                   NULL);
    g_object_set_data_full(G_OBJECT(d->area), "disc-state", d, g_free);
    /* Only LEFT/RIGHT panels are ever built (see g_discs' own comment). */
    if (side == INTVSESSION_PAD_LEFT || side == INTVSESSION_PAD_RIGHT)
        g_discs[side] = d;

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

/* Both handlers check g_map_state first (see this file's own header):
 * INTV_MAP_PICK_TARGET turns a press into "select this button as the map
 * target" instead of an injection, and INTV_MAP_WAIT_INPUT swallows clicks
 * outright -- at that point the window wants a *keyboard or gamepad* press,
 * not another mouse click reinterpreted as one. */
static void key_pressed(GtkGestureClick *g, int n_press, double x, double y,
                        gpointer user_data)
{
    KeyBinding *kb = user_data;
    (void)g;
    (void)n_press;
    (void)x;
    (void)y;
    if (g_map_state == INTV_MAP_PICK_TARGET) {
        map_select_target(intvsession_target_key(kb->side, kb->key));
        return;
    }
    if (g_map_state == INTV_MAP_WAIT_INPUT)
        return;
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
    if (g_map_state != INTV_MAP_IDLE)
        return;
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
    g_key_buttons[side][key] = btn;
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

/* ---- system row (Reset Game / Reset to CONFIG) --------------------------
 * Two buttons, distinct from the Map row's own "Reset Bindings" -- these
 * fire a machine-global system action (core/src/bindings.c's MAP_SYSACT
 * target kind), and are themselves Map-mode targets like every keypad
 * digit/action button and disc segment. "clicked", not a press/release
 * gesture: unlike a keypad key, a sysaction isn't held. */
static void on_sysact_clicked(GtkButton *btn, gpointer user_data)
{
    intvsession_sysaction a = (intvsession_sysaction)GPOINTER_TO_INT(user_data);
    (void)btn;

    if (g_map_state == INTV_MAP_PICK_TARGET) {
        map_select_target(intvsession_target_sysaction(a));
        return;
    }
    if (g_map_state == INTV_MAP_WAIT_INPUT)
        return; /* waiting for a key/pad press, not another click */
    if (g_session && intvsession_sysaction_fire(g_session, a) != 0)
        map_set_status(intvsession_last_error(g_session));
}

static GtkWidget *build_system_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    int a;

    for (a = 0; a < INTVSESSION_SYSACT_COUNT; a++) {
        GtkWidget *btn = gtk_button_new_with_label(
            intvsession_sysaction_name((intvsession_sysaction)a));
        g_signal_connect(btn, "clicked", G_CALLBACK(on_sysact_clicked),
                         GINT_TO_POINTER(a));
        g_sysact_buttons[a] = btn;
        gtk_box_append(GTK_BOX(row), btn);
    }
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    return row;
}

/* ---- keyboard passthrough (see file header) ----------------------------- */

/* GTK4's key-pressed signal carries no repeat flag -- tracked by hand so
 * holding a sysaction key (e.g. Backspace) fires once, not ~60 times/sec
 * for as long as it's held. Mirrors window.c's own IntvWindow::sysact_down;
 * a file-static here rather than a struct field, matching every other piece
 * of this singleton window's state. */
static gboolean g_sysact_down[INTVSESSION_SYSACT_COUNT];

static gboolean forward_key(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state, int down)
{
    intvsession_key_mapping m;
    uint32_t keysym;

    if (!g_session)
        return FALSE;

    keysym = intv_keysym_from_key_event(c, keyval, keycode, state);

    /* Map mode's keyboard half: a press while waiting for input completes
     * the mapping instead of reaching the machine at all (see this file's
     * own header). Only the press matters -- ignore the matching release
     * the same way a mouse click's own release is swallowed in key_pressed/
     * _released above, so releasing the just-mapped key doesn't also get
     * forwarded as a live keystroke. */
    if (g_map_state == INTV_MAP_WAIT_INPUT) {
        if (down)
            map_complete_key(keysym);
        return TRUE;
    }
    if (g_map_state == INTV_MAP_PICK_TARGET)
        return TRUE; /* still swallow -- waiting for a button click, not this */

    /* _bound, not the pure table: a remap made here (or in any other
     * window) has to take effect here too. Resolved once, up front, so a
     * system-action target can be checked ahead of ECS keyboard mode
     * below -- see window.c's own forward_key for the full rationale on why
     * (mirrored here). */
    m = intvsession_key_from_keysym_bound(g_session, keysym);

    if (m.kind == INTVSESSION_MAP_SYSACT) {
        if (down) {
            if (!g_sysact_down[m.sysact]) {
                g_sysact_down[m.sysact] = TRUE;
                intvsession_sysaction_fire(g_session, m.sysact);
            }
        } else {
            g_sysact_down[m.sysact] = FALSE;
        }
        return TRUE;
    }

    /* Honour "ECS Keyboard" input mode here exactly as window.c's own
     * forward_key does. Without this branch, typing while THIS window holds
     * focus drove the hand controllers even with ECS keyboard mode on --
     * the same keystroke meant two different things depending on which of
     * the app's windows happened to be focused. */
    if (intvsession_get_int(g_session, "keyboard_mode", 0)) {
        intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
        if (key != INTVSESSION_ECS_KEY_NONE)
            intvsession_ecs_key_set(g_session, key, down);
        return TRUE;
    }

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
    (void)user_data;
    return forward_key(c, keyval, keycode, state, 1);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)user_data;
    forward_key(c, keyval, keycode, state, 0);
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data)
{
    int a;
    (void)user_data;
    /* A Map sequence left armed behind a hidden window would still be
     * capturing gamepad input on the next show -- abort it, same as
     * clicking Map again to cancel. */
    map_reset_ui();
    /* Same reasoning as window.c's own on_focus_leave: a sysaction key held
     * when this window loses focus/closes shouldn't leave its slot stuck
     * "down" and unable to fire again until some other window sees the
     * matching release. */
    for (a = 0; a < INTVSESSION_SYSACT_COUNT; a++)
        g_sysact_down[a] = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE; /* don't destroy -- this is a singleton, hide and reuse */
}

/* The Map/Reset row beneath both controller panels -- see this file's own
 * header for the state machine on_map_clicked/on_reset_clicked/
 * map_select_target/map_complete_* implement together. */
static GtkWidget *build_map_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    g_map_btn = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_btn, "clicked", G_CALLBACK(on_map_clicked), NULL);
    gtk_box_append(GTK_BOX(buttons), g_map_btn);

    {
        GtkWidget *reset_btn = gtk_button_new_with_label("Reset Bindings");
        g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked),
                         NULL);
        gtk_box_append(GTK_BOX(buttons), reset_btn);
    }
    gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), buttons);

    g_status_label = gtk_label_new("");
    gtk_widget_add_css_class(g_status_label, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(g_status_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(g_status_label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(row), g_status_label);

    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    return row;
}

static void ensure_window(GtkWindow *parent, intvsession *session)
{
    GtkWidget *outer, *panels, *header, *tb;
    GtkEventController *keys;

    if (g_win)
        return;

    g_session = session;
    g_win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(g_win, "Keypad");
    gtk_window_set_default_size(g_win, 560, 420);
    gtk_window_set_resizable(g_win, FALSE);
    gtk_window_set_transient_for(g_win, parent);
    /* Not modal: the main window keeps taking input while this is open. */
    gtk_window_set_modal(g_win, FALSE);

    panels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_halign(panels, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(panels),
                   build_controller(session, INTVSESSION_PAD_LEFT,
                                   "Left Controller"));
    gtk_box_append(GTK_BOX(panels),
                   build_controller(session, INTVSESSION_PAD_RIGHT,
                                   "Right Controller"));

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);
    gtk_widget_set_margin_top(outer, 12);
    gtk_widget_set_margin_bottom(outer, 16);
    gtk_box_append(GTK_BOX(outer), panels);
    gtk_box_append(GTK_BOX(outer), build_system_row());
    gtk_box_append(GTK_BOX(outer), build_map_row());

    header = adw_header_bar_new();
    tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), outer);
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
    if (gtk_widget_get_visible(GTK_WIDGET(g_win))) {
        map_reset_ui(); /* see on_close_request's own comment */
        gtk_widget_set_visible(GTK_WIDGET(g_win), FALSE);
    } else {
        gtk_window_present(g_win);
    }
}

gboolean intv_keypad_window_is_visible(void)
{
    return g_win != NULL && gtk_widget_get_visible(GTK_WIDGET(g_win));
}
