/*
 * intv_host -- see intv_host.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "intv_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * cfg/cfg.h's cfg_t embeds every peripheral's struct directly (not through
 * pointers), so -- like jzintv.c itself -- every one of those headers has to
 * be visible before cfg.h is, in the same order jzintv.c uses them.
 */
#include "config.h"
#include "lzoe/lzoe.h"
#include "file/file.h"
#include "periph/periph.h"
#include "cp1600/cp1600.h"
#include "mem/mem.h"
#include "ecs/ecs.h"
#include "icart/icart.h"
#include "bincfg/bincfg.h"
#include "bincfg/legacy.h"
#include "pads/pads.h"
#include "pads/pads_cgc.h"
#include "pads/pads_intv2pc.h"
#include "avi/avi.h"
#include "gfx/gfx.h"
#include "gfx/palette.h"
#include "snd/snd.h"
#include "ay8910/ay8910.h"
#include "demo/demo.h"
#include "stic/stic.h"
#include "speed/speed.h"
#include "debug/debug_.h"
#include "debug/debug_if.h"
#include "event/event.h"
#include "ivoice/ivoice.h"
#include "jlp/jlp.h"
#include "fujinet/fujinet.h"
#include "locutus/locutus_adapt.h"
#include "cheat/cheat.h"
#include "cfg/mapping.h"
#include "cfg/cfg.h"

#include "roms_embedded.h"

extern int jzintv_entry_point(int argc, char *argv[]);

static pthread_t s_thread;
static int       s_running = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* argv jzintv_entry_point() reads for the life of the thread; kept alive on
 * the heap for exactly that reason (a stack copy in intv_host_start would be
 * gone the instant it returns). Freed once the thread has been joined. */
static char **s_argv;
static int    s_argc;

static int write_file_if_missing(const char *path, const unsigned char *data,
                                 size_t size)
{
    FILE *f = fopen(path, "rb");
    if (f)
    {
        fclose(f);
        return 0; /* Already present -- an imported ROM always wins. */
    }

    f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "intv_host: could not create %s\n", path);
        return -1;
    }
    const size_t written = fwrite(data, 1, size, f);
    fclose(f);
    if (written != size)
    {
        fprintf(stderr, "intv_host: short write to %s\n", path);
        return -1;
    }
    return 0;
}

int intv_host_provision_roms(const char *rom_dir)
{
    char path[4096];
    int rc = 0;

    for (int i = 0; i < intv_embedded_rom_count; i++)
    {
        const intv_embedded_rom *rom = &intv_embedded_roms[i];
        snprintf(path, sizeof(path), "%s/%s", rom_dir, rom->name);
        if (write_file_if_missing(path, rom->data, rom->size) != 0)
            rc = -1;
    }
    return rc;
}

static void *thread_main(void *arg)
{
    (void)arg;
    jzintv_entry_point(s_argc, s_argv);

    pthread_mutex_lock(&s_lock);
    s_running = 0;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

static void free_argv(void)
{
    if (!s_argv)
        return;
    for (int i = 0; i < s_argc; i++)
        free(s_argv[i]);
    free(s_argv);
    s_argv = NULL;
    s_argc = 0;
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d)
    {
        fprintf(stderr, "intv_host: out of memory\n");
        abort();
    }
    return d;
}

int intv_host_start(const intv_host_opts *opts)
{
    char exec_path[4096], grom_path[4096], fujinet_arg[256], e_arg[4160],
         g_arg[4160];
    FILE *f;

    pthread_mutex_lock(&s_lock);
    if (s_running)
    {
        pthread_mutex_unlock(&s_lock);
        fprintf(stderr, "intv_host: a session is already running\n");
        return -1;
    }
    pthread_mutex_unlock(&s_lock);

    if (!opts || !opts->rom_dir)
    {
        fprintf(stderr, "intv_host: rom_dir is required\n");
        return -1;
    }

    if (intv_host_provision_roms(opts->rom_dir) != 0)
        return -1;

    snprintf(exec_path, sizeof(exec_path), "%s/exec.bin", opts->rom_dir);
    snprintf(grom_path, sizeof(grom_path), "%s/grom.bin", opts->rom_dir);
    f = fopen(exec_path, "rb");
    if (!f)
    {
        fprintf(stderr, "intv_host: %s not found (import a system ROM, or "
                        "build -DWITH_INTV_ROMS=ON for local testing)\n",
                exec_path);
        return -1;
    }
    fclose(f);
    f = fopen(grom_path, "rb");
    if (!f)
    {
        fprintf(stderr, "intv_host: %s not found\n", grom_path);
        return -1;
    }
    fclose(f);

    snprintf(fujinet_arg, sizeof(fujinet_arg), "--fujinet=%s:%d",
             (opts->fujinet_host && opts->fujinet_host[0])
                 ? opts->fujinet_host : "127.0.0.1",
             opts->fujinet_port > 0 ? opts->fujinet_port : 1985);
    snprintf(e_arg, sizeof(e_arg), "-e%s", exec_path);
    snprintf(g_arg, sizeof(g_arg), "-g%s", grom_path);

    free_argv();
    s_argc = 4;
    s_argv = calloc((size_t)s_argc + 1, sizeof(*s_argv));
    if (!s_argv)
    {
        fprintf(stderr, "intv_host: out of memory\n");
        return -1;
    }
    s_argv[0] = xstrdup("fujinet-go-intv");
    s_argv[1] = xstrdup(e_arg);
    s_argv[2] = xstrdup(g_arg);
    s_argv[3] = xstrdup(fujinet_arg);
    s_argv[4] = NULL;

    pthread_mutex_lock(&s_lock);
    s_running = 1;
    pthread_mutex_unlock(&s_lock);

    if (pthread_create(&s_thread, NULL, thread_main, NULL) != 0)
    {
        fprintf(stderr, "intv_host: pthread_create failed\n");
        pthread_mutex_lock(&s_lock);
        s_running = 0;
        pthread_mutex_unlock(&s_lock);
        free_argv();
        return -1;
    }

    return 0;
}

