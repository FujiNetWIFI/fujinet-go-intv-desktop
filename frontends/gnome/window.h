/*
 * IntvWindow: main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

#define INTV_TYPE_WINDOW (intv_window_get_type())
G_DECLARE_FINAL_TYPE(IntvWindow, intv_window, INTV, WINDOW,
                     AdwApplicationWindow)

GtkWidget *intv_window_new(AdwApplication *app, intvsession *session);
void intv_window_toast(IntvWindow *self, const char *message);

/* Stops and restarts the session with intvsession_default_opts() re-read
 * from the settings store -- the "apply" side of a Settings dialog change
 * to ECS/Intellivoice/video standard, all of which need a fresh emulator
 * thread to take effect (see intvsession_start_opts). Toasts
 * intvsession_last_error() on failure rather than silently leaving the
 * machine stopped. */
void intv_window_restart_session(IntvWindow *self);

/* The picture, or the No-BIOS page when there are no system ROMs to boot. */
void intv_window_show_page(IntvWindow *self);

/* One OK-button alert over the window. */
void intv_window_message(IntvWindow *self, const char *heading,
                         const char *body);

/* The file chooser behind every import: a "these" filter (suffixes, any
 * case) and All files. */
GtkFileDialog *intv_file_dialog(const char *title, const char *filter_name,
                                const char *const *exts);

/* Asks for BIOS files (or a folder), imports each, shows the one "BIOS
 * Imported" / "Nothing Imported" alert, and (re)starts the machine when it
 * can now boot. done(done_data), if given, runs afterwards; the caller keeps
 * done_data alive until then. */
void intv_window_import_bios(IntvWindow *self, gboolean folder,
                             void (*done)(gpointer), gpointer done_data);

/* Installs the display-wide CSS (.intv-toggle:checked, .intv-accent: the
 * icon's dark green with white text). Idempotent; call once a display
 * exists. */
void intv_css_install(void);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *intv_icon_name(void);

G_END_DECLS
