/*
 * IntvPrefs -- Settings/Preferences dialog. Programmatic AdwPreferencesDialog
 * (no .ui file, no GResource -- nothing in this frontend is declarative),
 * transposed from fujinet-go-coco-desktop's frontends/gnome/prefs.c, the
 * newest and most complete settings dialog in the FujiNet Go Desktop family.
 * The macOS, KDE and Windows Settings windows in this repo cite this file's
 * key/default list as their own reference.
 *
 * Settings keys (shared INI, core/src/settings.c -- same store every
 * frontend in this app reads/writes):
 *   "ecs"            INTVSESSION_HW_* (default AUTO)  -- restart to apply
 *   "ivoice"         INTVSESSION_HW_* (default AUTO)  -- restart to apply
 *   "video_standard" INTVSESSION_VIDEO_* (default NTSC) -- restart to apply
 *   "keyboard_mode"  0 = hand controllers, 1 = ECS keyboard -- applies live
 *   "enable_gamepad" 0/1 (default 1) -- restart to apply
 *
 * Every row is one of three apply classes (RowBinding.apply below), the
 * same three-way split CoCo's prefs.c introduced: APPLY_LIVE pushes
 * straight into the running machine and persists immediately (no restart);
 * APPLY_ON_CLOSE persists immediately but only takes effect the next time
 * the session starts, so this dialog collects a "dirty" flag and restarts
 * once when it closes rather than once per row changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <string.h>

#include "window.h"
#include "controllers/controllers_window.h"
#include "help/help_window.h"

#define MAX_ROM_ROWS 8
#define MAX_PADS 8

typedef struct {
    IntvWindow *window;
    intvsession *session;
    gboolean session_dirty;
    GtkWidget *ecs_row;
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    AdwPreferencesDialog *dialog;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[MAX_PADS];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    enum { APPLY_LIVE, APPLY_ON_CLOSE } apply;
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def,
                               int apply)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->apply = apply;
    return b;
}

/* Materialises a NULL-terminated name(idx) table into a GtkStringList's
 * backing array -- so adding a new Auto/Off/On or NTSC/PAL style option in
 * intvsession.h's own table is the only place it has to be added. */
#define OPTION_TABLE(fn, storage)                                     \
    static const char *const *storage##_table(void)                  \
    {                                                                 \
        static const char *names[8];                                 \
        int i;                                                       \
        for (i = 0; i < (int)G_N_ELEMENTS(names) - 1; i++) {          \
            names[i] = fn(i);                                        \
            if (!names[i]) break;                                    \
        }                                                            \
        names[i] = NULL;                                             \
        return names;                                                \
    }

OPTION_TABLE(intvsession_hw_mode_name, hw_mode)
OPTION_TABLE(intvsession_video_name, video)

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;

    if (intvsession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    intvsession_set_int(b->state->session, b->key, sel);
    if (b->apply == APPLY_ON_CLOSE)
        b->state->session_dirty = TRUE;
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *const *names, int apply)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    RowBinding *b = binding_new(state, key, def, apply);
    int cur = intvsession_get_int(state->session, key, def);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)cur);
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          b, binding_free, 0);
    g_object_unref(model);
    return row;
}

static void switch_toggled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    (void)pspec;

    if (intvsession_get_int(b->state->session, b->key, b->def) == (on ? 1 : 0))
        return;
    intvsession_set_int(b->state->session, b->key, on ? 1 : 0);
    if (b->apply == APPLY_ON_CLOSE)
        b->state->session_dirty = TRUE;
    else if (!on)
        /* Leaving ECS keyboard mode -- release anything still held so a key
         * down at the moment of the switch doesn't stay stuck in the
         * emulated matrix (see intvsession_ecs_keys_clear's own comment). */
        intvsession_ecs_keys_clear(b->state->session);
}

#define EXTERNAL_PLACEHOLDER "127.0.0.1:65503"

