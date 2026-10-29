/*
 * IntvWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F9 (keypad
 * window), F10 (ECS keyboard window), F11 (fullscreen) and F12 (debugger)
 * to the Intellivision. No on-screen input panels are shown unless the
 * user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugger/dbg_window.h"
#include "display.h"
#include "ecskbd/ecskbd_window.h"
#include "keypad/keypad_window.h"
#include "keysym_map.h"
#include "prefs.h"

struct _IntvWindow {
    AdwApplicationWindow parent_instance;

    intvsession *session;
    IntvDisplay *display;
    GtkStack *stack;          /* the picture, or the No-BIOS page */
    AdwToastOverlay *toasts;
    guint shot_id;            /* INTV_SCREENSHOT timer */
    /* GTK4's key-pressed signal carries no repeat flag (unlike KDE/macOS,
     * which drop repeats before this code ever sees them) -- tracked by
     * hand so holding a sysaction key (e.g. Backspace) fires Reset Game
     * once on press, not ~60 times/sec for as long as it's held. Cleared on
     * focus loss (on_focus_leave) alongside the ECS matrix, so a key held
     * when focus moves elsewhere doesn't leave its slot stuck "down". */
    gboolean sysact_down[INTVSESSION_SYSACT_COUNT];
    guint sysact_timer_id;
};

G_DEFINE_FINAL_TYPE(IntvWindow, intv_window, ADW_TYPE_APPLICATION_WINDOW)

void intv_window_toast(IntvWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

/* ---- the display CSS ------------------------------------------------------
 * One display-wide provider, installed at startup so the keypad and ECS
 * windows get it too: checked toggles and highlighted buttons in the icon's
 * dark green (INTVSESSION_ACCENT_RGB) with white text, in light and dark. */

void intv_css_install(void)
{
    static gboolean done;
    GtkCssProvider *provider;
    g_autofree char *css = NULL;
    GdkDisplay *display = gdk_display_get_default();

    if (done || !display)
        return;
    done = TRUE;
    css = g_strdup_printf(
        "button.intv-toggle:checked, button.intv-accent,\n"
        "togglebutton.intv-toggle:checked {\n"
        "  background: #%06X; background-image: none; color: #ffffff;\n"
        "}\n"
        "button.intv-toggle:checked:hover, button.intv-accent:hover {\n"
        "  background: #2A6A19; background-image: none; color: #ffffff;\n"
        "}\n",
        (unsigned)INTVSESSION_ACCENT_RGB);
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void intv_window_message(IntvWindow *self, const char *heading,
                         const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

/* The picture while the machine runs or has ROMs to boot, else the No-BIOS
 * page. */
void intv_window_show_page(IntvWindow *self)
{
    gboolean ok = intvsession_is_running(self->session) ||
                  intvsession_has_system_roms(self->session);
    gtk_stack_set_visible_child_name(self->stack, ok ? "display" : "nobios");
}

void intv_window_restart_session(IntvWindow *self)
{
    intvsession_start_opts opts;
    intvsession_stop(self->session);
    if (intvsession_has_system_roms(self->session)) {
        intvsession_default_opts(self->session, &opts);
        if (intvsession_start(self->session, &opts) != 0)
            intv_window_toast(self, intvsession_last_error(self->session));
    }
    intv_window_show_page(self);
}

/* ---- file choosers -------------------------------------------------------- */

GtkFileDialog *intv_file_dialog(const char *title, const char *filter_name,
                                const char *const *exts)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *these = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    int i;

    gtk_file_filter_set_name(these, filter_name);
    for (i = 0; exts && exts[i]; i++)
        gtk_file_filter_add_suffix(these, exts[i]); /* any case */
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, these);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(these);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

/* ---- the BIOS ------------------------------------------------------------- */

static const char *const ROM_EXTS[] = {"zip", "rom", "bin", NULL};

typedef struct {
    IntvWindow *window;      /* strong ref */
    gboolean folder;
    void (*done)(gpointer);
    gpointer done_data;      /* strong ref'd by the caller if it needs one */
} BiosJob;

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all; the machine (re)starts if it now can -- a
 * running one is stopped and started again so a new ecs.bin is picked up. */
static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BiosJob *job = user_data;
    IntvWindow *self = job->window;
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GFile) dir = NULL;
    GString *log = g_string_new(NULL);
    int imported = 0;
    guint i;

    if (job->folder)
        dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res,
                                                   NULL);
    else
        files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res,
                                                     NULL);
    for (i = 0;; i++) {
        g_autoptr(GFile) f = NULL;
        g_autofree char *path = NULL;
        char msg[1024];
        if (job->folder) {
            if (i > 0 || !dir)
                break;
            f = g_object_ref(dir);
        } else {
            if (!files || i >= g_list_model_get_n_items(files))
                break;
            f = g_list_model_get_item(files, i);
        }
        path = g_file_get_path(f);
        if (!path)
            continue;
        msg[0] = '\0';
        if (intvsession_rom_import(self->session, path, msg, sizeof msg) == 0)
            imported++;
        g_string_append_printf(log, "%s%s", log->len ? "\n" : "", msg);
    }
    if (log->len) {
        intv_window_message(self, imported ? "BIOS Imported" : "Nothing Imported",
                            log->str);
        if (imported && intvsession_has_system_roms(self->session))
            intv_window_restart_session(self);
        else
            intv_window_show_page(self);
    }
    g_string_free(log, TRUE);
    if (job->done)
        job->done(job->done_data);
    g_object_unref(self);
    g_free(job);
}

