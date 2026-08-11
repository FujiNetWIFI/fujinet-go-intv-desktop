/*
 * IntvKeypadWindow: the clickable keypad sub-window, showing both hand
 * controllers side by side so a mouse alone can drive every button and
 * direction disc on the Intellivision controller.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void intv_keypad_window_toggle(GtkWindow *parent, intvsession *session);
gboolean intv_keypad_window_is_visible(void);

G_END_DECLS