void intv_host_stop(void)
{
    pthread_mutex_lock(&s_lock);
    const int running = s_running;
    pthread_mutex_unlock(&s_lock);

    if (!running)
        return;

    intv.do_exit = 1;
    pthread_join(s_thread, NULL);
    free_argv();
}

int intv_host_is_running(void)
{
    pthread_mutex_lock(&s_lock);
    const int running = s_running;
    pthread_mutex_unlock(&s_lock);
    return running;
}

/* ---- direct pad injection --------------------------------------------- */

/* Indexed by intv_pad_key; the value each key writes into pad_t.l[]/r[] on
 * press (0 on release, always). Copied verbatim from src/cfg/mapping.c's
 * PD0L_KP and PD0L_A entries -- W(pad0.l[N]) gives the array index (which
 * matches intv_pad_key's own numbering below the keypad digits), and the
 * or_mask[1] column gives this value. */
static const uint8_t key_codes[INTV_PAD_KEY_COUNT] = {
    [INTV_PAD_KEY_0]           = 0x48,
    [INTV_PAD_KEY_1]           = 0x81,
    [INTV_PAD_KEY_2]           = 0x41,
    [INTV_PAD_KEY_3]           = 0x21,
    [INTV_PAD_KEY_4]           = 0x82,
    [INTV_PAD_KEY_5]           = 0x42,
    [INTV_PAD_KEY_6]           = 0x22,
    [INTV_PAD_KEY_7]           = 0x84,
    [INTV_PAD_KEY_8]           = 0x44,
    [INTV_PAD_KEY_9]           = 0x24,
    [INTV_PAD_KEY_CLEAR]       = 0x88,
    [INTV_PAD_KEY_ENTER]       = 0x28,
    [INTV_PAD_ACTION_TOP]         = 0xA0,
    [INTV_PAD_ACTION_LOWER_LEFT]  = 0x60,
    [INTV_PAD_ACTION_LOWER_RIGHT] = 0xC0,
};

/* pad_t.l[]/r[] array index for each intv_pad_key -- KP0 is index 0, KP1-9
 * are 1-9, Clear is 10, Enter is 11, then the three action buttons at
 * 12-14. Matches mapping.c's W(pad0.l[N]) for each entry exactly. */
static const int key_index[INTV_PAD_KEY_COUNT] = {
    [INTV_PAD_KEY_0] = 0,
    [INTV_PAD_KEY_1] = 1, [INTV_PAD_KEY_2] = 2, [INTV_PAD_KEY_3] = 3,
    [INTV_PAD_KEY_4] = 4, [INTV_PAD_KEY_5] = 5, [INTV_PAD_KEY_6] = 6,
    [INTV_PAD_KEY_7] = 7, [INTV_PAD_KEY_8] = 8, [INTV_PAD_KEY_9] = 9,
    [INTV_PAD_KEY_CLEAR] = 10,
    [INTV_PAD_KEY_ENTER] = 11,
    [INTV_PAD_ACTION_TOP] = 12,
    [INTV_PAD_ACTION_LOWER_LEFT] = 13,
    [INTV_PAD_ACTION_LOWER_RIGHT] = 14,
};

/* pad_t.l[15]/r[15]'s value for each of the disc's 16 half-step positions,
 * clock position 0 = East, going clockwise (E, ENE, NE, NNE, N, ...).
 * Copied verbatim from mapping.c's PD0L_D and PD0R_D or_mask[1] column. */
static const uint32_t disc_codes[16] = {
    /*  0 E   */   1,
    /*  1 ENE */   3,
    /*  2 NE  */   2,
    /*  3 NNE */   6,
    /*  4 N   */   4,
    /*  5 NNW */  12,
    /*  6 NW  */   8,
    /*  7 WNW */  24,
    /*  8 W   */  16,
    /*  9 WSW */  48,
    /* 10 SW  */  32,
    /* 11 SSW */  96,
    /* 12 S   */  64,
    /* 13 SSE */ 192,
    /* 14 SE  */ 128,
    /* 15 ESE */ 129,
};

void intv_host_pad_key(intv_pad_side side, intv_pad_key key, int pressed)
{
    if (key < 0 || key >= INTV_PAD_KEY_COUNT)
        return;

    const uint32_t value = pressed ? key_codes[key] : 0;
    const int idx = key_index[key];

    if (side == INTV_PAD_LEFT)
        intv.pad0.l[idx] = value;
    else
        intv.pad0.r[idx] = value;
}

void intv_host_pad_disc(intv_pad_side side, int direction)
{
    uint32_t value = 0;
    if (direction >= 0 && direction < 16)
        value = disc_codes[direction];

    if (side == INTV_PAD_LEFT)
        intv.pad0.l[15] = value;
    else
        intv.pad0.r[15] = value;
}
