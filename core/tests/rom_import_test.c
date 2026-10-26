/*
 * rom_import_test -- BIOS import (file, zip, folder, MAME's ECS slices) and
 * Import Cartridge to SD, without a Mattel ROM: each image is filler of the
 * real ROM's size whose last four bytes are forged so its CRC32 is the real
 * ROM's. zlib's own crc32() checks the forgery.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "intvsession.h"
#include "rom_forge.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

enum { EXEC, GROM, ECS, SLICE0, SLICE1, SLICE2, N_ROM };
static const struct { const char *name; uint32_t size, crc; } k_rom[N_ROM] = {
    { "exec.bin", 0x2000, 0xcbce86f7u },
    { "grom.bin", 0x0800, 0x683a4158u },
    { "ecs.bin",  0x6000, 0xea790a06u },
    { "ecs_rom.20", 0x2000, 0x5f9d05e5u },
    { "ecs_rom.70", 0x2000, 0x46bb1f48u },
    { "ecs_rom.e0", 0x2000, 0xc2ebcd90u },
};
#define MAX_SIZE 0x6000

static void write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void make_dir(const char *path)
{
#if defined(_WIN32)
    mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

typedef struct { const char *name; const uint8_t *data; uint32_t size; int deflate; } zentry;

/* A zip of stored and raw-deflated entries, the way MAME's sets come. */
static void write_zip(const char *path, const zentry *e, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t off[8], csz[8], crc[8];
    uint8_t *comp[8];
    if (!f || n > 8) { perror(path); exit(1); }
    for (int i = 0; i < n; i++) {
        crc[i] = (uint32_t)crc32(0, e[i].data, e[i].size);
        if (e[i].deflate) {
            z_stream st;
            memset(&st, 0, sizeof st);
            comp[i] = malloc(e[i].size + 1024);
            deflateInit2(&st, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            st.next_in = (Bytef *)e[i].data;
            st.avail_in = e[i].size;
            st.next_out = comp[i];
            st.avail_out = e[i].size + 1024;
            deflate(&st, Z_FINISH);
            csz[i] = (uint32_t)st.total_out;
            deflateEnd(&st);
        } else {
            comp[i] = malloc(e[i].size);
            memcpy(comp[i], e[i].data, e[i].size);
            csz[i] = e[i].size;
        }
        off[i] = (uint32_t)ftell(f);
        put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, e[i].deflate ? 8 : 0);
        put16(f, 0); put16(f, 0); put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0);
        fputs(e[i].name, f);
        fwrite(comp[i], 1, csz[i], f);
    }
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; i < n; i++) {
        put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0);
        put16(f, e[i].deflate ? 8 : 0); put16(f, 0); put16(f, 0);
        put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0); put16(f, 0);
        put16(f, 0); put16(f, 0); put32(f, 0); put32(f, off[i]);
        fputs(e[i].name, f);
        free(comp[i]);
    }
    uint32_t cd_end = (uint32_t)ftell(f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (unsigned)n); put16(f, (unsigned)n);
    put32(f, cd_end - cd); put32(f, cd); put16(f, 0);
    fclose(f);
}


static uint8_t g_img[N_ROM][MAX_SIZE];

static intvsession *open_session(const char *dir, const char *name)
{
    char cfg[600], data[600], roms[600];
    intvsession_paths paths;
    intvsession *s;
    int i;
    snprintf(cfg, sizeof cfg, "%s/%s-config", dir, name);
    snprintf(data, sizeof data, "%s/%s-data", dir, name);
    snprintf(roms, sizeof roms, "%s/%s-roms", dir, name);
    make_dir(roms);
#if defined(_WIN32)
    _putenv_s("INTV_ROM_DIR", roms);
#else
    setenv("INTV_ROM_DIR", roms, 1);
#endif
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    s = intvsession_new(&paths);
    /* A WITH_INTV_ROMS=ON build materialised its own; start from none. */
    for (i = 0; i < 3; i++) {
        char p[700];
        snprintf(p, sizeof p, "%s/%s", roms, k_rom[i].name);
        remove(p);
    }
    return s;
}

static int roms_have(intvsession *s, int i)
{
    return intvsession_rom_item_available(s, i);
}

static int file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int)st.st_size : -1;
}

