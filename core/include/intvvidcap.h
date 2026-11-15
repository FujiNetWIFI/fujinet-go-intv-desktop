/*
 * intvvidcap -- the STIC's state as a frame left it, for the Video tab
 * (intvvidtab.h). Taken once per published frame on the emulator thread
 * (gfx_stic_tick in core/jzintv/desktop/gfx_desktop.c, right after the STIC
 * rendered the frame) while a tab exists, so a reader sees one consistent
 * frame while the game runs. A parked machine is filled live.
 *
 * Why one snapshot a frame is enough (checked against the staged
 * src/stic/stic.c): the STIC draws the WHOLE frame at once, in
 * stic_simulate_until at next_frame_render, from the registers, the BACKTAB
 * it fetched and GRAM as they stood then. Register and GRAM writes outside
 * the vertical-blank window are dropped (stic_is_stic_accessible,
 * stic_is_gmem_accessible), so nothing a program does mid-frame changes the
 * picture of that frame. There is no per-scanline register state to record,
 * unlike the Atari 2600 or the Astrocade. What does move mid-frame is the
 * BACKTAB fetch (a bus request every 16 scan lines); a parked machine shows
 * how far it got in busrq_count and fifo_wr_ptr.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTVVIDCAP_H
#define INTVVIDCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTVVID_W 160           /* the published frame (gfx_t.vid) */
#define INTVVID_H 200
#define INTVVID_CARD_COLS 20    /* the BACKTAB */
#define INTVVID_CARD_ROWS 12
#define INTVVID_CARDS (INTVVID_CARD_COLS * INTVVID_CARD_ROWS)
#define INTVVID_BT_X 0          /* where the BACKTAB's first card sits in the frame (no delay) */
#define INTVVID_BT_Y 4          /* 4 border lines above; each card row is 8 pixels, 16 lines */
#define INTVVID_CARD_LINES 16
#define INTVVID_GMEM_BYTES (0x200 * 8)

typedef struct intvvid_capture {
    uint32_t raw[0x40];         /* the STIC register file */
    uint8_t  gmem[INTVVID_GMEM_BYTES];   /* GROM then GRAM card bytes, 8 a card */
    uint16_t mob_bmp[8][16];    /* decoded 1bpp MOB bitmaps */
    uint16_t btab[INTVVID_CARDS];        /* the BACKTAB as the STIC saw it */
    uint16_t btab_sr[INTVVID_CARDS];     /* the BACKTAB as system RAM held it ($0200-$02EF) */
    uint8_t  vid[INTVVID_W * INTVVID_H]; /* palette indices of the published frame */
    uint8_t  palette[16][3];
    int      vid_enable;        /* the display was on (else the frame is dimmed) */
    int      mode, p_mode;      /* 0 colour stack, 1 foreground/background; p_mode takes effect next frame */
    int      gram_size;         /* 0 = 64 cards, 1 = 128, 2 = 256 */
    int      pal;               /* 0 NTSC, 1 PAL */
    uint16_t gram_mask;
    uint64_t cycle;             /* the machine clock at the capture (live: now) */
    uint64_t last_intrq, next_intrq;     /* the frame's INTRQ and the next one */
    int      busrq_count, fifo_wr_ptr;   /* BACKTAB fetch progress (words fetched = fifo_wr_ptr) */
    uint64_t frame;             /* frames captured since the tab asked */
    uint64_t serial;            /* 0: nothing captured yet */
    int      live;              /* filled from a parked machine, not taken at a frame */
} intvvid_capture;

/* Frames are captured only while at least one tab wants them (a count:
 * enable(1) from each tab, enable(0) when it is freed). Thread-safe. */
void intv_host_capture_enable(int on);

/* Copies the latest captured frame into dst iff its serial differs from
 * *serial_inout, updates it and returns 1; 0 and dst untouched otherwise.
 * *serial_inout = 0 forces a copy once something has been captured. */
int intv_host_capture_get(intvvid_capture *dst, uint64_t *serial_inout);

/* Fills dst from the machine as it stands. Only for a machine parked by the
 * debugger (the emulator thread is then not running the STIC); it reads the
 * live structs. Sets live = 1. The serial is left as the last capture's. */
void intv_host_capture_fill(intvvid_capture *dst);

#ifdef __cplusplus
}
#endif

#endif
