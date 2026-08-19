/*
 * IntvEcsKeyboardWindow -- an on-screen ECS keyboard: 48 buttons covering
 * every key of the ECS's own 7x8 scan-matrix keyboard (see
 * core/jzintv/intv_host.h's intv_ecs_key), each driving
 * intvsession_ecs_key_set directly. Independent of the "keyboard_mode"
 * setting (window.c's forward_key) -- these on-screen buttons work
 * whether or not the physical keyboard is currently routed to the ECS, so
 * opening this window never silently changes that setting.
 *
 * Every ordinary key is pressed with a raw GtkGestureClick (press/release),
 * not GtkButton's "clicked" -- same reasoning as keypad_window.c: a real
 * ECS key is held for as long as it's down, and intvsession_ecs_key_set
 * models exactly that. Shift and Ctrl are GtkToggleButtons instead: a
 * mouse can't hold two buttons down to chord a Shifted letter the way a
 * hand can, so they latch, and intv_host_ecs_key's OR-in-a-bit chording
 * (core/jzintv/intv_host.c) does the rest.
 *
 * FOCUS: forwards keyboard events to the ECS the same way the keypad
 * window forwards to the hand controllers (see that file's own FOCUS
 * note) -- GTK4 has no reliable way to refuse focus outright, so typing
 * while this window has it still drives the machine correctly.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ecskbd_window.h"

#include "../keysym_map.h"

static GtkWindow *g_win;
static intvsession *g_session;
static GtkWidget *g_notice;

/* ---- ordinary keys ------------------------------------------------------ */

typedef struct {
    intvsession *session;
    intvsession_ecs_key key;
} KeyBinding;

static void key_binding_free(gpointer p) { g_free(p); }

static void key_pressed(GtkGestureClick *g, int n_press, double x, double y,
                        gpointer user_data)
{
    KeyBinding *kb = user_data;
    (void)g; (void)n_press; (void)x; (void)y;
    intvsession_ecs_key_set(kb->session, kb->key, 1);
}

static void key_released(GtkGestureClick *g, int n_press, double x, double y,
                         gpointer user_data)
{
    KeyBinding *kb = user_data;
    (void)g; (void)n_press; (void)x; (void)y;
    intvsession_ecs_key_set(kb->session, kb->key, 0);
}

static GtkWidget *make_key(const char *label, intvsession *session,
                           intvsession_ecs_key key, int width)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    KeyBinding *kb = g_new0(KeyBinding, 1);
    GtkGesture *click;

    kb->session = session;
    kb->key = key;
    g_object_set_data_full(G_OBJECT(btn), "key-binding", kb,
                           key_binding_free);

    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(key_pressed), kb);
    g_signal_connect(click, "released", G_CALLBACK(key_released), kb);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));

    gtk_widget_set_size_request(btn, width, 40);
    return btn;
}

/* ---- Shift / Ctrl (latching -- see file header) -------------------------- */

typedef struct {
    intvsession *session;
    intvsession_ecs_key key;
} ModBinding;

static void mod_toggled(GtkToggleButton *btn, gpointer user_data)
{
    ModBinding *m = user_data;
    intvsession_ecs_key_set(m->session, m->key,
                            gtk_toggle_button_get_active(btn));
}

static GtkWidget *make_mod_key(const char *label, intvsession *session,
                               intvsession_ecs_key key)
{
    GtkWidget *btn = gtk_toggle_button_new_with_label(label);
    ModBinding *m = g_new0(ModBinding, 1);

    m->session = session;
    m->key = key;
    g_object_set_data_full(G_OBJECT(btn), "mod-binding", m, g_free);
    g_signal_connect(btn, "toggled", G_CALLBACK(mod_toggled), m);
    gtk_widget_set_size_request(btn, 64, 40);
    return btn;
}

/* ---- layout --------------------------------------------------------------
 * Four QWERTY-ish rows plus a function row, covering all 48 keys of
 * intv_ecs_key exactly once. See core/jzintv/intv_host.h for the row/mask
 * this ultimately maps to. */

typedef struct { const char *label; intvsession_ecs_key key; } EcsKeyLabel;

static const EcsKeyLabel row_digits[10] = {
    {"1", INTVSESSION_ECS_KEY_1}, {"2", INTVSESSION_ECS_KEY_2},
    {"3", INTVSESSION_ECS_KEY_3}, {"4", INTVSESSION_ECS_KEY_4},
    {"5", INTVSESSION_ECS_KEY_5}, {"6", INTVSESSION_ECS_KEY_6},
    {"7", INTVSESSION_ECS_KEY_7}, {"8", INTVSESSION_ECS_KEY_8},
    {"9", INTVSESSION_ECS_KEY_9}, {"0", INTVSESSION_ECS_KEY_0},
};

static const EcsKeyLabel row_qwerty[10] = {
    {"Q", INTVSESSION_ECS_KEY_Q}, {"W", INTVSESSION_ECS_KEY_W},
    {"E", INTVSESSION_ECS_KEY_E}, {"R", INTVSESSION_ECS_KEY_R},
    {"T", INTVSESSION_ECS_KEY_T}, {"Y", INTVSESSION_ECS_KEY_Y},
    {"U", INTVSESSION_ECS_KEY_U}, {"I", INTVSESSION_ECS_KEY_I},
    {"O", INTVSESSION_ECS_KEY_O}, {"P", INTVSESSION_ECS_KEY_P},
};

