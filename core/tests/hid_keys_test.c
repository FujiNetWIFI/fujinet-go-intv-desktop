/*
 * hid_keys_test -- the native-key-code -> USB HID usage tables in
 * core/src/hid_keys.c.
 *
 * WHY THIS TEST MATTERS MORE THAN ITS SIZE SUGGESTS: two of the three tables
 * describe hardware this test never runs on. The maintainer has no Windows
 * machine (see cmake/toolchains/mingw-w64.cmake's own header) and CI's macOS
 * job cannot press keys, so a transposed digit in either table would
 * otherwise stay invisible until a user reported that some key would not
 * map -- which is exactly how the bug these tables fix was found. Keeping
 * hid_keys.c host-independent is what lets Linux CI check all three.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "intvsession.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "hid_keys_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static void eq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "hid_keys_test: FAILED: %s (got 0x%02X, want 0x%02X)\n",
                what, (unsigned)got, (unsigned)want);
        failed = 1;
    }
}

/* No two distinct native codes may name the same HID usage. A duplicated
 * entry is the likeliest way to get one of these tables wrong, and it is
 * silent otherwise: two physical keys would collapse onto one keysym, so
 * binding one would steal the other. */
static void check_injective(const char *what,
                            uint32_t (*fn)(unsigned), unsigned limit)
{
    unsigned char seen[256];
    memset(seen, 0, sizeof(seen));
    for (unsigned c = 0; c < limit; c++) {
        const uint32_t u = fn(c);
        if (u == 0)
            continue;
        if (u > INTVSESSION_HID_USAGE_MAX) {
            fprintf(stderr, "hid_keys_test: FAILED: %s maps %u past the "
                            "usage page (0x%02X)\n", what, c, (unsigned)u);
            failed = 1;
            continue;
        }
        if (seen[u]) {
            fprintf(stderr, "hid_keys_test: FAILED: %s maps two codes to "
                            "usage 0x%02X\n", what, (unsigned)u);
            failed = 1;
        }
        seen[u] = 1;
    }
}

static uint32_t win_plain(unsigned sc)
{
    return intvsession_hid_from_win_scancode(sc, 0);
}

static uint32_t win_ext(unsigned sc)
{
    return intvsession_hid_from_win_scancode(sc, 1);
}

