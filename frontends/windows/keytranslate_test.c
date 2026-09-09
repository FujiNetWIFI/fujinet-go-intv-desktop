/*
 * keytranslate_test -- frontends/windows/key_translate.c.
 *
 * The regression this exists for: every VK outside special_keysym/base_char
 * used to translate to keysym 0, and a keypad window's Map mode reads 0 as
 * "not a key" and discards the press in silence. That made an
 * Intellivision-to-USB adapter in keyboard mode unmappable -- the keys it
 * emits are exactly the ones neither table names -- and it presented as "the
 * key can't be captured", with no clue as to why.
 *
 * So the central assertion here is a negative one: NO key message resolves
 * to 0. Anything the curated tables miss must land in one of
 * intvsession.h's fallback bands.
 *
 * Runs under Wine on the maintainer's Linux box and natively in CI. It needs
 * no window, no session and no keyboard -- WM_KEYDOWN's wParam/lParam pair is
 * just two integers, which is the whole reason key_translate.c was split out
 * of main.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "intvsession.h"
#include "key_forward.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "keytranslate_test: FAILED: %s\n", what);
        failed = 1;
    }
}

/* Builds the lParam a real WM_KEYDOWN carries: scancode in bits 16-23, the
 * extended-key flag in bit 24. Nothing else in it is read. */
static LPARAM lp_for(UINT scancode, int extended)
{
    return (LPARAM)((scancode & 0xFF) << 16) |
           (extended ? (LPARAM)0x01000000 : 0);
}

static uint32_t sym(WPARAM vk, UINT scancode, int extended)
{
    return intv_keysym_from_msg(vk, lp_for(scancode, extended));
}

static void eq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "keytranslate_test: FAILED: %s (got 0x%X, want 0x%X)\n",
                what, (unsigned)got, (unsigned)want);
        failed = 1;
    }
}