/* The address field: valid text (or none, meaning 127.0.0.1:65503) is saved
 * and marks the session for a restart; anything else is flagged and not
 * saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (intvsession_parse_hostport(text, host, sizeof(host), &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(intvsession_get_str(state->session, "fujinet_external_addr",
                                      ""), text) == 0)
        return;
    intvsession_set_str(state->session, "fujinet_external_addr", text);
    state->session_dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec,
                             gpointer user_data)
{
    gtk_widget_set_sensitive(
        GTK_WIDGET(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int apply)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, key, def, apply);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              intvsession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_toggled),
                          b, binding_free, 0);
    return row;
}

/* ---- the ROMs page --------------------------------------------------------- */

static void ecs_row_update(PrefsState *st)
{
    gboolean have = intvsession_has_ecs_rom(st->session);
    if (!st->ecs_row)
        return;
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(st->ecs_row),
        have ? "Entertainment Computer System -- second controller pair, "
               "ECS keyboard, extra sound"
             : "Unavailable: ecs.bin not found in the ROM directory");
    gtk_widget_set_sensitive(st->ecs_row, have);
}

static void refresh_rows(PrefsState *st)
{
    int r;
    for (r = 0; r < intvsession_rom_item_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r])
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(st->rom_rows[r]),
                intvsession_rom_item_available(st->session, r)
                    ? "Imported" : "Not imported");
    ecs_row_update(st);
}

/* The dialog owns the state, so it is still there when an import started
 * from it finishes after it was closed. */
static void import_done(gpointer dialog)
{
    PrefsState *st = g_object_get_data(G_OBJECT(dialog), "intv-prefs");
    if (st)
        refresh_rows(st);
    g_object_unref(dialog);
}

static void on_rom_import(GtkButton *b, gpointer user_data)
{
    GObject *dialog = user_data;
    PrefsState *st = g_object_get_data(dialog, "intv-prefs");
    gboolean folder =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "intv-folder"));
    intv_window_import_bios(st->window, folder, import_done,
                            g_object_ref(dialog));
}

static AdwPreferencesPage *build_roms_page(PrefsState *st, GObject *dialog)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    char desc[1200];
    int r;

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");
    for (r = 0; r < intvsession_rom_item_count() && r < MAX_ROM_ROWS; r++) {
        const intv_rom_item *item = intvsession_rom_item(r);
        GtkWidget *row;
        if (!last || strcmp(last, item->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, item->group);
            if (!last) {
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget *files = gtk_button_new_with_label("Import\xe2\x80\xa6");
                GtkWidget *dir =
                    gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
                g_snprintf(desc, sizeof desc,
                    "The Intellivision's system ROMs are not part of the "
                    "app. Import them from files, a zip or a folder (MAME's "
                    "intv set works); they are recognised by their "
                    "checksums. Kept in %s.",
                    intvsession_roms_path(st->session));
                adw_preferences_group_set_description(group, desc);
                g_object_set_data(G_OBJECT(dir), "intv-folder",
                                  GINT_TO_POINTER(1));
                g_signal_connect(files, "clicked", G_CALLBACK(on_rom_import),
                                 dialog);
                g_signal_connect(dir, "clicked", G_CALLBACK(on_rom_import),
                                 dialog);
                gtk_widget_set_valign(files, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(dir, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(box), files);
                gtk_box_append(GTK_BOX(box), dir);
                gtk_box_append(GTK_BOX(box), intv_help_button("prefs/ROMs"));
                adw_preferences_group_set_header_suffix(group, box);
            }
            adw_preferences_page_add(page, group);
            last = item->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->name);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
    }
    return page;
}

/* ---- gamepads: one row per pad, with the controller it drives ---------------- */