void intv_window_import_bios(IntvWindow *self, gboolean folder,
                             void (*done)(gpointer), gpointer done_data)
{
    GtkFileDialog *dlg = intv_file_dialog(
        folder ? "Import BIOS from a Folder" : "Import BIOS",
        "BIOS images and ROM sets", ROM_EXTS);
    BiosJob *job = g_new0(BiosJob, 1);

    job->window = g_object_ref(self);
    job->folder = folder;
    job->done = done;
    job->done_data = done_data;
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL,
                                      on_bios_chosen, job);
    else
        gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL,
                                      on_bios_chosen, job);
    g_object_unref(dlg);
}

static void action_import_bios(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    (void)action;
    (void)param;
    intv_window_import_bios(INTV_WINDOW(user_data), FALSE, NULL, NULL);
}

static void action_import_bios_folder(GSimpleAction *action, GVariant *param,
                                      gpointer user_data)
{
    (void)action;
    (void)param;
    intv_window_import_bios(INTV_WINDOW(user_data), TRUE, NULL, NULL);
}

static void on_nobios_import(GtkButton *b, gpointer user_data)
{
    intv_window_import_bios(
        INTV_WINDOW(user_data),
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "intv-folder")), NULL,
        NULL);
}

/* In place of the picture until the machine has its ROMs. */
static GtkWidget *nobios_page(IntvWindow *self)
{
    GtkWidget *page = adw_status_page_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *files = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    GtkWidget *folder = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), intv_icon_name());
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "No Intellivision BIOS");
    adw_status_page_set_description(ADW_STATUS_PAGE(page),
        "The Intellivision Executive and Graphics ROMs are not part of the "
        "app. Import the ones you have \xe2\x80\x94 exec.bin and grom.bin, a "
        "zip such as MAME's intv set, or a folder \xe2\x80\x94 and the "
        "Intellivision starts.");
    gtk_widget_add_css_class(files, "pill");
    gtk_widget_add_css_class(files, "suggested-action");
    gtk_widget_add_css_class(folder, "pill");
    g_object_set_data(G_OBJECT(folder), "intv-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_nobios_import), self);
    g_signal_connect(folder, "clicked", G_CALLBACK(on_nobios_import), self);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), files);
    gtk_box_append(GTK_BOX(box), folder);
    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    return page;
}

/* ---- FujiNet's SD host ---------------------------------------------------- */

static const char *const CART_EXTS[] = {"rom", "bin", "int", "itv", "cfg",
                                        "zip", NULL};

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    IntvWindow *self = user_data;
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    guint n = files ? g_list_model_get_n_items(files) : 0, i;
    guint ok = 0;
    g_autofree char *first = NULL;
    g_autofree char *err = NULL;

    for (i = 0; i < n; i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (!path)
            continue;
        if (intvsession_import_cart_to_sd(self->session, path, dest,
                                          sizeof dest) == 0) {
            if (!ok)
                first = g_path_get_basename(dest);
            ok++;
        } else if (!err) {
            err = g_strdup(intvsession_last_error(self->session));
        }
    }
    if (err) {
        intv_window_message(self, "Import failed", err);
    } else if (ok == 1) {
        g_autofree char *msg = g_strdup_printf(
            "%s is on the SD host. Boot it from the CONFIG client.", first);
        intv_window_toast(self, msg);
    } else if (ok > 1) {
        g_autofree char *msg = g_strdup_printf(
            "%u files are on the SD host. Boot them from the CONFIG client.",
            ok);
        intv_window_toast(self, msg);
    }
    g_object_unref(self);
}

