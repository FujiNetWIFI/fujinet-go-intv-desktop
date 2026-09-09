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

static uint32_t chr(WPARAM vk, UINT scancode, int shift)
{
    return intv_char_from_msg(vk, lp_for(scancode, 0), shift);
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

    /* ---- ECS keyboard characters --------------------------------------
     * intv_char_from_msg is a SEPARATE table from everything above, and the
     * two answer different questions: the keysym is a key's persisted
     * binding identity, the character is what the user just typed, which in
     * ECS keyboard mode is what picks the key. The ECS's shifted layer is
     * nothing like a PC's -- '/' is SHIFT+7 over there, '%' is
     * SHIFT+LEFT-ARROW -- so the symbols are unreachable except by
     * character. */
    eq("Shift+5 types '%'",     chr('5', 0x06, 1), '%');
    eq("unshifted 5 types '5'", chr('5', 0x06, 0), '5');
    eq("Shift+6 types '^'",     chr('6', 0x07, 1), '^');
    eq("Shift+9 types '('",     chr('9', 0x0A, 1), '(');
    /* The two cases base_char must NOT have, and this table must. */
    eq("'/' types '/'",         chr(VK_OEM_2, 0x35, 0), '/');
    eq("Shift+/ types '?'",     chr(VK_OEM_2, 0x35, 1), '?');
    eq("apostrophe types '\''", chr(VK_OEM_7, 0x28, 0), '\'');
    eq("Shift+' types '\"'",    chr(VK_OEM_7, 0x28, 1), '"');
    eq("'-' types '-'",         chr(VK_OEM_MINUS, 0x0C, 0), '-');
    eq("Shift+= types '+'",     chr(VK_OEM_PLUS, 0x0D, 1), '+');
    eq("Shift+; types ':'",     chr(VK_OEM_1, 0x27, 1), ':');
    eq("Shift+, types '<'",     chr(VK_OEM_COMMA, 0x33, 1), '<');
    eq("Shift+. types '>'",     chr(VK_OEM_PERIOD, 0x34, 1), '>');
    eq("letters fold to one case per side",
       chr('A', 0x1E, 0), 'a');
    eq("Shift+A types 'A'",     chr('A', 0x1E, 1), 'A');
    /* The numpad types nothing here on purpose: its VKs carry digits, but
     * the ECS map sends KP_7 to ECS "1" (upstream's keypad-shaped layout),
     * so letting a character through would silently re-lay the whole
     * numpad. intvsession_ecs_key_from_char refuses it too -- this is the
     * belt to that suspenders. */
    eq("numpad 7 types nothing", chr(VK_NUMPAD7, 0x47, 0), 0);
    eq("numpad / types nothing", chr(VK_DIVIDE, 0x35, 0), 0);
    /* Modifiers and F-keys type nothing. */
    eq("Shift itself types nothing", chr(VK_LSHIFT, 0x2A, 0), 0);
    eq("F5 types nothing",           chr(VK_F5, 0x3F, 0), 0);

    /* ---- the ECS held-key id -------------------------------------------
     * intvsession_ecs_key_event looks a release up by this, so the ONE
     * thing it must never do is collapse two physically distinct keys.
     * Windows reports the generic VK_SHIFT for both shift keys, which is
     * exactly the collapse to guard against: hold one, release the other,
     * and an ECS key still being held would be released. */
    check("the two Shift keys get distinct ids",
          intv_host_key_from_msg(VK_SHIFT, lp_for(0x2A, 0)) !=
          intv_host_key_from_msg(VK_SHIFT, lp_for(0x36, 0)));
    check("the two Ctrl keys get distinct ids",
          intv_host_key_from_msg(VK_CONTROL, lp_for(0x1D, 0)) !=
          intv_host_key_from_msg(VK_CONTROL, lp_for(0x1D, 1)));
    check("a key's id is the same on press and release",
          intv_host_key_from_msg('5', lp_for(0x06, 0)) ==
          intv_host_key_from_msg('5', lp_for(0x06, 0)));
    check("different keys get different ids",
          intv_host_key_from_msg('5', lp_for(0x06, 0)) !=
          intv_host_key_from_msg('6', lp_for(0x07, 0)));
    /* Windows gives some HID usages a VK with a MakeCode of 0; those must
     * still get a non-zero id of their own, or they would all collapse
     * together AND read as "no id" to the session. */
    check("a VK with no scancode still gets a distinct non-zero id",
          intv_host_key_from_msg(0xFF, 0) != 0 &&
          intv_host_key_from_msg(0xFF, 0) != intv_host_key_from_msg(0xFE, 0));

    /* ---- and binding identity did NOT move ----------------------------
     * The whole point of keeping the two tables apart. If base_char had
     * grown a VK_OEM_2 case to serve the character path, "/" would stop
     * landing in the HID band and every persisted binding for that key
     * would silently change meaning. */
    eq("'/' still lands in the HID band, unchanged",
       sym(VK_OEM_2, 0x35, 0), INTVSESSION_KEYSYM_HID_BASE + 0x38);
    eq("apostrophe still lands in the HID band, unchanged",
       sym(VK_OEM_7, 0x28, 0), INTVSESSION_KEYSYM_HID_BASE + 0x34);

    if (failed) {
        fprintf(stderr, "keytranslate_test: FAILED\n");
        return 1;
    }
    printf("keytranslate_test: OK\n");
    return 0;
}