static const char *const k_side_names[4] = { "Left", "Right", "ECS Left", "ECS Right" };

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    /* 0 = automatic, then Left, Right and, with the ECS on, its pair */
    intvsession_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    const gboolean ecs = intvsession_ecs_on(state->session) != 0;
    const char *choices[6] = { "Automatic", "Left", "Right", NULL, NULL, NULL };
    int idx, count = intvsession_gamepad_count(state->session);
    char name[128], sub[64];

    if (ecs) {
        choices[3] = "ECS Left";
        choices[4] = "ECS Right";
    }
    for (idx = 0; idx < state->pad_row_count; idx++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[idx]);
    state->pad_row_count = 0;

    if (count == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No gamepads connected");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        const int effective = intvsession_gamepad_effective_side(state->session, idx);
        const int assign = intvsession_gamepad_assignment(state->session, idx);
        intvsession_gamepad_name(state->session, idx, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (effective >= 0 && effective < 4)
            g_snprintf(sub, sizeof sub, "Driving the %s controller", k_side_names[effective]);
        else
            g_snprintf(sub, sizeof sub, "Driving nothing");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                   assign < 0 || assign + 1 >= (ecs ? 5 : 3) ? 0 : (guint)assign + 1);
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(idx));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned generation = intvsession_gamepad_generation(state->session);
    if (generation != state->pad_generation) {
        state->pad_generation = generation;
        rebuild_pad_rows(state);
    }
    return G_SOURCE_CONTINUE;
}

static void on_open_controllers(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    intv_controllers_window_show(GTK_WINDOW(state->window), state->session);
}

/* Every key and gamepad button back to its default, and every gamepad back to
 * automatic assignment. */
static void on_reset_controls(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx;
    (void)button;
    intvsession_bindings_reset(state->session);
    for (idx = 0; idx < intvsession_gamepad_count(state->session); idx++)
        intvsession_gamepad_assign(state->session, idx, -1);
    rebuild_pad_rows(state);
    intv_controllers_window_refresh();
    adw_preferences_dialog_add_toast(
        state->dialog,
        adw_toast_new("Controls back to their defaults; gamepads on automatic"));
}

/* A row with a title, a line under it and a button. */
static GtkWidget *button_row(const char *title, const char *subtitle, const char *label,
                             GCallback callback, gpointer data)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *button = gtk_button_new_with_label(label);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", callback, data);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
    return row;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer)
        g_source_remove(state->pad_timer);
    state->pad_timer = 0;
    if (state->session_dirty) {
        intv_window_restart_session(state->window);
        intv_window_toast(state->window,
                          "Machine options applied (session restarted)");
    }
}