static void action_import_cart(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    GtkFileDialog *dlg = intv_file_dialog(
        "Import Cartridge to SD", "Cartridge images", CART_EXTS);
    (void)action;
    (void)param;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_cart_chosen,
                                  g_object_ref(self));
    g_object_unref(dlg);
}

static void action_open_sd(GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    const char *path = intvsession_sd_path(self->session);
    g_autoptr(GFile) dir = path ? g_file_new_for_path(path) : NULL;
    GtkFileLauncher *launcher;
    (void)action;
    (void)param;
    if (!dir)
        return;
    g_mkdir_with_parents(path, 0755);
    launcher = gtk_file_launcher_new(dir);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(launcher);
}

/* ---- keyboard capture ---------------------------------------------------
 * Everything goes to the machine except F9 (keypad window), F11
 * (fullscreen) and F12 (debugger). Both press and release are forwarded:
 * jzIntv's pad_t tracks held keys via a direct bit injection (see
 * core/jzintv/intv_host.h), so a missed release leaves that key down in the
 * machine. */

static gboolean forward_key(IntvWindow *self, GtkEventControllerKey *ctrl,
                            guint keyval, guint keycode,
                            GdkModifierType state, int down)
{
    uint32_t keysym = intv_keysym_from_key_event(ctrl, keyval, keycode, state);

    /* _bound, not the pure table: a keypad window "Map" remap has to take
     * effect here too, since typing while the main window holds focus is
     * the common case, not the exception. Resolved once, up front, so a
     * system-action target can be checked ahead of ECS keyboard mode
     * below. */
    intvsession_key_mapping m = intvsession_key_from_keysym_bound(self->session,
                                                                  keysym);

    /* System actions outrank ECS keyboard mode: Escape/Backspace default to
     * Reset to CONFIG/Reset Game, but both are also real ECS keys
     * (intv_keymap.c's ecs_key_from_keysym) -- ESC has to always be able to
     * get back to CONFIG regardless of which mode has the keyboard. Cost:
     * ESC/Backspace stop reaching the emulated ECS matrix from the HOST
     * keyboard while bound this way; the F10 on-screen ECS keyboard window
     * is unaffected (ecskbd_window.c calls intvsession_ecs_key_set
     * directly, ignoring keyboard_mode). GTK4 delivers no repeat flag on
     * key-pressed, so self->sysact_down gates this to the leading edge --
     * see that field's own comment. */
    if (m.kind == INTVSESSION_MAP_SYSACT) {
        if (down) {
            if (!self->sysact_down[m.sysact]) {
                self->sysact_down[m.sysact] = TRUE;
                intvsession_sysaction_fire(self->session, m.sysact);
            }
        } else {
            self->sysact_down[m.sysact] = FALSE;
        }
        return TRUE;
    }

    /* "ECS Keyboard" input mode (Preferences -> Input, or toggled live from
     * there) steals the host keyboard for the ECS's own keyboard instead of
     * the hand controllers -- the two can't both claim it at once. See
     * intvsession_ecs_key_from_keysym's own comment.
     *
     * The already-shifted keyval goes over as the character (the ECS's
     * shifted layer is nothing like a PC's -- '%' is SHIFT+LEFT-ARROW over
     * there -- so symbols only reach the machine by character), the
     * shift-stripped keysym as the fallback for keys that produce none, and
     * the hardware keycode as the stable id the release is looked up by.
     * See intvsession_ecs_key_event. */
    if (intvsession_get_int(self->session, "keyboard_mode", 0)) {
        intvsession_ecs_key_event(self->session, keycode, keysym,
                                  intv_char_from_keyval(keyval), down);
        return TRUE;
    }

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

    switch (keyval) {
    case GDK_KEY_F9:
        intv_keypad_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F10:
        intv_ecskbd_window_toggle(GTK_WINDOW(self), self->session);
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
    case GDK_KEY_r:
    case GDK_KEY_R:
        if (state & GDK_CONTROL_MASK) {
            gtk_widget_activate_action(GTK_WIDGET(self), "win.reset-config",
                                       NULL);
            return TRUE;
        }
        break;
    default:
        break;
    }

    return forward_key(self, controller, keyval, keycode, state, 1);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F10 ||
        keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return;
    if ((keyval == GDK_KEY_r || keyval == GDK_KEY_R) &&
        (state & GDK_CONTROL_MASK))
        return;
    forward_key(self, controller, keyval, keycode, state, 0);
}

