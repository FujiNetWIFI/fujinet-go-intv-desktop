/*
 * intv_frame -- the frame store the desktop gfx backend publishes into and
 * frontends copy out of.
 *
 * Analogous to cocosession_copy_frame in fujinet-go-coco-desktop: the
 * emulator thread (through core/jzintv/desktop/gfx_desktop.c) publishes the
 * latest completed frame; a UI thread pulls it on its own vsync tick.
 * copy_frame copies the latest frame into dst (XRGB8888, tightly packed,
 * INTV_FRAME_WIDTH*INTV_FRAME_HEIGHT uint32) iff its serial differs from
 * *serial_inout, updates *serial_inout, and returns 1; returns 0 when the
 * frame is unchanged (dst untouched). Pass *serial_inout = 0 to force a copy
 * (e.g. first paint after a window map).
 *
 * The Intellivision's STIC frame geometry is fixed (unlike the CoCo's, which
 * varies with video mode), so unlike cocosession there is no w_out/h_out --
 * callers just use the macros below.
 *
 * Thread safety: intv_frame_publish is called from the emulator thread
 * (inside gfx_refresh); intv_frame_copy from any UI thread. Both take the
 * same mutex, so publish never races a partial copy.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_FRAME_H
#define INTV_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches gfx_t.vid's fixed 160x200 8bpp buffer (src/gfx/gfx.h). */
#define INTV_FRAME_WIDTH  160
#define INTV_FRAME_HEIGHT 200

/* Called by gfx_desktop.c with the raw palette-index buffer (row-major,
 * INTV_FRAME_WIDTH*INTV_FRAME_HEIGHT bytes, values 0-15 -- the STIC bakes
 * the border color into this buffer itself, see stic_push_vid in the staged
 * tree's src/stic/stic.c, so there is nothing extra to composite here) and
 * the current 16-color palette; expands through the palette into XRGB8888
 * and bumps the serial. vid_enabled selects the STIC's own on/off dimming,
 * same as the SDL backend's pal_on/pal_off (src/gfx/gfx_sdl2.c). */
void intv_frame_publish(const uint8_t *vid, const uint8_t palette[16][3],
                        int vid_enabled);

/* See the file header for the copy contract. */
int intv_frame_copy(uint32_t *dst, uint64_t *serial_inout);

/* Registers a callback fired at the end of intv_frame_publish, after the
 * new frame is visible to intv_frame_copy -- on the emulator thread, same
 * as publish itself. Unused by any desktop frontend (they pull frames on
 * their own vsync tick instead), but lets a presenter-style consumer (the
 * Android port's render thread, which has no vsync tick of its own to poll
 * on) block on a condvar and wake exactly once per published frame instead
 * of either busy-polling or free-running ahead of the emulator. Pass NULL
 * to clear. Not thread-safe against itself -- call once at startup. */
void intv_frame_set_publish_hook(void (*hook)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* INTV_FRAME_H */
