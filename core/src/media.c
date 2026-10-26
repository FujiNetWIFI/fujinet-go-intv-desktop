/*
 * media.c -- importing cartridge images onto FujiNet's SD host.
 *
 * Files land in the SD root so the CONFIG client shows them at the top of the
 * SD host with no navigation. FujiNet pushes .rom, .bin, .int and .itv files
 * to the console; a .cfg sibling (game.rom -> game.cfg) is pushed ahead of the
 * image and carries its mapper, so it travels with it. A zip is unpacked.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "session_internal.h"
#include "zipread.h"

#define MAX_IMAGE_SIZE (4u * 1024 * 1024)

const char *intvsession_sd_path(const intvsession *s)
{
    return s->fujinet_sd;
}

static const char *base_name(const char *path)
{
    const char *p = path + strlen(path);
    while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
    return p;
}

static int ext_is(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    size_t i;
    if (n < e) return 0;
    for (i = 0; i < e; i++)
        if (tolower((unsigned char)name[n - e + i]) != ext[i]) return 0;
    return 1;
}

static int is_cart_name(const char *name)
{
    return ext_is(name, ".rom") || ext_is(name, ".bin") ||
           ext_is(name, ".int") || ext_is(name, ".itv");
}

static int write_bytes(const char *path, const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return -1;
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* The .cfg sibling of an image, if there is one. */
static int cfg_sibling(const char *path, char *out, int outsz)
{
    const char *dot = strrchr(base_name(path), '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    struct stat st;

    if ((int)stem + 5 > outsz) return 0;
    memcpy(out, path, stem);
    memcpy(out + stem, ".cfg", 5);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

static int import_zip(intvsession *s, zipread *z, char *dest_out, int dest_sz)
{
    int i, n = 0;
    for (i = 0; i < zipread_count(z); i++) {
        const char *name = base_name(zipread_name(z, i));
        char dst[INTV_PATH_MAX * 2];
        uint8_t *data = NULL;
        uint32_t size = 0;

        if (!*name || (!is_cart_name(name) && !ext_is(name, ".cfg"))) continue;
        if (zipread_extract(z, i, &data, &size, MAX_IMAGE_SIZE) != 0) continue;
        snprintf(dst, sizeof dst, "%s/%s", s->fujinet_sd, name);
        if (write_bytes(dst, data, size) == 0) {
            if (!n) snprintf(dest_out, (size_t)dest_sz, "%s", dst);
            n++;
        }
        free(data);
    }
    return n;
}

int intvsession_import_cart_to_sd(intvsession *s, const char *src_path,
                                  char *dest_out, int dest_sz)
{
    const char *name;
    char cfg[INTV_PATH_MAX], cfg_dst[INTV_PATH_MAX * 2];
    zipread *z;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    name = base_name(src_path);
    if (!s->fujinet_sd[0] || mkdir_p(s->fujinet_sd) != 0) {
        session_set_error(s, "There is no SD folder to import %s into.", name);
        return -1;
    }
    z = zipread_open(src_path);
    if (z) {
        int n = import_zip(s, z, dest_out, dest_sz);
        zipread_close(z);
        if (!n) {
            session_set_error(s, "%s holds no .rom, .bin, .int or .itv image",
                              name);
            return -1;
        }
        return 0;
    }
    if (!is_cart_name(name) && !ext_is(name, ".cfg")) {
        session_set_error(s, "%s is not a cartridge image (.rom, .bin, .int, "
                          ".itv) or a zip of them", name);
        return -1;
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    if (cfg_sibling(src_path, cfg, sizeof cfg)) {
        snprintf(cfg_dst, sizeof cfg_dst, "%s/%s", s->fujinet_sd, base_name(cfg));
        if (copy_file(cfg, cfg_dst) != 0) {
            session_set_error(s, "Could not copy %s to %s", base_name(cfg),
                              s->fujinet_sd);
            return -1;
        }
    }
    return 0;
}
