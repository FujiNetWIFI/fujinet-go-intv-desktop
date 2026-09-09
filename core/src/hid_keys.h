/*
 * hid_keys -- private interface to core/src/hid_keys.c, for bindings.c's
 * name tables. The native-code -> HID-usage translators the frontends call
 * are public instead, declared in intvsession.h beside the keysym bands they
 * feed (INTVSESSION_KEYSYM_HID_BASE and friends).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_HID_KEYS_H
#define INTV_HID_KEYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable name for a USB HID Keyboard usage ID -- "F16", "Keypad A",
 * "International 3" -- using jzIntv's own vocabulary where it has one (see
 * core/jzintv-generated/src/event/event_tbl.inc, whose names these mirror so
 * a binding reads the same here as in a jzIntv hackfile). NULL for a usage
 * with no name, which is the caller's cue to synthesize one; callers must
 * handle that rather than print NULL. */
const char *intv_hid_usage_name(uint32_t usage);

#ifdef __cplusplus
}
#endif

#endif /* INTV_HID_KEYS_H */