int main(void)
{
    /* ---- the curated symbols still win --------------------------------
     * They must be tried before the fallback bands: a key with a curated
     * symbol must never ALSO be reachable through a band, or the same
     * physical key would bind under two different values and a binding made
     * one way would not match the other. */
    eq("'a' is ASCII", sym('A', 0x1E, 0), 'a');
    eq("'5' is ASCII", sym('5', 0x06, 0), '5');
    eq("space is ASCII", sym(VK_SPACE, 0x39, 0), ' ');
    eq("Up Arrow keeps its curated symbol",
       sym(VK_UP, 0x48, 1), INTVSESSION_KEYSYM_UP);
    eq("Numpad 7 keeps its curated symbol",
       sym(VK_NUMPAD7, 0x47, 0), INTVSESSION_KEYSYM_KP_7);
    eq("Escape keeps its curated symbol",
       sym(VK_ESCAPE, 0x01, 0), INTVSESSION_KEYSYM_ESCAPE);
    eq("plain Return keeps its curated symbol",
       sym(VK_RETURN, 0x1C, 0), INTVSESSION_KEYSYM_RETURN);
    /* The extended bit is the only thing separating these two. */
    eq("Numpad Enter is the keypad symbol, not Return",
       sym(VK_RETURN, 0x1C, 1), INTVSESSION_KEYSYM_KP_ENTER);
    eq("Right Ctrl resolves to the right-hand symbol",
       sym(VK_CONTROL, 0x1D, 1), INTVSESSION_KEYSYM_RCTRL);
    eq("Left Ctrl resolves to the left-hand symbol",
       sym(VK_CONTROL, 0x1D, 0), INTVSESSION_KEYSYM_LCTRL);

    /* ---- and everything else now resolves too --------------------------
     * Each of these returned 0 before the fallback bands existed. */
    eq("F1 lands in the HID band",
       sym(VK_F1, 0x3B, 0), INTVSESSION_KEYSYM_HID_BASE + 0x3A);
    eq("F13 lands in the HID band",
       sym(VK_F13, 0x64, 0), INTVSESSION_KEYSYM_HID_BASE + 0x68);
    eq("Tab lands in the HID band",
       sym(VK_TAB, 0x0F, 0), INTVSESSION_KEYSYM_HID_BASE + 0x2B);
    eq("Insert lands in the HID band",
       sym(VK_INSERT, 0x52, 1), INTVSESSION_KEYSYM_HID_BASE + 0x49);
    eq("Numpad * lands in the HID band",
       sym(VK_MULTIPLY, 0x37, 0), INTVSESSION_KEYSYM_HID_BASE + 0x55);
    eq("'[' lands in the HID band",
       sym(VK_OEM_4, 0x1A, 0), INTVSESSION_KEYSYM_HID_BASE + 0x2F);
    eq("'/' lands in the HID band",
       sym(VK_OEM_2, 0x35, 0), INTVSESSION_KEYSYM_HID_BASE + 0x38);
    /* Pause is the one key no scancode table can resolve on its own:
     * Windows reports it as 0x45 with the extended bit CLEAR, which is Num
     * Lock's own code. The VK is what separates them. */
    eq("Pause resolves despite sharing Num Lock's scancode",
       sym(VK_PAUSE, 0x45, 0), INTVSESSION_KEYSYM_HID_BASE + 0x48);
    eq("Num Lock still resolves to Num Lock",
       sym(VK_NUMLOCK, 0x45, 0), INTVSESSION_KEYSYM_HID_BASE + 0x53);
    /* An OEM-specific VK with no scancode at all -- what Windows hands some
     * HID usages from a non-standard keyboard. The native band catches it. */
    eq("a VK with no scancode falls back to the native band",
       sym(0xFF, 0x00, 0), INTVSESSION_KEYSYM_NATIVE_BASE + 0xFF);

    /* ---- THE regression: nothing resolves to 0 -------------------------
     * Sweep the whole VK space against a plausible scancode, and the whole
     * scancode space against a VK that names nothing. Before the fallback
     * bands, most of both swept to 0 and was silently unmappable. */
    for (WPARAM vk = 1; vk <= 0xFF; vk++) {
        char what[64];
        /* Skip the VKs that only ever arrive as a side-resolved pair; they
         * are covered explicitly above and MapVirtualKey needs a real
         * scancode to split them. */
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (sym(vk, 0x00, 0) != 0 && sym(vk, 0x1E, 0) != 0)
            continue;
        snprintf(what, sizeof(what), "VK 0x%02X resolves to something",
                (unsigned)vk);
        check(what, 0);
    }
    for (UINT sc = 1; sc <= 0xFF; sc++) {
        char what[64];
        /* VK 0 with a scancode: the scancode alone has to carry it. */
        if (sym(0, sc, 0) != 0 || intvsession_hid_from_win_scancode(sc, 0) == 0)
            continue;
        snprintf(what, sizeof(what), "scancode 0x%02X resolves to something",
                sc);
        check(what, 0);
    }

    /* ---- every result must be nameable --------------------------------
     * Map mode prints the name of whatever it just bound. A keysym with no
     * name used to leave the caller's buffer untouched, i.e. printed as
     * uninitialised stack. */
    for (WPARAM vk = 1; vk <= 0xFF; vk++) {
        char name[64];
        const uint32_t k = sym(vk, 0x1E, 0);
        if (!k)
            continue;
        memset(name, 'x', sizeof(name));
        if (intvsession_keysym_name(k, name, sizeof(name)) <= 0 ||
            name[0] == 'x') {
            fprintf(stderr, "keytranslate_test: FAILED: keysym 0x%X (VK "
                            "0x%02X) has no name\n",
                    (unsigned)k, (unsigned)vk);
            failed = 1;
        }
    }

    /* ---- reserved hotkeys ----------------------------------------------
     * Map mode asks so it can refuse the key with an explanation instead of
     * storing a binding the main window's own hotkey handling would shadow
     * forever. */
    {
        const char *what = NULL;
        check("F11 is reserved and names its claimant",
              intv_key_is_reserved(VK_F11, &what) && what &&
                  strstr(what, "fullscreen"));
        what = (const char *)1;
        check("F1 is not reserved, and clears *what",
              !intv_key_is_reserved(VK_F1, &what) && what == NULL);
        check("intv_key_is_reserved tolerates a NULL out-parameter",
              intv_key_is_reserved(VK_F12, NULL));
    }

    if (failed) {
        fprintf(stderr, "keytranslate_test: FAILED\n");
        return 1;
    }
    printf("keytranslate_test: OK\n");
    return 0;
}
