/*
 * The CP-1610/STIC debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "intvsession.h"

G_BEGIN_DECLS

void intv_debugger_show(GtkWindow *parent, intvsession *session);

G_END_DECLS
