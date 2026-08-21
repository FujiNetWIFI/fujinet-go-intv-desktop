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
#include "intv_audio.h"

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
static int    s_argv_cap;

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

/* 12K words = 24576 bytes, matching cfg.c's
 * file_read_rom16(f, 12*1024, cfg->ecs_img). */
#define ECS_ROM_SIZE (12 * 1024 * 2)

int intv_host_has_ecs_rom(const char *rom_dir)
{
    char path[4096];
    FILE *f;
    long size;

    if (!rom_dir)
        return 0;

    snprintf(path, sizeof(path), "%s/ecs.bin", rom_dir);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    fclose(f);
    return size == ECS_ROM_SIZE;
}

static void (*s_thread_hook)(void) = NULL;

void intv_host_set_thread_hook(void (*hook)(void))
{
    s_thread_hook = hook;
}

static void *thread_main(void *arg)
{
    (void)arg;
    if (s_thread_hook)
        s_thread_hook();
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
    s_argv_cap = 0;
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

/* Appends one string onto s_argv, growing it (doubling) as needed and
 * keeping it NULL-terminated (jzintv_entry_point's argv, like any argv,
 * doesn't need argc's slot NULL'd, but this makes free_argv's bound and any
 * accidental strlen-till-NULL walk equally safe). Returns 0 on success, -1
 * on allocation failure (s_argv is left in a valid, freeable state either
 * way -- the caller's job is to call free_argv() and bail). */
static int push_arg(const char *s)
{
    if (s_argc + 1 >= s_argv_cap)
    {
        const int new_cap = s_argv_cap ? s_argv_cap * 2 : 8;
        char **grown = realloc(s_argv, (size_t)new_cap * sizeof(*s_argv));
        if (!grown)
        {
            fprintf(stderr, "intv_host: out of memory\n");
            return -1;
        }
        s_argv = grown;
        s_argv_cap = new_cap;
    }
    s_argv[s_argc++] = xstrdup(s);
    s_argv[s_argc] = NULL;
    return 0;
}

int intv_host_start(const intv_host_opts *opts)
{
    char exec_path[4096], grom_path[4096], fujinet_arg[256], e_arg[4160],
         g_arg[4160], bootdir_arg[4160];
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

    /* ECS precheck: do this before touching s_argv at all, so a rejected
     * request leaves the previous argv (if any) alone. A user-forced
     * --ecs=1 with no readable/right-sized ecs.bin would otherwise reach
     * cfg_init() and exit(1) the whole process (cfg.c's ECS ROM load) --
     * refuse here instead, same shape as the exec/grom check above. */
    char ecs_path[4096] = {0};
    if (opts->ecs > 0)
    {
        snprintf(ecs_path, sizeof(ecs_path), "%s/ecs.bin", opts->rom_dir);
        if (!intv_host_has_ecs_rom(opts->rom_dir))
        {
            fprintf(stderr, "intv_host: ECS enabled but %s is missing or "
                            "not %d bytes\n", ecs_path, ECS_ROM_SIZE);
            return -1;
        }
    }

    snprintf(fujinet_arg, sizeof(fujinet_arg), "--fujinet=%s:%d",
             (opts->fujinet_host && opts->fujinet_host[0])
                 ? opts->fujinet_host : "127.0.0.1",
             opts->fujinet_port > 0 ? opts->fujinet_port : 1985);
    snprintf(e_arg, sizeof(e_arg), "-e%s", exec_path);
    snprintf(g_arg, sizeof(g_arg), "-g%s", grom_path);

    free_argv();
    if (push_arg("fujinet-go-intv") != 0 ||
        push_arg(e_arg) != 0 ||
        push_arg(g_arg) != 0 ||
        push_arg(fujinet_arg) != 0)
    {
        free_argv();
        return -1;
    }

    /* Where jzIntv stages a FujiNet-pushed .bin + .cfg pair so its own
     * BIN+CFG loader can read it, and where pushed JLP titles keep their
     * saves (state_dir/jlpsave). jzIntv falls back to $TMPDIR then /tmp on
     * its own, which is fine for a standalone run but not for us: the
     * Android build has neither, and a JLP save belongs beside the rest of
     * the session's state anyway, not in a directory the OS may clear. */
    if (opts->state_dir && opts->state_dir[0])
    {
        snprintf(bootdir_arg, sizeof(bootdir_arg), "--fujinet-bootdir=%s",
                 opts->state_dir);
        if (push_arg(bootdir_arg) != 0) { free_argv(); return -1; }
    }

    /* ECS/Intellivoice are tri-state (INTV_HW_AUTO/OFF/ON): only emit the
     * flag when the caller actually wants to override jzIntv's own
     * cart-metadata-driven default, matching --ecs/--voice's own "0/1"
     * argument convention (cfg.c's optional-argument parsing, so the value
     * must be attached with no space -- "--ecs=1", never "--ecs 1"). */
    if (opts->ecs != INTV_HW_AUTO)
    {
        char flag[16];
        snprintf(flag, sizeof(flag), "--ecs=%d", opts->ecs > 0 ? 1 : 0);
        if (push_arg(flag) != 0) { free_argv(); return -1; }

        if (opts->ecs > 0)
        {
            char e_ecs_arg[4160];
            /* jzIntv's own default "ecs.bin" resolves against its rom_path
             * search list, not our session's ROM dir -- always pass the
             * absolute path explicitly. */
            snprintf(e_ecs_arg, sizeof(e_ecs_arg), "-E%s", ecs_path);
            if (push_arg(e_ecs_arg) != 0) { free_argv(); return -1; }
        }
    }

    if (opts->ivoice != INTV_HW_AUTO)
    {
        char flag[16];
        snprintf(flag, sizeof(flag), "--voice=%d", opts->ivoice > 0 ? 1 : 0);
        if (push_arg(flag) != 0) { free_argv(); return -1; }
    }

    if (opts->pal && push_arg("--pal") != 0) { free_argv(); return -1; }

    /* Shrink snd_desktop's producer burst from its 2048-sample default
     * (~43ms at 48kHz) down to 512 (~11ms): intv_audio's ring
     * (core/jzintv/intv_audio.c) is drained by an SDL callback whose own
     * chunk size is whatever the host audio device quantum happens to be,
     * which on Linux/PipeWire can be well under 2048 -- a smaller, more
     * frequent producer burst keeps the ring topped up in smaller
     * increments instead of arriving in one lump that has to last until
     * the next 43ms publish, cutting both latency and jitter. */
    if (push_arg("-B512") != 0) { free_argv(); return -1; }

    /* cfg.c defaults rate_ctl to "on" UNLESS plat_is_batch_mode() says
     * otherwise (src/cfg/cfg.c: "cfg->rate_ctl = !batch_mode"), and our
     * plat backend is src/plat/plat_null.c, whose plat_is_batch_mode()
     * unconditionally returns true (it has no window to be interactive
     * about) -- so left alone, rate control defaults OFF and the CPU runs
     * as fast as the host machine allows, hundreds of times real NTSC
     * speed. -r1 forces jzIntv's own real-time throttle (speed.c) on
     * regardless of that batch-mode default; this is not a workaround for
     * something specific to us, it's the same throttle a real windowed
     * jzIntv build gets for free from plat_sdl.c's own (non-batch)
     * plat_is_batch_mode(). */
    if (push_arg("-r1") != 0) { free_argv(); return -1; }

    /* Trailing positional argument: cfg.c reads argv_copy[optind] (the
     * first non-flag argument, which by construction is the last thing we
     * push) into cfg->fn_game when present, and only falls back to booting
     * the embedded FujiNet config ROM (fujinet_use_config_rom = 1) when it
     * is absent -- so this MUST be pushed last, after every flag above. */
    if (opts->cart_path && opts->cart_path[0])
    {
        f = fopen(opts->cart_path, "rb");
        if (!f)
        {
            fprintf(stderr, "intv_host: cartridge not readable: %s\n",
                    opts->cart_path);
            free_argv();
            return -1;
        }
        fclose(f);
        if (push_arg(opts->cart_path) != 0) { free_argv(); return -1; }
    }

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

void intv_host_reset(void)
{
    pthread_mutex_lock(&s_lock);
    const int running = s_running;
    pthread_mutex_unlock(&s_lock);

    if (!running)
        return;

    /* Release every held input first: this port injects straight into
     * intv.pad0/pad1 (see intv_host_pad_key/_pad_disc/_ecs_key below) and
     * never goes through jzIntv's own event layer, so jzIntv's own
     * pad_reset_inputs() call in its main loop (gated on chg_evt_map, which
     * this port never sets) never runs -- a key held at the moment of reset
     * would otherwise stay latched down in the machine across it.
     * pad_reset_inputs also clears pad_t.k[] (ECS keyboard) and fake_shift/
     * stale, so this subsumes intv_host_ecs_keys_clear(). Both synchronous:
     * done here, on the caller's thread, before do_reset is even armed. */
    pad_reset_inputs(&intv.pad0);
    pad_reset_inputs(&intv.pad1);

    /* So up to ~100ms of pre-reset audio doesn't play out across the
     * reset -- see intv_audio.h's own comment on the ring this drains. */
    intv_audio_reset();

    /* Arms jzIntv's one-shot RESET for its main loop to consume on its next
     * iteration (see jzintv.c: do_reset == 2 clears itself, forces the
     * CP1610's PC to 0x1000, and periph_reset() runs exactly once, one
     * iteration later) -- same cross-thread write pattern as do_exit above.
     * Not declared volatile/atomic upstream (neither is do_exit), but the
     * loop body between reading it and clearing it does enough (periph_tick/
     * gfx_refresh/plat_delay) that the compiler cannot hoist the reload;
     * __atomic_store_n costs nothing here and documents the intent. */
    __atomic_store_n(&intv.do_reset, 2u, __ATOMIC_RELAXED);
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

/* side's top bit picks pad0 (Master Component) vs pad1 (ECS second pair);
 * the low bit picks l[]/r[] within that pad, exactly matching
 * INTV_PAD_LEFT=0/RIGHT=1/ECS_LEFT=2/ECS_RIGHT=3's own encoding. pad1 is
 * only bus-registered when ECS is enabled, but writing its l[]/r[] array
 * unconditionally is harmless -- same "safe whether or not running" shape
 * as the base pair. */
void intv_host_pad_key(intv_pad_side side, intv_pad_key key, int pressed)
{
    if (key < 0 || key >= INTV_PAD_KEY_COUNT)
        return;

    const uint32_t value = pressed ? key_codes[key] : 0;
    const int idx = key_index[key];
    pad_t *const pad = (side & 2) ? &intv.pad1 : &intv.pad0;

    if ((side & 1) == 0)
        pad->l[idx] = value;
    else
        pad->r[idx] = value;
}

void intv_host_pad_disc(intv_pad_side side, int direction)
{
    uint32_t value = 0;
    if (direction >= 0 && direction < 16)
        value = disc_codes[direction];

    pad_t *const pad = (side & 2) ? &intv.pad1 : &intv.pad0;
    if ((side & 1) == 0)
        pad->l[15] = value;
    else
        pad->r[15] = value;
}

/* ---- ECS keyboard injection -------------------------------------------- */

/* Indexed by intv_ecs_key; {row, mask} into intv.pad1.k[row]. Transposed
 * entry-by-entry from mapping.c's "KEYB_*" table (cfg_key_bind[] column 3,
 * pad1.k[0..6]) -- see intv_host.h's own comment on this table. */
static const struct { uint8_t row; uint32_t mask; }
ecs_key_codes[INTV_ECS_KEY_COUNT] = {
    [INTV_ECS_KEY_LEFT]    = { 0, 1   },
    [INTV_ECS_KEY_PERIOD]  = { 0, 2   },
    [INTV_ECS_KEY_SEMI]    = { 0, 4   },
    [INTV_ECS_KEY_P]       = { 0, 8   },
    [INTV_ECS_KEY_ESC]     = { 0, 16  },
    [INTV_ECS_KEY_0]       = { 0, 32  },
    [INTV_ECS_KEY_ENTER]   = { 0, 64  },

    [INTV_ECS_KEY_COMMA]   = { 1, 1   },
    [INTV_ECS_KEY_M]       = { 1, 2   },
    [INTV_ECS_KEY_K]       = { 1, 4   },
    [INTV_ECS_KEY_I]       = { 1, 8   },
    [INTV_ECS_KEY_9]       = { 1, 16  },
    [INTV_ECS_KEY_8]       = { 1, 32  },
    [INTV_ECS_KEY_O]       = { 1, 64  },
    [INTV_ECS_KEY_L]       = { 1, 128 },

    [INTV_ECS_KEY_N]       = { 2, 1   },
    [INTV_ECS_KEY_B]       = { 2, 2   },
    [INTV_ECS_KEY_H]       = { 2, 4   },
    [INTV_ECS_KEY_Y]       = { 2, 8   },
    [INTV_ECS_KEY_7]       = { 2, 16  },
    [INTV_ECS_KEY_6]       = { 2, 32  },
    [INTV_ECS_KEY_U]       = { 2, 64  },
    [INTV_ECS_KEY_J]       = { 2, 128 },

    [INTV_ECS_KEY_V]       = { 3, 1   },
    [INTV_ECS_KEY_C]       = { 3, 2   },
    [INTV_ECS_KEY_F]       = { 3, 4   },
    [INTV_ECS_KEY_R]       = { 3, 8   },
    [INTV_ECS_KEY_5]       = { 3, 16  },
    [INTV_ECS_KEY_4]       = { 3, 32  },
    [INTV_ECS_KEY_T]       = { 3, 64  },
    [INTV_ECS_KEY_G]       = { 3, 128 },

    [INTV_ECS_KEY_X]       = { 4, 1   },
    [INTV_ECS_KEY_Z]       = { 4, 2   },
    [INTV_ECS_KEY_S]       = { 4, 4   },
    [INTV_ECS_KEY_W]       = { 4, 8   },
    [INTV_ECS_KEY_3]       = { 4, 16  },
    [INTV_ECS_KEY_2]       = { 4, 32  },
    [INTV_ECS_KEY_E]       = { 4, 64  },
    [INTV_ECS_KEY_D]       = { 4, 128 },

    [INTV_ECS_KEY_SPACE]   = { 5, 1   },
    [INTV_ECS_KEY_DOWN]    = { 5, 2   },
    [INTV_ECS_KEY_UP]      = { 5, 4   },
    [INTV_ECS_KEY_Q]       = { 5, 8   },
    [INTV_ECS_KEY_1]       = { 5, 16  },
    [INTV_ECS_KEY_RIGHT]   = { 5, 32  },
    [INTV_ECS_KEY_CTRL]    = { 5, 64  },
    [INTV_ECS_KEY_A]       = { 5, 128 },

    [INTV_ECS_KEY_SHIFT]   = { 6, 128 },
};

void intv_host_ecs_key(intv_ecs_key key, int pressed)
{
    if (key < 0 || key >= INTV_ECS_KEY_COUNT)
        return;

    const uint8_t row = ecs_key_codes[key].row;
    const uint32_t mask = ecs_key_codes[key].mask;

    if (pressed)
        intv.pad1.k[row] |= mask;
    else
        intv.pad1.k[row] &= ~mask;
}

void intv_host_ecs_keys_clear(void)
{
    for (int row = 0; row < 7; row++)
        intv.pad1.k[row] = 0;
}

void intv_host_debug_test_tone(void)
{
    /* Register 8 (mixer) is active-LOW, per ay8910_calc_sound's own
     * bit_a = (snd_a | chn_a) & (noi_a | chn_n): a set disable-bit forces
     * that term to 1 unconditionally, bypassing (not gating) the actual
     * tone/noise generator state. 0x3E = disable tone B/C and all three
     * noise channels (bits 1-5 set), leave tone A's disable bit (bit 0)
     * clear -- i.e. tone A is the only channel actually enabled. */
    periph_t *const psg = AS_PERIPH(&intv.psg0);
    psg->write(psg, NULL,  0, 0x10); /* R0:  tone A period, low byte */
    psg->write(psg, NULL, 11, 0x0F); /* R11: channel A fixed amplitude, max */
    psg->write(psg, NULL,  8, 0x3E); /* R8:  mixer -- tone A enabled only */
}
