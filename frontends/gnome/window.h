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

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *intv_icon_name(void);

G_END_DECLS
