/*
 * IntvEcsKeyboardWindow: the clickable ECS keyboard sub-window, so a mouse
 * alone can drive every key of the ECS's own 7x8 scan-matrix keyboard --
 * the on-screen counterpart to the hand-controller keypad window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void intv_ecskbd_window_toggle(GtkWindow *parent, intvsession *session);
gboolean intv_ecskbd_window_is_visible(void);

G_END_DECLS
