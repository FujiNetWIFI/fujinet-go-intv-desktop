/*
 * intv_keysym_from_gdk: translates a GDK keyval to the private numbering
 * intvsession_key_from_keysym() actually expects (intvsession.h's own
 * INTVSESSION_KEYSYM_* enum, starting at 0x1000) -- NOT an X11/GDK keysym.
 * GDK's own values for arrows/numpad/modifiers (e.g. GDK_KEY_Up == 0xFF52)
 * happen to look plausible but do not match that enum at all, so passing
 * them through unmodified silently drops every arrow/numpad/modifier key
 * (matched no case, always hit the default: none() branch) while ASCII
 * letters/digits kept working by coincidence, since GDK's values for those
 * ARE their ASCII codes. Shared by window.c and keypad/keypad_window.c so
 * the mapping only lives in one place.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gdk/gdk.h>

#include "intvsession.h"

static inline uint32_t intv_keysym_from_gdk(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Up:    return INTVSESSION_KEYSYM_UP;
    case GDK_KEY_Down:  return INTVSESSION_KEYSYM_DOWN;
    case GDK_KEY_Left:  return INTVSESSION_KEYSYM_LEFT;
    case GDK_KEY_Right: return INTVSESSION_KEYSYM_RIGHT;
    /* NumLock-on keypad keysyms only -- the NumLock-off navigation
     * aliases (GDK_KEY_KP_End et al) are not handled; a keypad user
     * pressing digits expects NumLock on anyway. */
    case GDK_KEY_KP_0: return INTVSESSION_KEYSYM_KP_0;
    case GDK_KEY_KP_1: return INTVSESSION_KEYSYM_KP_1;
    case GDK_KEY_KP_2: return INTVSESSION_KEYSYM_KP_2;
    case GDK_KEY_KP_3: return INTVSESSION_KEYSYM_KP_3;
    case GDK_KEY_KP_4: return INTVSESSION_KEYSYM_KP_4;
    case GDK_KEY_KP_5: return INTVSESSION_KEYSYM_KP_5;
    case GDK_KEY_KP_6: return INTVSESSION_KEYSYM_KP_6;
    case GDK_KEY_KP_7: return INTVSESSION_KEYSYM_KP_7;
    case GDK_KEY_KP_8: return INTVSESSION_KEYSYM_KP_8;
    case GDK_KEY_KP_9: return INTVSESSION_KEYSYM_KP_9;
    case GDK_KEY_KP_Decimal: return INTVSESSION_KEYSYM_KP_PERIOD;
    case GDK_KEY_KP_Enter:   return INTVSESSION_KEYSYM_KP_ENTER;
    case GDK_KEY_Shift_L:   return INTVSESSION_KEYSYM_LSHIFT;
    case GDK_KEY_Shift_R:   return INTVSESSION_KEYSYM_RSHIFT;
    case GDK_KEY_Control_L: return INTVSESSION_KEYSYM_LCTRL;
    case GDK_KEY_Control_R: return INTVSESSION_KEYSYM_RCTRL;
    case GDK_KEY_Alt_L:     return INTVSESSION_KEYSYM_LALT;
    case GDK_KEY_Alt_R:     return INTVSESSION_KEYSYM_RALT;
    default:
        /* Printable ASCII: GDK's own keyval for a letter/digit/punctuation
         * key IS its ASCII value (GDK_KEY_a == 0x61, GDK_KEY_1 == 0x31,
         * ...), which is exactly what intvsession_key_from_keysym's own
         * ASCII cases expect -- pass through unchanged. */
        return (uint32_t)keyval;
    }
}
