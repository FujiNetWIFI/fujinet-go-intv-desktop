/*
 * intv_forward_key -- translate a Win32 key message to the emulated
 * machine's input, defined in main.c beside the VK translation tables it
 * uses (special_keysym/base_char/resolve_side) and the session pointer it
 * writes to.
 *
 * The keypad and ECS keyboard windows need this because their child
 * controls -- BUTTON, and the custom disc class -- take keyboard focus on
 * click, so WM_KEYDOWN/WM_KEYUP land there rather than on the main window,
 * and Win32 does not bubble those messages back up to the parent on its
 * own. Rather than subclassing every child that could end up with focus
 * (easy to forget -- see keypad_window.c's own FOCUS note), each window
 * calls its own *_pretranslate (intv_keypad_pretranslate,
 * intv_ecskbd_pretranslate) from the frontend's message pump in main.c,
 * ahead of TranslateMessage/DispatchMessageA, keyed off which top-level
 * window owns msg->hwnd rather than which child currently has focus. This
 * is a filter in the app's own loop, not a system-wide keyboard hook.
 *
 * Hotkeys (F9/F10/F11/F12, Ctrl-R) are deliberately NOT handled here --
 * they stay in main.c's own on_key, since only the main window should
 * claim them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <windows.h>

/* `down` is 1 for a press, 0 for a release. Both matter: jzIntv's pad_t
 * tracks a held key by level, so a missed release leaves it stuck down.
 *
 * intv_forward_key honours the "keyboard_mode" setting, routing to either
 * the hand controllers or the ECS keyboard. intv_forward_ecs_key always
 * routes to the ECS keyboard -- for the ECS keyboard window, which means
 * the same thing either way. */
void intv_forward_key(WPARAM vk, LPARAM lp, int down);
void intv_forward_ecs_key(WPARAM vk, LPARAM lp, int down);

/* Translate-only half of intv_forward_key, exported for the keypad window's
 * own Map mode (frontends/windows/keypad/keypad_window.c): it needs the
 * VK+lParam -> intvsession.h keysym translation to know WHICH key was
 * pressed while capturing, without intv_forward_key's own dispatch (which
 * would inject the keystroke into the machine, exactly what capturing is
 * supposed to prevent).
 *
 * Non-zero for any key Windows actually delivered -- including keys with no
 * default mapping and keys a normal PC keyboard has no cap for, which land
 * in intvsession.h's HID/native fallback bands. (It used to return 0 for
 * those, and Map mode dropped the press: that is what made an
 * Intellivision-to-USB adapter unmappable.) 0 only for a message carrying no
 * VK at all. Defined in main.c, next to the VK translation tables it wraps
 * (special_keysym/base_char/resolve_side). */
uint32_t intv_keysym_from_msg(WPARAM vk, LPARAM lp);

/* The US-layout character a key message types, for ECS keyboard mode only.
 * DELIBERATELY independent of intv_keysym_from_msg: that one answers "which
 * key is this" and its result is a persisted binding identity, while this
 * answers "what did the user type", which is what picks an ECS key. See
 * key_translate.c's own comment on why the two tables must not be merged --
 * "/" and "'" appear here and must NOT appear in base_char. `shift` is the
 * live Shift state; returns 0 for a key that types nothing. */
uint32_t intv_char_from_msg(WPARAM vk, LPARAM lp, int shift);

/* A stable id for the physical key a message came from -- what
 * intvsession_ecs_key_event's held-key table is keyed on. Never the VK
 * alone; see key_translate.c's own comment on the shift keys. */
uint32_t intv_host_key_from_msg(WPARAM vk, LPARAM lp);

/* Non-zero if `vk` is one of the hotkeys the main window claims before any
 * binding is consulted (F9-F12, Ctrl-R). Map mode asks so it can refuse the
 * key with an explanation instead of storing a binding on_key would shadow.
 * On a non-zero return *what (if non-NULL) names what claims it, e.g.
 * "fullscreen"; it is set to NULL otherwise. Defined in main.c beside
 * on_key, the function whose behaviour it describes. */
int intv_key_is_reserved(WPARAM vk, const char **what);

/* Handle WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP, returning 1 if the
 * message was consumed. Anything else returns 0 and should be passed on as
 * usual. */
#define INTV_DEFINE_FORWARD_KEY_MSG(name, fn)                                 \
    static inline int name(UINT msg, WPARAM wp, LPARAM lp)                    \
    {                                                                         \
        switch (msg) {                                                        \
        case WM_KEYDOWN:                                                      \
        case WM_SYSKEYDOWN:                                                   \
            fn(wp, lp, 1);                                                    \
            return 1;                                                         \
        case WM_KEYUP:                                                        \
        case WM_SYSKEYUP:                                                     \
            fn(wp, lp, 0);                                                    \
            return 1;                                                         \
        default:                                                              \
            return 0;                                                         \
        }                                                                     \
    }

INTV_DEFINE_FORWARD_KEY_MSG(intv_forward_key_msg, intv_forward_key)
INTV_DEFINE_FORWARD_KEY_MSG(intv_forward_ecs_key_msg, intv_forward_ecs_key)
