/*
 * key_translate -- Win32 key message -> intvsession.h keysym, and the
 * hotkeys the main window claims before any binding.
 *
 * Split out of main.c so it can be unit-tested: main.c is a WIN32_EXECUTABLE
 * with a WinMain and a live session behind it, which nothing can link
 * against. Modelled on frontends/kde/KeyForward.cpp, which was carved out of
 * its own frontend for exactly the same reason and is covered by
 * keyforward_test.
 *
 * Testing this matters more here than anywhere else in the tree. The
 * maintainer has no Windows machine (cmake/toolchains/mingw-w64.cmake), and
 * an untested translation gap in this file is what made an Intellivision-to-
 * USB adapter unmappable: every VK the two tables below have no case for
 * resolved to 0, and a keypad window's Map mode reads 0 as "not a key" and
 * threw the press away without a word. See keytranslate_test.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>

#include "intvsession.h"
#include "key_forward.h"

/* ---- keyboard ----------------------------------------------------------------
 * intvsession_key_from_keysym wants intvsession.h's OWN private numbering
 * for non-printable keys (INTVSESSION_KEYSYM_* starting at 0x1000) -- NOT
 * an X11 keysym the way the CoCo/MSX ports' own special_keysym tables
 * produce. See frontends/gnome/keysym_map.h's header comment for why that
 * distinction matters (a real bug was caught there: passing a toolkit's
 * native key value straight through silently drops every arrow/numpad/
 * modifier key while ASCII letters keep working by coincidence).
 *
 * Win32's VK_* codes carry no shift state of their own, so non-special keys
 * resolve straight to their base US-layout character, stable across
 * down/up by construction -- same reasoning the CoCo/MSX ports' own
 * base_char() documents.
 *
 * These two tables cover only the keys with a DEFAULT mapping. They are no
 * longer the whole vocabulary: resolve_keysym below falls back to
 * intvsession.h's HID/native bands for everything else, so any key Windows
 * delivers can be captured and bound even though nothing here names it. */

static uint32_t special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_UP:       return INTVSESSION_KEYSYM_UP;
    case VK_DOWN:     return INTVSESSION_KEYSYM_DOWN;
    case VK_LEFT:     return INTVSESSION_KEYSYM_LEFT;
    case VK_RIGHT:    return INTVSESSION_KEYSYM_RIGHT;
    case VK_NUMPAD0:  return INTVSESSION_KEYSYM_KP_0;
    case VK_NUMPAD1:  return INTVSESSION_KEYSYM_KP_1;
    case VK_NUMPAD2:  return INTVSESSION_KEYSYM_KP_2;
    case VK_NUMPAD3:  return INTVSESSION_KEYSYM_KP_3;
    case VK_NUMPAD4:  return INTVSESSION_KEYSYM_KP_4;
    case VK_NUMPAD5:  return INTVSESSION_KEYSYM_KP_5;
    case VK_NUMPAD6:  return INTVSESSION_KEYSYM_KP_6;
    case VK_NUMPAD7:  return INTVSESSION_KEYSYM_KP_7;
    case VK_NUMPAD8:  return INTVSESSION_KEYSYM_KP_8;
    case VK_NUMPAD9:  return INTVSESSION_KEYSYM_KP_9;
    case VK_DECIMAL:  return INTVSESSION_KEYSYM_KP_PERIOD;
    case VK_LSHIFT:   return INTVSESSION_KEYSYM_LSHIFT;
    case VK_RSHIFT:   return INTVSESSION_KEYSYM_RSHIFT;
    case VK_LCONTROL: return INTVSESSION_KEYSYM_LCTRL;
    case VK_RCONTROL: return INTVSESSION_KEYSYM_RCTRL;
    case VK_LMENU:    return INTVSESSION_KEYSYM_LALT;
    case VK_RMENU:    return INTVSESSION_KEYSYM_RALT;
    /* VK_RETURN doubles as the numpad Enter key when the extended-key bit
     * is set (checked by the caller via lParam), otherwise it resolves to
     * INTVSESSION_KEYSYM_RETURN in on_key itself, not here (this table has
     * no lParam to check). Non-printable keys the base controller map has
     * no use for, but the ECS keyboard map (on_key's own ECS branch) does. */
    case VK_ESCAPE: return INTVSESSION_KEYSYM_ESCAPE;
    case VK_BACK:   return INTVSESSION_KEYSYM_BACKSPACE;
    default:
        return 0;
    }
}

