/*
 * intv_keysym_from_gdk: translates a GDK keyval to the private numbering
 * intvsession_key_from_keysym() actually expects (intvsession.h's own
 * INTVSESSION_KEYSYM_* enum, starting at 0x1000) -- NOT an X11/GDK keysym.
 * GDK's own values for arrows/numpad/modifiers (e.g. GDK_KEY_Up == 0xFF52)
 * happen to look plausible but do not match that enum at all, so passing
 * them through unmodified silently drops every arrow/numpad/modifier key
 * (matched no case, always hit the default: none() branch) while ASCII
 * letters/digits kept working by coincidence, since GDK's values for those
 * ARE their ASCII codes. Shared by window.c, keypad/keypad_window.c and
 * ecskbd/ecskbd_window.c so the mapping only lives in one place.
 *
 * Callers handling a GtkEventControllerKey signal should use
 * intv_keysym_from_key_event() at the bottom of this file rather than
 * calling intv_keysym_from_gdk() on the raw keyval -- see its own comment
 * for why the raw keyval breaks action-button + keypad chords.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "intvsession.h"

static inline uint32_t intv_keysym_from_gdk(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Up:    return INTVSESSION_KEYSYM_UP;
    case GDK_KEY_Down:  return INTVSESSION_KEYSYM_DOWN;
    case GDK_KEY_Left:  return INTVSESSION_KEYSYM_LEFT;
    case GDK_KEY_Right: return INTVSESSION_KEYSYM_RIGHT;
    /* Both the NumLock-on keypad keysyms and the NumLock-off navigation
     * aliases GDK sends in their place -- jzIntv reads the keypad through
     * SDL, which reports SDLK_KP_* either way, so the left controller's
     * whole keypad must not depend on a NumLock LED. */
    case GDK_KEY_KP_Insert: return INTVSESSION_KEYSYM_KP_0;
    case GDK_KEY_KP_End:    return INTVSESSION_KEYSYM_KP_1;
    case GDK_KEY_KP_Down:   return INTVSESSION_KEYSYM_KP_2;
    case GDK_KEY_KP_Next:   return INTVSESSION_KEYSYM_KP_3;
    case GDK_KEY_KP_Left:   return INTVSESSION_KEYSYM_KP_4;
    case GDK_KEY_KP_Begin:  return INTVSESSION_KEYSYM_KP_5;
    case GDK_KEY_KP_Right:  return INTVSESSION_KEYSYM_KP_6;
    case GDK_KEY_KP_Home:   return INTVSESSION_KEYSYM_KP_7;
    case GDK_KEY_KP_Up:     return INTVSESSION_KEYSYM_KP_8;
    case GDK_KEY_KP_Prior:  return INTVSESSION_KEYSYM_KP_9;
    case GDK_KEY_KP_Delete: return INTVSESSION_KEYSYM_KP_PERIOD;
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
    /* Not pad keys, but the ECS keyboard map wants all three. Without
     * these they hit the default: passthrough below and arrive as GDK's
     * 0xFF-range values, which never match intvsession.h's private
     * 0x1000-range enum -- exactly the silent-drop bug this file's own
     * header describes, just for a different set of keys. */
    case GDK_KEY_Escape:    return INTVSESSION_KEYSYM_ESCAPE;
    case GDK_KEY_Return:    return INTVSESSION_KEYSYM_RETURN;
    case GDK_KEY_BackSpace: return INTVSESSION_KEYSYM_BACKSPACE;
    default:
        /* Printable ASCII: GDK's own keyval for a letter/digit/punctuation
         * key IS its ASCII value (GDK_KEY_a == 0x61, GDK_KEY_1 == 0x31,
         * ...), which is exactly what intvsession_key_from_keysym's own
         * ASCII cases expect -- pass through unchanged. */
        return (uint32_t)keyval;
    }
}

/* The entry point the key-event handlers should actually call.
 *
 * GTK hands its key-pressed/key-released signals an ALREADY-SHIFTED keyval:
 * press Shift and "1", and keyval is GDK_KEY_exclam, not GDK_KEY_1. That is
 * fatal here rather than cosmetic, because Shift is itself a controller
 * binding -- mapping.c binds LSHIFT/RSHIFT to the pads' top action buttons
 * -- while the number row is the RIGHT controller's keypad. Taken together,
 * the plain keyval means holding an action button silently kills all twelve
 * of that controller's keypad keys, and holding Shift turns "," (the left
 * disc's SE) into "<". Pressing a button and a keypad key at once is
 * ordinary Intellivision play, so re-translate the hardware keycode with
 * Shift masked out and map the unshifted result instead.
 *
 * Only the Shift-held path pays for the extra translation; everything else
 * takes the keyval GTK already computed. */
static inline uint32_t intv_keysym_from_key_event(GtkEventControllerKey *ctrl,
                                                  guint keyval, guint keycode,
                                                  GdkModifierType state)
{
    GdkEvent *event;
    GdkDisplay *display;
    guint unshifted = 0;

    if (!(state & GDK_SHIFT_MASK))
        return intv_keysym_from_gdk(keyval);

    event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctrl));
    display = event ? gdk_event_get_display(event) : NULL;
    if (display &&
        gdk_display_translate_key(display, keycode, state & ~GDK_SHIFT_MASK,
                                  gdk_key_event_get_layout(event), &unshifted,
                                  NULL, NULL, NULL))
        return intv_keysym_from_gdk(unshifted);

    /* No event to read the layout from, or no translation for this keycode:
     * the shifted keyval is still better than nothing (letters, arrows and
     * modifiers are all unaffected by Shift anyway). */
    return intv_keysym_from_gdk(keyval);
}
