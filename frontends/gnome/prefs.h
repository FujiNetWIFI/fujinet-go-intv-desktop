/*
 * IntvPrefs -- the Settings/Preferences dialog. See prefs.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"
#include "window.h"

G_BEGIN_DECLS

/* Shows the (singleton, modal-over-parent) preferences dialog. Machine
 * options (ECS/Intellivoice/video standard) are applied by restarting the
 * session when the dialog closes, via intv_window_restart_session; the ECS
 * keyboard input-mode toggle applies live. */
void intv_prefs_show(IntvWindow *parent, intvsession *session);

G_END_DECLS
