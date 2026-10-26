/*
 * zipread.c -- a minimal zip reader (stored and deflated entries) over zlib.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "zipread.h"

typedef struct {
    char *name;
    uint16_t method;
    uint32_t csize, usize, local_off;
} entry;

struct zipread {
    FILE *fp;
    entry *e;
    int n;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

zipread *zipread_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t tail[65536 + 22];
    long size, start;
    size_t got;
    int i, eocd = -1;
    uint32_t cd_off, cd_size, nent;
    uint8_t *cd;
    zipread *z;

    if (!fp)
        return NULL;
    if (fread(tail, 1, 4, fp) != 4 || rd32(tail) != 0x04034b50) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    start = size > (long)sizeof tail ? size - (long)sizeof tail : 0;
    fseek(fp, start, SEEK_SET);
    got = fread(tail, 1, sizeof tail, fp);
    for (i = (int)got - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        fclose(fp);
        return NULL;
    }
    nent = rd16(tail + eocd + 10);
    cd_size = rd32(tail + eocd + 12);
    cd_off = rd32(tail + eocd + 16);
    if (cd_size > 16u * 1024 * 1024 || (long)cd_off + (long)cd_size > size) {
        fclose(fp);
        return NULL;
    }
    cd = malloc(cd_size ? cd_size : 1);
    z = calloc(1, sizeof *z);
    if (!cd || !z) {
        free(cd);
        free(z);
        fclose(fp);
        return NULL;
    }
    fseek(fp, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, fp) != cd_size) {
        free(cd);
        free(z);
        fclose(fp);
        return NULL;
    }
    z->fp = fp;
    z->e = calloc(nent ? nent : 1, sizeof *z->e);
    {
        uint32_t off = 0, k;
        for (k = 0; z->e && k < nent && off + 46 <= cd_size; k++) {
            const uint8_t *h = cd + off;
            uint16_t nlen, xlen, clen;
            if (rd32(h) != 0x02014b50)
                break;
            nlen = rd16(h + 28);
            xlen = rd16(h + 30);
            clen = rd16(h + 32);
            if (off + 46u + nlen > cd_size)
                break;
            if (nlen && h[46 + nlen - 1] != '/') {   /* skip directories */
                entry *e = &z->e[z->n++];
                e->method = rd16(h + 10);
                e->csize = rd32(h + 20);
                e->usize = rd32(h + 24);
                e->local_off = rd32(h + 42);
                e->name = malloc(nlen + 1u);
                if (e->name) {
                    memcpy(e->name, h + 46, nlen);
                    e->name[nlen] = 0;
                }
            }
            off += 46u + nlen + xlen + clen;
        }
    }
    free(cd);
    return z;
}

void zipread_close(zipread *z)
{
    int i;
    if (!z)
        return;
    for (i = 0; i < z->n; i++)
        free(z->e[i].name);
    free(z->e);
    fclose(z->fp);
    free(z);
}

int zipread_count(zipread *z) { return z ? z->n : 0; }
const char *zipread_name(zipread *z, int i) { return z && i >= 0 && i < z->n && z->e[i].name ? z->e[i].name : ""; }
uint32_t zipread_size(zipread *z, int i) { return z && i >= 0 && i < z->n ? z->e[i].usize : 0; }

int zipread_extract(zipread *z, int i, uint8_t **data, uint32_t *size, uint32_t max)
{
    entry *e;
    uint8_t lh[30], *comp, *out;
    if (!z || i < 0 || i >= z->n)
        return -1;
    e = &z->e[i];
    if (e->usize > max || (e->method != 0 && e->method != 8))
        return -1;
    fseek(z->fp, (long)e->local_off, SEEK_SET);
    if (fread(lh, 1, 30, z->fp) != 30 || rd32(lh) != 0x04034b50)
        return -1;
    fseek(z->fp, (long)e->local_off + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    comp = malloc(e->csize ? e->csize : 1);
    out = malloc(e->usize ? e->usize : 1);
    if (!comp || !out || fread(comp, 1, e->csize, z->fp) != e->csize) {
        free(comp);
        free(out);
        return -1;
    }
    if (e->method == 0) {
        if (e->csize != e->usize) {
            free(comp);
            free(out);
            return -1;
        }
        memcpy(out, comp, e->usize);
    }
    else {
        z_stream st;
        int rc;
        memset(&st, 0, sizeof st);
        if (inflateInit2(&st, -MAX_WBITS) != Z_OK) {
            free(comp);
            free(out);
            return -1;
        }
        st.next_in = comp;
        st.avail_in = e->csize;
        st.next_out = out;
        st.avail_out = e->usize;
        rc = inflate(&st, Z_FINISH);
        inflateEnd(&st);
        if (rc != Z_STREAM_END || st.total_out != e->usize) {
            free(comp);
            free(out);
            return -1;
        }
    }
    free(comp);
    *data = out;
    *size = e->usize;
    return 0;
}
