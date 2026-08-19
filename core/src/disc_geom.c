/*
 * disc_geom -- the hand controller disc's pointer hit test, shared by all
 * four frontends' keypad-window disc widgets (GNOME, KDE, macOS, Windows).
 *
 * Pure arithmetic, no session and no toolkit: see intvsession.h for the
 * angle/numbering convention and for why this resolves all 16 positions
 * while intv_disc_from_stick (gamepad_sdl.c) resolves only 8.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _USE_MATH_DEFINES
#include <math.h>

#include "intvsession.h"

int intvsession_disc_from_point(double dx, double dy, double radius)
{
    double dist, deg;
    int dir;

    if (!(radius > 0.0))
        return -1;

    dist = sqrt(dx * dx + dy * dy);
    if (dist < radius * INTVSESSION_DISC_DEADZONE_FRAC)
        return -1;

    /* dy arrives in screen convention (increasing DOWNWARD); negating it
     * converts to the ordinary compass/math convention the disc's numbering
     * is defined in, where North is +90 degrees. Without this a click below
     * centre would report North (4) instead of South (12). */
    deg = atan2(-dy, dx) * 180.0 / M_PI;
    if (deg < 0.0)
        deg += 360.0;

    /* Round to the nearest sector centre rather than truncating: position
     * n owns [n*22.5 - 11.25, n*22.5 + 11.25), so East's sector straddles
     * 0 degrees and the top of the range (348.75 and up) wraps back to it
     * -- which is what the modulo is for, since floor() yields 16 there. */
    dir = (int)floor(deg / INTVSESSION_DISC_SECTOR_DEG + 0.5) %
          INTVSESSION_DISC_POSITIONS;
    if (dir < 0)
        dir += INTVSESSION_DISC_POSITIONS;
    return dir;
}