static const EcsKeyLabel row_asdf[10] = {
    {"A", INTVSESSION_ECS_KEY_A}, {"S", INTVSESSION_ECS_KEY_S},
    {"D", INTVSESSION_ECS_KEY_D}, {"F", INTVSESSION_ECS_KEY_F},
    {"G", INTVSESSION_ECS_KEY_G}, {"H", INTVSESSION_ECS_KEY_H},
    {"J", INTVSESSION_ECS_KEY_J}, {"K", INTVSESSION_ECS_KEY_K},
    {"L", INTVSESSION_ECS_KEY_L}, {";", INTVSESSION_ECS_KEY_SEMI},
};

static const EcsKeyLabel row_zxcv[10] = {
    {"Z", INTVSESSION_ECS_KEY_Z}, {"X", INTVSESSION_ECS_KEY_X},
    {"C", INTVSESSION_ECS_KEY_C}, {"V", INTVSESSION_ECS_KEY_V},
    {"B", INTVSESSION_ECS_KEY_B}, {"N", INTVSESSION_ECS_KEY_N},
    {"M", INTVSESSION_ECS_KEY_M}, {",", INTVSESSION_ECS_KEY_COMMA},
    {".", INTVSESSION_ECS_KEY_PERIOD}, {"←", INTVSESSION_ECS_KEY_LEFT},
};

static void add_row(GtkWidget *box, const EcsKeyLabel *row,
                    int count, intvsession *session)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    int i;
    for (i = 0; i < count; i++)
        gtk_box_append(GTK_BOX(hbox), make_key(row[i].label, session,
                                              row[i].key, 40));
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), hbox);
}

static GtkWidget *build_keyboard(intvsession *session)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    add_row(box, row_digits, 10, session);
    add_row(box, row_qwerty, 10, session);
    add_row(box, row_asdf, 10, session);
    add_row(box, row_zxcv, 10, session);

    {
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append(GTK_BOX(hbox), make_key("Esc", session,
                                              INTVSESSION_ECS_KEY_ESC, 56));
        gtk_box_append(GTK_BOX(hbox),
                       make_mod_key("Ctrl", session, INTVSESSION_ECS_KEY_CTRL));
        gtk_box_append(GTK_BOX(hbox),
                       make_mod_key("Shift", session,
                                   INTVSESSION_ECS_KEY_SHIFT));
        gtk_box_append(GTK_BOX(hbox), make_key("Space", session,
                                              INTVSESSION_ECS_KEY_SPACE, 160));
        gtk_box_append(GTK_BOX(hbox), make_key("↑", session,
                                              INTVSESSION_ECS_KEY_UP, 40));
        gtk_box_append(GTK_BOX(hbox), make_key("↓", session,
                                              INTVSESSION_ECS_KEY_DOWN, 40));
        gtk_box_append(GTK_BOX(hbox), make_key("→", session,
                                              INTVSESSION_ECS_KEY_RIGHT, 40));
        gtk_box_append(GTK_BOX(hbox), make_key("Enter", session,
                                              INTVSESSION_ECS_KEY_ENTER, 56));
        gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), hbox);
    }

    return box;
}

/* ---- keyboard passthrough (see file header) ----------------------------- */

static gboolean forward_key(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state, int down)
{
    intvsession_ecs_key key;
    if (!g_session)
        return FALSE;
    key = intvsession_ecs_key_from_keysym(
        intv_keysym_from_key_event(c, keyval, keycode, state));
    if (key != INTVSESSION_ECS_KEY_NONE)
        intvsession_ecs_key_set(g_session, key, down);
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

/* Losing focus, or the window closing, must not leave a key stuck down in
 * the emulated matrix -- see intvsession_ecs_keys_clear's own comment. */
static void release_all(void)
{
    if (g_session)
        intvsession_ecs_keys_clear(g_session);
}

static void on_focus_leave(GtkEventControllerFocus *c, gpointer user_data)
{
    (void)c; (void)user_data;
    release_all();
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data)
{
    (void)user_data;
    release_all();
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE; /* don't destroy -- this is a singleton, hide and reuse */
}

static void ensure_window(GtkWindow *parent, intvsession *session)
{
    GtkWidget *root, *header, *tb;
    GtkEventController *keys, *focus;

    if (g_win)
        return;

    g_session = session;
    g_win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(g_win, "ECS Keyboard");
    gtk_window_set_default_size(g_win, 640, 260);
    gtk_window_set_resizable(g_win, FALSE);
    gtk_window_set_transient_for(g_win, parent);
    gtk_window_set_modal(g_win, FALSE);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 16);

    g_notice = gtk_label_new(
        "ECS is not enabled -- see Preferences to turn it on.");
    gtk_widget_add_css_class(g_notice, "dim-label");
    gtk_widget_set_visible(g_notice, !intvsession_has_ecs_rom(session) ||
                          intvsession_get_int(session, "ecs",
                                              INTVSESSION_HW_AUTO) ==
                              INTVSESSION_HW_OFF);
    gtk_box_append(GTK_BOX(root), g_notice);

    gtk_box_append(GTK_BOX(root), build_keyboard(session));

    header = adw_header_bar_new();
    tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), root);
    adw_window_set_content(ADW_WINDOW(g_win), tb);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(g_win), focus);

    g_signal_connect(g_win, "close-request",
                     G_CALLBACK(on_close_request), NULL);
}

void intv_ecskbd_window_toggle(GtkWindow *parent, intvsession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(GTK_WIDGET(g_win))) {
        gtk_widget_set_visible(GTK_WIDGET(g_win), FALSE);
        release_all();
    } else {
        gtk_widget_set_visible(
            g_notice, !intvsession_has_ecs_rom(session) ||
                     intvsession_get_int(session, "ecs",
                                         INTVSESSION_HW_AUTO) ==
                         INTVSESSION_HW_OFF);
        gtk_window_present(g_win);
    }
}

gboolean intv_ecskbd_window_is_visible(void)
{
    return g_win != NULL && gtk_widget_get_visible(GTK_WIDGET(g_win));
}