int main(void)
{
    /* ---- Windows: PS/2 set 1, the code in WM_KEYDOWN's lParam ---------- */
    eq("win A",            win_plain(0x1E), 0x04);
    eq("win 1",            win_plain(0x02), 0x1E);
    eq("win Escape",       win_plain(0x01), 0x29);
    eq("win F1",           win_plain(0x3B), 0x3A);
    eq("win F12",          win_plain(0x58), 0x45);
    /* F13-F24 are the band's whole point: real keys with no cap on a normal
     * keyboard, and the kind an adapter emits. */
    eq("win F13",          win_plain(0x64), 0x68);
    eq("win F24",          win_plain(0x76), 0x73);
    eq("win Left Ctrl",    win_plain(0x1D), 0xE0);
    eq("win Return",       win_plain(0x1C), 0x28);
    /* The extended bit is the whole difference between these two pairs --
     * getting it wrong silently merges Enter with Keypad Enter. */
    eq("win Keypad Enter", win_ext(0x1C),   0x58);
    eq("win Right Ctrl",   win_ext(0x1D),   0xE4);
    eq("win Right Alt",    win_ext(0x38),   0xE6);
    eq("win Keypad /",     win_ext(0x35),   0x54);
    eq("win Keypad *",     win_plain(0x37), 0x55);
    eq("win Insert",       win_ext(0x52),   0x49);
    eq("win Up Arrow",     win_ext(0x48),   0x52);
    check("win out-of-range scancode is unmapped",
          win_plain(0x180) == 0 && win_ext(0x180) == 0);
    check("win scancode 0 is unmapped",
          win_plain(0) == 0 && win_ext(0) == 0);

    /* ---- Linux: evdev codes (GTK/Qt report these biased by 8) ---------- */
    eq("evdev A",            intvsession_hid_from_evdev(30),  0x04);
    eq("evdev Escape",       intvsession_hid_from_evdev(1),   0x29);
    eq("evdev F1",           intvsession_hid_from_evdev(59),  0x3A);
    eq("evdev F13",          intvsession_hid_from_evdev(183), 0x68);
    eq("evdev F24",          intvsession_hid_from_evdev(194), 0x73);
    eq("evdev Right Ctrl",   intvsession_hid_from_evdev(97),  0xE4);
    eq("evdev Keypad Enter", intvsession_hid_from_evdev(96),  0x58);
    eq("evdev Insert",       intvsession_hid_from_evdev(110), 0x49);
    check("evdev out-of-range code is unmapped",
          intvsession_hid_from_evdev(500) == 0);

    /* ---- macOS: NSEvent.keyCode (Apple's own virtual key set) ---------- */
    eq("macos A",            intvsession_hid_from_macos_keycode(0x00), 0x04);
    eq("macos Escape",       intvsession_hid_from_macos_keycode(0x35), 0x29);
    eq("macos F1",           intvsession_hid_from_macos_keycode(0x7A), 0x3A);
    eq("macos F13",          intvsession_hid_from_macos_keycode(0x69), 0x68);
    eq("macos Right Ctrl",   intvsession_hid_from_macos_keycode(0x3E), 0xE4);
    eq("macos Keypad Enter", intvsession_hid_from_macos_keycode(0x4C), 0x58);
    eq("macos Up Arrow",     intvsession_hid_from_macos_keycode(0x7E), 0x52);
    check("macos out-of-range code is unmapped",
          intvsession_hid_from_macos_keycode(200) == 0);

    /* ---- the three tables must agree with each other -------------------
     * Same physical key, three host numberings, one usage. This is what
     * makes a binding made on one platform mean the same thing on another,
     * and it cross-checks each table against the other two. */
    {
        static const struct {
            const char *key;
            unsigned win; int win_ext;
            unsigned evdev;
            unsigned macos;
        } common[] = {
            { "A",            0x1E, 0, 30,  0x00 },
            { "Z",            0x2C, 0, 44,  0x06 },
            { "1",            0x02, 0, 2,   0x12 },
            { "Space",        0x39, 0, 57,  0x31 },
            { "Tab",          0x0F, 0, 15,  0x30 },
            { "Escape",       0x01, 0, 1,   0x35 },
            { "F1",           0x3B, 0, 59,  0x7A },
            { "F12",          0x58, 0, 88,  0x6F },
            { "F13",          0x64, 0, 183, 0x69 },
            { "Left Shift",   0x2A, 0, 42,  0x38 },
            { "Right Shift",  0x36, 0, 54,  0x3C },
            { "Keypad 5",     0x4C, 0, 76,  0x57 },
            { "Keypad Enter", 0x1C, 1, 96,  0x4C },
            { "Keypad /",     0x35, 1, 98,  0x4B },
            { "Right Ctrl",   0x1D, 1, 97,  0x3E },
            { "Up Arrow",     0x48, 1, 103, 0x7E },
            { "Home",         0x47, 1, 102, 0x73 },
        };
        for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
            char what[96];
            const uint32_t w = intvsession_hid_from_win_scancode(
                common[i].win, common[i].win_ext);
            const uint32_t e = intvsession_hid_from_evdev(common[i].evdev);
            const uint32_t m = intvsession_hid_from_macos_keycode(
                common[i].macos);
            snprintf(what, sizeof(what),
                    "%s agrees across win/evdev/macos", common[i].key);
            if (w == 0 || w != e || e != m) {
                fprintf(stderr, "hid_keys_test: FAILED: %s (win 0x%02X, "
                                "evdev 0x%02X, macos 0x%02X)\n",
                        what, (unsigned)w, (unsigned)e, (unsigned)m);
                failed = 1;
            }
        }
    }

    check_injective("win_set1",    win_plain, 256);
    check_injective("win_set1_e0", win_ext,   256);
    check_injective("evdev",       intvsession_hid_from_evdev, 256);
    check_injective("macos",       intvsession_hid_from_macos_keycode, 256);

    /* ---- the band wrapper ---------------------------------------------- */
    eq("keysym_from_hid wraps into the band",
       intvsession_keysym_from_hid(0x68), INTVSESSION_KEYSYM_HID_BASE + 0x68);
    check("keysym_from_hid(0) is 0 (nothing to bind)",
          intvsession_keysym_from_hid(0) == 0);
    check("keysym_from_hid rejects a usage past the keyboard page",
          intvsession_keysym_from_hid(INTVSESSION_HID_USAGE_MAX + 1) == 0);
    /* The bands must not overlap the curated symbols, or a key with a
     * curated symbol would be bindable under two different values and a
     * binding made through one path would not match the other. */
    check("the HID band sits past every curated keysym",
          INTVSESSION_KEYSYM_HID_BASE > INTVSESSION_KEYSYM_BACKSPACE &&
              INTVSESSION_KEYSYM_HID_BASE > 0x7E);
    check("the native band sits past the whole HID band",
          INTVSESSION_KEYSYM_NATIVE_BASE >
              INTVSESSION_KEYSYM_HID_BASE + INTVSESSION_HID_USAGE_MAX);

    /* ---- every usage any table can produce must name itself ------------
     * intvsession_keysym_name's contract is that dst is always written; a
     * Map mode prints whatever it returns, so a usage with no entry has to
     * synthesize a name rather than leave the buffer alone. */
    for (unsigned c = 0; c < 256; c++) {
        const uint32_t usages[4] = {
            win_plain(c), win_ext(c),
            intvsession_hid_from_evdev(c),
            intvsession_hid_from_macos_keycode(c)
        };
        for (int k = 0; k < 4; k++) {
            char name[64];
            if (usages[k] == 0)
                continue;
            memset(name, 'x', sizeof(name));
            if (intvsession_keysym_name(
                    intvsession_keysym_from_hid(usages[k]), name,
                    sizeof(name)) <= 0 || name[0] == 'x') {
                fprintf(stderr, "hid_keys_test: FAILED: usage 0x%02X has no "
                                "name\n", (unsigned)usages[k]);
                failed = 1;
            }
        }
    }

    if (failed) {
        fprintf(stderr, "hid_keys_test: FAILED\n");
        return 1;
    }
    printf("hid_keys_test: OK\n");
    return 0;
}
