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

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *intv_icon_name(void);

G_END_DECLS