void intv_prefs_show(IntvWindow *parent, intvsession *session)
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesPage *input = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *keyboard =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *pads =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *controls =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *fujinet =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    state->window = parent;
    state->session = session;
    state->dialog = dialog;
    state->pad_group = GTK_WIDGET(pads);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_page_set_icon_name(page, "preferences-system-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_header_suffix(machine, intv_help_button("prefs/Machine"));
    adw_preferences_group_set_description(
        machine, "Applied by restarting the session when this dialog "
                "closes. FujiNet stays connected.");
    {
        gboolean have_ecs_rom = intvsession_has_ecs_rom(session);
        GtkWidget *ecs_row = combo_row(
            state, "ECS",
            have_ecs_rom
                ? "Entertainment Computer System -- second controller pair, "
                  "ECS keyboard, extra sound"
                : "Unavailable: ecs.bin not found in the ROM directory",
            "ecs", INTVSESSION_HW_AUTO, hw_mode_table(), APPLY_ON_CLOSE);
        gtk_widget_set_sensitive(ecs_row, have_ecs_rom);
        adw_preferences_group_add(machine, ecs_row);
        state->ecs_row = ecs_row;
    }
    adw_preferences_group_add(
        machine,
        combo_row(state, "Intellivoice", "Speech synthesis peripheral",
                 "ivoice", INTVSESSION_HW_AUTO, hw_mode_table(),
                 APPLY_ON_CLOSE));
    adw_preferences_group_add(
        machine,
        combo_row(state, "Video Standard", NULL, "video_standard",
                 INTVSESSION_VIDEO_NTSC, video_table(), APPLY_ON_CLOSE));

    adw_preferences_page_set_title(input, "Input");
    adw_preferences_page_set_name(input, "Input");
    adw_preferences_page_set_icon_name(input, "input-gaming-symbolic");

    adw_preferences_group_set_title(keyboard, "Keyboard");
    adw_preferences_group_set_header_suffix(keyboard, intv_help_button("prefs/Input"));
    adw_preferences_group_add(
        keyboard,
        switch_row(state, "ECS Keyboard",
                  "When on, the keyboard types on the ECS instead of "
                  "driving the hand controllers (toggle any time, applies "
                  "immediately). Warning: the FujiNet CONFIG ROM has no ECS "
                  "keyboard support yet, so while this is on its menus can "
                  "only be driven by a gamepad or the Controllers window (F9)",
                  "keyboard_mode", 0, APPLY_LIVE));

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_description(
        pads, "Assigned to the controllers in connection order unless chosen "
              "here, and remembered. Enabling gamepads is read when the session "
              "starts, so changing it restarts the session when this dialog "
              "closes.");
    adw_preferences_group_add(
        pads,
        switch_row(state, "Enable gamepads", "Poll USB/Bluetooth gamepads",
                   "enable_gamepad", 1, APPLY_ON_CLOSE));
    rebuild_pad_rows(state);
    state->pad_generation = intvsession_gamepad_generation(session);

    adw_preferences_group_set_title(controls, "Controls");
    adw_preferences_group_set_description(
        controls, "Keys and gamepad buttons are re-mapped in the Controllers "
                  "window (F9): press Map, choose a control, then press the key "
                  "or gamepad button to give it.");
    adw_preferences_group_add(
        controls,
        button_row("Controllers window", "The handsets, the console's RESET and Map",
                   "Open Controllers", G_CALLBACK(on_open_controllers), state));
    adw_preferences_group_add(
        controls,
        button_row("Reset Controls", "Put every key and gamepad button back to its default",
                   "Reset Controls", G_CALLBACK(on_reset_controls), state));

    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_set_description(
        fujinet, "Applied by restarting the session when this dialog closes.");
    {
        GtkWidget *external = switch_row(
            state, "Use external FujiNet",
            "Skip the built-in FujiNet and dial another instance. Start that "
            "instance listening on this address (its [BOIP] port).",
            "fujinet_external", 0, APPLY_ON_CLOSE);
        GtkWidget *addr = adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                      "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
        gtk_editable_set_text(
            GTK_EDITABLE(addr),
            intvsession_get_str(session, "fujinet_external_addr", ""));
        g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
        gtk_widget_set_sensitive(
            addr, adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
        g_signal_connect(external, "notify::active",
                         G_CALLBACK(external_toggled), addr);
        adw_preferences_group_add(fujinet, external);
        adw_preferences_group_add(fujinet, addr);
    }

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, fujinet);
    adw_preferences_page_add(input, keyboard);
    adw_preferences_page_add(input, pads);
    adw_preferences_page_add(input, controls);
    adw_preferences_dialog_add(dialog, page);
    adw_preferences_dialog_add(dialog, input);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    g_object_set_data_full(G_OBJECT(dialog), "intv-prefs", state, g_free);
    adw_preferences_dialog_add(dialog, build_roms_page(state, G_OBJECT(dialog)));
    refresh_rows(state);
    /* Developer affordance for screenshots: INTV_PREFS_PAGE=roms. */
    if (g_strcmp0(g_getenv("INTV_PREFS_PAGE"), "roms") == 0)
        adw_preferences_dialog_set_visible_page_name(dialog, "ROMs");
    else if (g_strcmp0(g_getenv("INTV_PREFS_PAGE"), "input") == 0)
        adw_preferences_dialog_set_visible_page_name(dialog, "Input");

    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
