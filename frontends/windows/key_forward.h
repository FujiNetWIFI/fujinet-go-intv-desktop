/*
 * intv_forward_key -- translate a Win32 key message to the emulated
 * machine's input, defined in main.c beside the VK translation tables it
 * uses (special_keysym/base_char/resolve_side) and the session pointer it
 * writes to.
 *
 * The keypad and ECS keyboard windows need this because their child
 * controls -- BUTTON, and the custom disc class -- take keyboard focus on
 * click, so WM_KEYDOWN/WM_KEYUP land there rather than on the main window.
 * Each of those child procs is already subclassed for its own mouse
 * handling, so forwarding the key messages from the same place costs
 * nothing and avoids a global keyboard hook.
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
 * supposed to prevent). 0 for a key the emulated machine has no use for.
 * Defined in main.c, next to the VK translation tables it wraps
 * (special_keysym/base_char/resolve_side). */
uint32_t intv_keysym_from_msg(WPARAM vk, LPARAM lp);

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
