/*
 * IntvWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F9 (keypad
 * window), F11 (fullscreen) and F12 (debugger) to the Intellivision. No
 * on-screen input panels are shown unless the user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>
#include <string.h>

#include "debugger/dbg_window.h"
#include "display.h"
#include "keypad/keypad_window.h"
#include "keysym_map.h"

struct _IntvWindow {
    AdwApplicationWindow parent_instance;

    intvsession *session;
    IntvDisplay *display;
    AdwToastOverlay *toasts;
};

G_DEFINE_FINAL_TYPE(IntvWindow, intv_window, ADW_TYPE_APPLICATION_WINDOW)

void intv_window_toast(IntvWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

/* ---- keyboard capture ---------------------------------------------------
 * Everything goes to the machine except F9 (keypad window), F11
 * (fullscreen) and F12 (debugger). Both press and release are forwarded:
 * jzIntv's pad_t tracks held keys via a direct bit injection (see
 * core/jzintv/intv_host.h), so a missed release leaves that key down in the
 * machine. */

static gboolean forward_key(IntvWindow *self, guint keyval,
                            GdkModifierType state, int down)
{
    intvsession_key_mapping m =
        intvsession_key_from_keysym(intv_keysym_from_gdk(keyval));
    (void)state;
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(self->session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(self->session, m.side, down ? m.direction : -1);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)controller;
    (void)keycode;

    switch (keyval) {
    case GDK_KEY_F9:
        intv_keypad_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.debugger", NULL);
        return TRUE;
    default:
        break;
    }

    return forward_key(self, keyval, state, 1);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)controller;
    (void)keycode;
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return;
    forward_key(self, keyval, state, 0);
}

/* ---- actions ------------------------------------------------------------ */

static void action_keypad(GSimpleAction *action, GVariant *param,
                          gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    intv_keypad_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    GtkUriLauncher *launcher;
    (void)action;
    (void)param;
    /* No embedded WebKitGTK view yet -- open the system browser. */
    launcher =
        gtk_uri_launcher_new(intvsession_fujinet_webui_url(self->session));
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(launcher);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    char buf[16384];
    GtkWidget *dialog, *scroll, *view;
    GtkTextBuffer *tbuf;
    (void)action;
    (void)param;

    intvsession_fujinet_copy_log(self->session, buf, sizeof(buf));

    dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "FujiNet Console Log");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self));

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(tbuf, buf, -1);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(GTK_WINDOW(dialog), scroll);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    intv_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go Intv",
        "application-icon", intv_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", INTV_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Mattel Intellivision with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

static const GActionEntry win_actions[] = {
    {.name = "keypad", .activate = action_keypad},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "debugger", .activate = action_debugger},
    {.name = "about", .activate = action_about},
};

static void intv_window_class_init(IntvWindowClass *klass)
{
    (void)klass;
}

static void intv_window_init(IntvWindow *self)
{
    (void)self;
}

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();

    g_menu_append(view, "Keypad (F9)", "win.keypad");
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "About FujiNet Go Intv", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(view);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

GtkWidget *intv_window_new(AdwApplication *app, intvsession *session)
{
    IntvWindow *self = g_object_new(INTV_TYPE_WINDOW,
                                    "application", app,
                                    "title", "FujiNet Go Intv",
                                    /* 4:3 content area plus room for the
                                     * header bar -- matches the display's
                                     * own letterbox aspect (display.c)
                                     * instead of the raw 160x200 frame
                                     * buffer's portrait shape, so the
                                     * window doesn't open with big black
                                     * bars top and bottom. */
                                    "default-width", 800,
                                    "default-height", 650,
                                    NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkMenuButton *menu_button;
    GMenu *menu;
    GtkEventController *keys;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    header = ADW_HEADER_BAR(adw_header_bar_new());
    menu = build_menu();
    menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(menu_button, "open-menu-symbolic");
    gtk_menu_button_set_menu_model(menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(menu_button));

    self->display = INTV_DISPLAY(intv_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance, matching the sibling repos: open the debugger
     * alongside the main window. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        intv_debugger_show(GTK_WINDOW(self), session);
    if (getenv("INTV_OPEN_KEYPAD"))
        intv_keypad_window_toggle(GTK_WINDOW(self), session);
    return GTK_WIDGET(self);
}