int main(void)
{
    char dir[600], p[900], q[900], msg[256];
    intvsession *s;
    int i;

    crc_tables();
    for (i = 0; i < N_ROM; i++) {
        forge(g_img[i], k_rom[i].size, 0x1000u + (uint32_t)i, k_rom[i].crc);
        CHECK((uint32_t)crc32(0, g_img[i], k_rom[i].size) == k_rom[i].crc, "forgery");
    }

    test_tmp_template(dir, sizeof dir, "intv-rom-import-");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    /* ---- a single file, under a name nobody asked for ---- */
    s = open_session(dir, "a");
    CHECK(s != NULL, "session a");
    CHECK(!intvsession_has_system_roms(s), "no ROMs at first");
    snprintf(p, sizeof p, "%s/EXECUTIVE.ROM", dir);
    write_file(p, g_img[EXEC], k_rom[EXEC].size);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == 0, "import exec");
    CHECK(strstr(msg, "Now available") && strstr(msg, "exec.bin"), "exec message");
    CHECK(roms_have(s, 0) && !roms_have(s, 1), "exec only");
    CHECK(!intvsession_has_system_roms(s), "exec alone is not enough");
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == -1 ||
          intvsession_rom_import(s, p, msg, sizeof msg) == 0, "re-import is harmless");

    /* junk and wrong sizes */
    write_file(p, g_img[GROM], 100);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == -1, "short file rejected");
    CHECK(strstr(msg, "No Intellivision ROM"), "nothing-found message");
    memset(g_img[GROM] + 0, 0xA5, 8);   /* breaks the CRC */
    write_file(p, g_img[GROM], k_rom[GROM].size);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == -1, "wrong CRC rejected");
    forge(g_img[GROM], k_rom[GROM].size, 0x1001u, k_rom[GROM].crc);

    write_file(p, g_img[GROM], k_rom[GROM].size);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == 0, "import grom");
    CHECK(intvsession_has_system_roms(s), "both system ROMs");
    snprintf(q, sizeof q, "%s/a-roms/grom.bin", dir);
    CHECK(file_size(q) == 0x800, "grom.bin written at its size");
    intvsession_free(s);

    /* ---- a zip, one stored and one deflated ---- */
    s = open_session(dir, "b");
    {
        zentry e[2] = {
            { "exec.bin", g_img[EXEC], k_rom[EXEC].size, 0 },
            { "ro-3-9503-003.u21", g_img[GROM], k_rom[GROM].size, 1 },
        };
        snprintf(p, sizeof p, "%s/intv.zip", dir);
        write_zip(p, e, 2);
    }
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == 0, "import zip");
    CHECK(intvsession_has_system_roms(s), "zip gave both");
    CHECK(strstr(msg, "exec.bin") && strstr(msg, "grom.bin"), "zip message");
    intvsession_free(s);

    /* ---- a folder, with MAME's three ECS slices and an ecs.bin-less set ---- */
    s = open_session(dir, "c");
    snprintf(p, sizeof p, "%s/folder", dir);
    make_dir(p);
    snprintf(q, sizeof q, "%s/folder/sub", dir);
    make_dir(q);
    for (i = SLICE0; i <= SLICE2; i++) {
        snprintf(q, sizeof q, "%s/folder/sub/%s", dir, k_rom[i].name);
        write_file(q, g_img[i], k_rom[i].size);
    }
    CHECK(!roms_have(s, 2), "no ECS yet");
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == 0, "import slices");
    CHECK(roms_have(s, 2) && intvsession_has_ecs_rom(s), "slices joined");
    snprintf(q, sizeof q, "%s/c-roms/ecs.bin", dir);
    CHECK(file_size(q) == 0x6000, "ecs.bin is 24K");
    {
        uint8_t back[0x6000];
        FILE *f = fopen(q, "rb");
        CHECK(f && fread(back, 1, sizeof back, f) == sizeof back, "read ecs.bin");
        if (f) fclose(f);
        CHECK(memcmp(back, g_img[SLICE0], 0x2000) == 0 &&
              memcmp(back + 0x2000, g_img[SLICE1], 0x2000) == 0 &&
              memcmp(back + 0x4000, g_img[SLICE2], 0x2000) == 0, "slices in order");
    }
    /* an empty folder */
    snprintf(p, sizeof p, "%s/empty", dir);
    make_dir(p);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == -1, "empty folder");
    intvsession_free(s);

    /* ---- the ECS as one 24K file ---- */
    s = open_session(dir, "d");
    snprintf(p, sizeof p, "%s/ecs-whole.bin", dir);
    write_file(p, g_img[ECS], k_rom[ECS].size);
    CHECK(intvsession_rom_import(s, p, msg, sizeof msg) == 0, "import ecs.bin");
    CHECK(roms_have(s, 2), "ecs.bin available");
    CHECK(intvsession_rom_item_count() == 3 && intvsession_rom_item(0) &&
          intvsession_rom_item(3) == NULL, "item list");
    intvsession_free(s);

    /* ---- Import Cartridge to SD ---- */
    s = open_session(dir, "e");
    {
        char dest[900];
        snprintf(p, sizeof p, "%s/game.rom", dir);
        write_file(p, g_img[EXEC], 1000);
        snprintf(q, sizeof q, "%s/game.cfg", dir);
        write_file(q, (const uint8_t *)"mapper=1\n", 9);
        CHECK(intvsession_import_cart_to_sd(s, p, dest, sizeof dest) == 0, "cart to SD");
        CHECK(file_size(dest) == 1000, "cart copied");
        snprintf(q, sizeof q, "%s/game.cfg", intvsession_sd_path(s));
        CHECK(file_size(q) == 9, "cfg sibling copied");

        snprintf(p, sizeof p, "%s/notes.txt", dir);
        write_file(p, (const uint8_t *)"x", 1);
        CHECK(intvsession_import_cart_to_sd(s, p, dest, sizeof dest) == -1,
              "non-cartridge refused");
        CHECK(strstr(intvsession_last_error(s), "not a cartridge"), "refusal reason");

        {
            zentry e[2] = {
                { "dir/zipped.int", g_img[GROM], 500, 1 },
                { "readme.txt", g_img[GROM], 20, 0 },
            };
            snprintf(p, sizeof p, "%s/carts.zip", dir);
            write_zip(p, e, 2);
        }
        CHECK(intvsession_import_cart_to_sd(s, p, dest, sizeof dest) == 0, "zip to SD");
        snprintf(q, sizeof q, "%s/zipped.int", intvsession_sd_path(s));
        CHECK(file_size(q) == 500, "zip entry unpacked into the SD root");
        snprintf(q, sizeof q, "%s/readme.txt", intvsession_sd_path(s));
        CHECK(file_size(q) == -1, "non-cartridge zip entry skipped");
    }
    intvsession_free(s);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("rom_import_test: ok\n");
    return 0;
}
