/*
 * IntvDisplay: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

#define INTV_TYPE_DISPLAY (intv_display_get_type())
G_DECLARE_FINAL_TYPE(IntvDisplay, intv_display, INTV, DISPLAY, GtkWidget)

GtkWidget *intv_display_new(intvsession *session);

G_END_DECLS