/* Losing keyboard focus (alt-tab, another window raised) shouldn't leave a
 * key stuck down in whichever matrix was active -- see
 * intvsession_ecs_keys_clear's own comment. */
static void on_focus_leave(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    int i;
    (void)controller;
    intvsession_ecs_keys_clear(self->session);
    for (i = 0; i < INTVSESSION_SYSACT_COUNT; i++)
        self->sysact_down[i] = FALSE;
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

static void action_ecs_keyboard(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    intv_ecskbd_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_reset_config(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    if (intvsession_reset_to_config(self->session) != 0)
        intv_window_toast(self, intvsession_last_error(self->session));
    else
        intv_window_toast(self, "Reset to CONFIG");
}

static void action_reset_game(GSimpleAction *action, GVariant *param,
                              gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    if (intvsession_reset_game(self->session) != 0)
        intv_window_toast(self, intvsession_last_error(self->session));
    else
        intv_window_toast(self, "Reset Game");
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    GtkUriLauncher *launcher;
    (void)action;
    (void)param;
    if (intvsession_fujinet_external(self->session)) {
        intv_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
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

    if (intvsession_fujinet_external(self->session)) {
        intv_window_toast(self, "FujiNet is external: see its own console");
        return;
    }
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

static void action_preferences(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    (void)action;
    (void)param;
    intv_prefs_show(self, self->session);
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

/* Renders a widget the way GTK would draw it, with a renderer of its own
 * rather than the window's (on Broadway or a headless CI there is no
 * renderer that can read back), and saves it as a PNG. */
static gboolean save_widget_png(GtkWidget *widget, const char *path)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    GdkPaintable *paintable;
    GtkSnapshot *snapshot;
    GskRenderNode *node;
    GskRenderer *renderer;
    GdkTexture *texture;
    GError *err = NULL;
    gboolean ok;

    if (w <= 0 || h <= 0)
        return FALSE;
    paintable = gtk_widget_paintable_new(widget);
    snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, w, h);
    node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (!node)
        return FALSE;

    renderer = gsk_cairo_renderer_new();
#if GTK_CHECK_VERSION(4, 14, 0)
    ok = gsk_renderer_realize_for_display(renderer, gtk_widget_get_display(widget), &err);
#else
    ok = gsk_renderer_realize(renderer, NULL, &err);
#endif
    if (!ok) {
        g_warning("screenshot: %s", err ? err->message : "no renderer");
        g_clear_error(&err);
        g_object_unref(renderer);
        gsk_render_node_unref(node);
        return FALSE;
    }
    texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
    ok = gdk_texture_save_to_png(texture, path);
    g_object_unref(texture);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    gsk_render_node_unref(node);
    return ok;
}

/* The main window to <file>.png and every other visible window of the app
 * to <file>-<title>.png, then quit. */
static gboolean screenshot_tick(gpointer user_data)
{
    IntvWindow *self = user_data;
    const char *path = g_getenv("INTV_SCREENSHOT");
    g_autofree char *stem = NULL;
    GListModel *tops = gtk_window_get_toplevels();
    guint i, n = g_list_model_get_n_items(tops);

    self->shot_id = 0;
    stem = g_str_has_suffix(path, ".png") ? g_strndup(path, strlen(path) - 4) : g_strdup(path);
    for (i = 0; i < n; i++) {
        GtkWidget *win = g_list_model_get_item(tops, i);
        g_autofree char *file = NULL;
        if (!gtk_widget_get_visible(win)) {
            g_object_unref(win);
            continue;
        }
        if (win == GTK_WIDGET(self)) {
            file = g_strdup_printf("%s.png", stem);
        } else {
            const char *title = gtk_window_get_title(GTK_WINDOW(win));
            g_autofree char *slug = g_ascii_strdown(title ? title : "window", -1);
            g_strdelimit(slug, " /", '-');
            file = g_strdup_printf("%s-%s.png", stem, slug);
        }
        if (save_widget_png(win, file))
            g_print("screenshot: %s\n", file);
        else
            g_printerr("screenshot: could not save %s\n", file);
        g_object_unref(win);
    }
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

/* ---- construction ------------------------------------------------------- */

static const GActionEntry win_actions[] = {
    {.name = "keypad", .activate = action_keypad},
    {.name = "ecs-keyboard", .activate = action_ecs_keyboard},
    {.name = "import-bios", .activate = action_import_bios},
    {.name = "import-bios-folder", .activate = action_import_bios_folder},
    {.name = "import-cart", .activate = action_import_cart},
    {.name = "open-sd", .activate = action_open_sd},
    {.name = "reset-game", .activate = action_reset_game},
    {.name = "reset-config", .activate = action_reset_config},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "debugger", .activate = action_debugger},
    {.name = "preferences", .activate = action_preferences},
    {.name = "about", .activate = action_about},
};

static void intv_window_dispose(GObject *object)
{
    IntvWindow *self = INTV_WINDOW(object);
    if (self->sysact_timer_id) {
        g_source_remove(self->sysact_timer_id);
        self->sysact_timer_id = 0;
    }
    if (self->shot_id) {
        g_source_remove(self->shot_id);
        self->shot_id = 0;
    }
    G_OBJECT_CLASS(intv_window_parent_class)->dispose(object);
}

static void intv_window_class_init(IntvWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = intv_window_dispose;
}

static void intv_window_init(IntvWindow *self)
{
    (void)self;
}

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();

    g_menu_append(machine, "Import BIOS\xe2\x80\xa6", "win.import-bios");
    g_menu_append(machine, "Import BIOS from a Folder\xe2\x80\xa6",
                  "win.import-bios-folder");
    g_menu_append(machine, "Import Cartridge to SD\xe2\x80\xa6",
                  "win.import-cart");
    g_menu_append(machine, "Open SD Folder", "win.open-sd");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append(view, "Keypad (F9)", "win.keypad");
    g_menu_append(view, "ECS Keyboard (F10)", "win.ecs-keyboard");
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(fujinet, "Reset Game (Backspace)", "win.reset-game");
    g_menu_append(fujinet, "Reset to CONFIG (Ctrl+R)", "win.reset-config");
    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "About FujiNet Go Intv", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(view);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

/* Drains intvsession_sysaction_take -- the only caller that can't fire a
 * sysaction directly (gamepad_sdl.c's SDL thread, queuing RESET_CONFIG so it
 * never joins itself) -- and fires each on this, the UI thread. Lives on the
 * main window rather than the keypad window: the latter hides rather than
 * destroys itself (see keypad_window.c) but is hidden far more often than
 * shown, and a gamepad-bound sysaction has to keep working regardless of
 * whether that window happens to be open. ~100ms: fast enough that a
 * gamepad press feels immediate, cheap enough to run for the app's whole
 * lifetime. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    IntvWindow *self = INTV_WINDOW(user_data);
    intvsession_sysaction a;

    while (intvsession_sysaction_take(self->session, &a)) {
        if (intvsession_sysaction_fire(self->session, a) != 0)
            intv_window_toast(self, intvsession_last_error(self->session));
    }
    return G_SOURCE_CONTINUE;
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
    self->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(self->stack, GTK_WIDGET(self->display), "display");
    gtk_stack_add_named(self->stack, nobios_page(self), "nobios");
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->stack));

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

    {
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
        gtk_widget_add_controller(GTK_WIDGET(self), focus);
    }

    self->sysact_timer_id = g_timeout_add(100, sysact_drain_tick, self);

    intv_window_show_page(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance, matching the sibling repos: open the debugger
     * alongside the main window. */
    if (getenv("INTV_OPEN_DEBUGGER"))
        intv_debugger_show(GTK_WINDOW(self), session);
    if (getenv("INTV_OPEN_KEYPAD"))
        intv_keypad_window_toggle(GTK_WINDOW(self), session);
    if (getenv("INTV_OPEN_ECS"))
        intv_ecskbd_window_toggle(GTK_WINDOW(self), session);
    if (getenv("INTV_OPEN_SETTINGS"))
        intv_prefs_show(self, session);
    /* INTV_SCREENSHOT=<file.png>: every open window, saved after
     * INTV_SCREENSHOT_DELAY_MS (3 s), then quit. */
    {
        const char *env = g_getenv("INTV_SCREENSHOT");
        if (env && *env) {
            const char *delay = g_getenv("INTV_SCREENSHOT_DELAY_MS");
            self->shot_id = g_timeout_add(
                delay && *delay ? (guint)atoi(delay) : 3000, screenshot_tick,
                self);
        }
    }
    return GTK_WIDGET(self);
}
