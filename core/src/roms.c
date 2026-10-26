/*
 * roms.c -- Intellivision system ROM import and availability.
 *
 * jzIntv is given exact paths for exec.bin, grom.bin and ecs.bin, so a dump
 * called EXEC.BIN or ro-3-9503-003.u21 is invisible to it. Import therefore
 * recognises each known dump by size and CRC32, whatever the file is called,
 * and writes it under the name jzIntv looks for. The input can be a file, a
 * zip (MAME's intv set works) or a folder, as in fujinet-go-coco-desktop.
 * ecs.bin may also arrive as MAME's three 8K slices, which are joined.
 * See COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"
#include "zipread.h"

#define MAX_ROM_SIZE 0x6000u
#define ECS_SLICE    0x2000u

static const intv_rom_item k_items[] = {
    { "Executive ROM (exec.bin)", "System ROMs" },
    { "Graphics ROM (grom.bin)",  "System ROMs" },
    { "ECS ROM (ecs.bin)",        "Entertainment Computer System" },
};
#define N_ITEMS ((int)(sizeof k_items / sizeof k_items[0]))

static const char *const k_item_file[N_ITEMS] = { "exec.bin", "grom.bin", "ecs.bin" };
static const uint32_t k_item_size[N_ITEMS] = { 0x2000, 0x800, 0x6000 };

static const struct {
    uint32_t size;
    uint32_t crc32;
    int item;
} k_dumps[] = {
    { 0x2000, 0xcbce86f7u, 0 },   /* Mattel EXEC */
    { 0x2000, 0xea552a22u, 0 },   /* Sears Super Video Arcade EXEC */
    { 0x0800, 0x683a4158u, 1 },   /* GROM */
    { 0x6000, 0xea790a06u, 2 },   /* ECS, the three slices in one */
};
#define N_DUMPS ((int)(sizeof k_dumps / sizeof k_dumps[0]))

/* MAME's three ECS ROMs: the 8K pieces at $2000, $7000 and $E000. */
static const uint32_t k_slice_crc[3] = { 0x5f9d05e5u, 0x46bb1f48u, 0xc2ebcd90u };

/* CRC32 (IEEE, the zip/MAME polynomial), table-free. */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

static int size_wanted(uint32_t size)
{
    int i;
    for (i = 0; i < N_DUMPS; i++)
        if (k_dumps[i].size == size) return 1;
    return size == ECS_SLICE;
}

static int item_present(const char *dir, int item)
{
    char path[INTV_PATH_MAX + 32];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, k_item_file[item]);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           (uint32_t)st.st_size == k_item_size[item];
}

int intvsession_has_system_roms(const intvsession *s)
{
    return item_present(s->roms_dir, 0) && item_present(s->roms_dir, 1);
}

int intvsession_has_ecs_rom(const intvsession *s)
{
    return item_present(s->roms_dir, 2);
}

int intvsession_rom_item_count(void)
{
    return N_ITEMS;
}

const intv_rom_item *intvsession_rom_item(int i)
{
    return i >= 0 && i < N_ITEMS ? &k_items[i] : NULL;
}

int intvsession_rom_item_available(const intvsession *s, int i)
{
    return s && i >= 0 && i < N_ITEMS && item_present(s->roms_dir, i);
}

typedef struct {
    intvsession *s;
    int imported;
    int got[N_ITEMS];
    uint8_t *slice[3];          /* ECS pieces seen so far */
} import_ctx;

static int write_rom(import_ctx *ctx, int item, const uint8_t *data, uint32_t size)
{
    char path[INTV_PATH_MAX + 32], tmp[INTV_PATH_MAX + 40];
    FILE *f;
    int ok;

    mkdir_p(ctx->s->roms_dir);
    snprintf(path, sizeof(path), "%s/%s", ctx->s->roms_dir, k_item_file[item]);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    if (ok) {
#if defined(_WIN32)
        remove(path); /* rename() will not replace an existing file here */
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (!ctx->got[item]) {
        ctx->got[item] = 1;
        ctx->imported++;
    }
    return 1;
}

static void join_slices(import_ctx *ctx)
{
    uint8_t *all;
    int k;

    if (ctx->got[2] || !ctx->slice[0] || !ctx->slice[1] || !ctx->slice[2])
        return;
    all = malloc(k_item_size[2]);
    if (!all) return;
    for (k = 0; k < 3; k++)
        memcpy(all + (size_t)k * ECS_SLICE, ctx->slice[k], ECS_SLICE);
    write_rom(ctx, 2, all, k_item_size[2]);
    free(all);
}

/* One candidate image. Written through a temporary so a failed write leaves
 * the old ROM. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    uint32_t crc;
    int i, k;

    if (!size_wanted(size)) return;
    crc = crc32_of(data, size);
    for (i = 0; i < N_DUMPS; i++)
        if (k_dumps[i].size == size && k_dumps[i].crc32 == crc) {
            write_rom(ctx, k_dumps[i].item, data, size);
            return;
        }
    if (size != ECS_SLICE) return;
    for (k = 0; k < 3; k++)
        if (crc == k_slice_crc[k] && !ctx->slice[k]) {
            ctx->slice[k] = malloc(ECS_SLICE);
            if (ctx->slice[k]) memcpy(ctx->slice[k], data, ECS_SLICE);
            join_slices(ctx);
        }
}

static uint8_t *read_whole(const char *path, uint32_t want)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    uint8_t extra;
    size_t n;

    if (!f) return NULL;
    buf = malloc(want);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf, 1, want, f);
    if (n != want || fread(&extra, 1, 1, f) == 1) { /* shorter or longer */
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_zip(import_ctx *ctx, zipread *z)
{
    int i;
    for (i = 0; i < zipread_count(z); i++) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        if (!size_wanted(zipread_size(z, i))) continue;
        if (zipread_extract(z, i, &data, &size, MAX_ROM_SIZE) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    char path[INTV_PATH_MAX * 2];
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;

    if (depth > 8 || stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        import_dir(ctx, path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode)) return;
    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z);
        zipread_close(z);
        return;
    }
    if (size_wanted((uint32_t)st.st_size)) {
        uint8_t *data = read_whole(path, (uint32_t)st.st_size);
        if (data) {
            take(ctx, data, (uint32_t)st.st_size);
            free(data);
        }
    }
}

int intvsession_rom_import(intvsession *s, const char *path, char *msg, int msgsz)
{
    import_ctx ctx;
    int before[N_ITEMS];
    int i, len = 0;

    if (!s) return -1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.s = s;
    for (i = 0; i < N_ITEMS; i++)
        before[i] = item_present(s->roms_dir, i);
    if (path && *path)
        import_path(&ctx, path, 0);
    for (i = 0; i < 3; i++)
        free(ctx.slice[i]);

    if (msg && msgsz > 0) {
        msg[0] = '\0';
        for (i = 0; i < N_ITEMS; i++) {
            if (!before[i] && item_present(s->roms_dir, i) && len < msgsz)
                len += snprintf(msg + len, (size_t)(msgsz - len), "%s%s",
                                len ? ", " : "Now available: ", k_items[i].name);
        }
        if (!len) {
            if (ctx.imported)
                snprintf(msg, (size_t)msgsz,
                         "Imported %d file%s; nothing new is complete yet",
                         ctx.imported, ctx.imported == 1 ? "" : "s");
            else
                snprintf(msg, (size_t)msgsz,
                         "No Intellivision ROM was found in %s", path ? path : "");
        }
    }
    return ctx.imported > 0 ? 0 : -1;
}