static uint32_t base_char(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PLUS:   return '=';
    case VK_OEM_COMMA:  return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_1:      return ';'; /* ECS-keyboard-only (KEYB_SEMI) */
    default:            return 0;
    }
}

/* WM_(SYS)KEYDOWN/UP only report the generic VK_SHIFT/CONTROL/MENU; the
 * left/right pair is recovered from the scancode (Shift) or the
 * extended-key bit (Control/Alt), the standard Win32 idiom for this. */
static WPARAM resolve_side(WPARAM vk, LPARAM lp)
{
    UINT scancode;
    int extended;

    switch (vk) {
    case VK_SHIFT:
        scancode = (UINT)((lp >> 16) & 0xFF);
        return MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

/* The hotkeys on_key below claims before anything else sees them. Exported
 * (key_forward.h) so a Map mode can refuse to bind one and say why, rather
 * than accepting a binding on_key would then shadow forever. */
int intv_key_is_reserved(WPARAM vk, const char **what)
{
    const char *name = NULL;

    switch (vk) {
    case VK_F9:  name = "the keypad window"; break;
    case VK_F10: name = "the ECS keyboard window"; break;
    case VK_F11: name = "fullscreen"; break;
    case VK_F12: name = "the debugger"; break;
    case 'R':
        if (GetKeyState(VK_CONTROL) & 0x8000)
            name = "Reset to CONFIG";
        break;
    default:
        break;
    }
    if (what)
        *what = name;
    return name != NULL;
}

/* VK code + message lParam -> intvsession.h's keysym numbering. Never 0 for
 * a key Windows actually delivered: the curated symbols are tried first, and
 * anything they have no name for falls into intvsession.h's HID band (via
 * the lParam scancode) or, failing that, its native band.
 *
 * The fallback is the fix for a real bug. This used to return 0 for every VK
 * outside special_keysym/base_char above -- every F-key, most punctuation,
 * the numpad operators, and the whole OEM-specific/unassigned band
 * (0x88-0x8F, 0x92-0x96, 0xE0-0xE4, 0xE9-0xF5, 0xFF) that Windows hands a
 * HID keyboard reporting usages a normal PC keyboard has no cap for. A
 * keypad window's Map mode reads 0 as "not a key" and discarded the press,
 * so an Intellivision-to-USB adapter in keyboard mode could not be mapped at
 * all: the keys the user most needed were exactly the ones that vanished. */
static uint32_t resolve_keysym(WPARAM vk, LPARAM lp)
{
    const WPARAM rvk = resolve_side(vk, lp);
    const UINT scancode = (UINT)((lp >> 16) & 0xFF);
    const int extended = (lp & 0x01000000) != 0;
    uint32_t keysym, usage;

    /* Numpad Enter reports as VK_RETURN with the extended-key bit set;
     * plain Enter has no keypad binding in intv_keymap.c's base controller
     * map (jzIntv's own mapping.c has no ENTER-key binding outside the
     * numpad), but the ECS keyboard map does want it, hence
     * INTVSESSION_KEYSYM_RETURN rather than dropping it outright. */
    if (rvk == VK_RETURN && (lp & 0x01000000))
        keysym = INTVSESSION_KEYSYM_KP_ENTER;
    else if (rvk == VK_RETURN)
        keysym = INTVSESSION_KEYSYM_RETURN;
    else
        keysym = special_keysym(rvk);
    if (!keysym)
        keysym = base_char(rvk);
    if (keysym)
        return keysym;

    /* Pause is the one key no scancode table can resolve: Windows reports it
     * as scancode 0x45 with the extended bit CLEAR, which is Num Lock's own
     * code (see core/src/hid_keys.c's note on the E0 table). The VK
     * disambiguates it, and only this layer has the VK. */
    if (rvk == VK_PAUSE)
        return intvsession_keysym_from_hid(0x48);

    usage = intvsession_hid_from_win_scancode(scancode, extended);
    keysym = intvsession_keysym_from_hid(usage);
    if (keysym)
        return keysym;

    /* No usable scancode -- Windows gives some HID usages a VK with a
     * MakeCode of 0. Fall back to the VK so the key is still bindable, just
     * named as raw hex. */
    return rvk ? INTVSESSION_KEYSYM_NATIVE_BASE + (uint32_t)rvk : 0;
}

/* Exported wrapper -- see key_forward.h's own comment on why the keypad
 * window's Map mode needs the translation half of intv_forward_key without
 * its dispatch half. */
uint32_t intv_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    return resolve_keysym(vk, lp);
}
