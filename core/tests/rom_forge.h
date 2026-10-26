/*
 * rom_forge.h -- images of a given size whose CRC32 is a chosen value.
 *
 * CRC32 is linear, so the last four bytes of any buffer can be set to reach any
 * value; that lets the tests hand the importer a "Color BASIC" it recognises
 * without a copy of the real, copyrighted ROM. zlib's own crc32() checks it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_ROM_FORGE_H
#define COCO_ROM_FORGE_H

#include <stdint.h>

static uint32_t T[256];
static uint8_t inv[256];        /* T's top bytes are a permutation */

static void crc_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        T[i] = c;
        inv[c >> 24] = (uint8_t)i;
    }
}

/* Filler (seeded) ending in the four bytes that make its CRC32
 * `want`: pick the four table entries whose sum is the final register, then
 * the bytes that select them from the register the filler leaves. */
static void forge(uint8_t *buf, uint32_t size, uint32_t seed, uint32_t want)
{
    uint32_t r = 0xffffffffu, v = ~want;
    uint8_t idx[4];
    for (uint32_t i = 0; i < size - 4; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
        r = (r >> 8) ^ T[(r ^ buf[i]) & 0xff];
    }
    idx[3] = inv[v >> 24];         v ^= T[idx[3]];
    idx[2] = inv[(v >> 16) & 0xff]; v ^= T[idx[2]] >> 8;
    idx[1] = inv[(v >> 8) & 0xff];  v ^= T[idx[1]] >> 16;
    idx[0] = inv[v & 0xff];
    for (int k = 0; k < 4; k++) {
        buf[size - 4 + k] = (uint8_t)(idx[k] ^ (r & 0xff));
        r = (r >> 8) ^ T[idx[k]];
    }
}

#endif
