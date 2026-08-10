/*
 * intvsession -- see intvsession.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "intv_frame.h"
#include "intv_host.h"
#include "session_internal.h"

intvsession *intvsession_new(const intvsession_paths *paths)
{
    intvsession *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        free(s);
        return NULL;
    }
    settings_init(s);
    s->fujinet_port = INTVSESSION_BOIP_PORT;
    return s;
}

void intvsession_free(intvsession *s)
{
    if (!s)
        return;
    intvsession_stop(s);
    settings_free_all(s);
    free(s);
}

int intvsession_start(intvsession *s)
{
    if (!intvsession_has_system_roms(s)) {
        session_set_error(s, "%s is missing exec.bin/grom.bin -- import "
                          "system ROMs, or build -DWITH_INTV_ROMS=ON for "
                          "local testing", s->roms_dir);
        return -1;
    }

    intv_host_opts opts = {
        .rom_dir = s->roms_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = s->fujinet_port,
    };
    if (intv_host_start(&opts) != 0) {
        session_set_error(s, "failed to start the emulator thread");
        return -1;
    }
    return 0;
}

void intvsession_stop(intvsession *s)
{
    (void)s;
    intv_host_stop();
}

int intvsession_is_running(const intvsession *s)
{
    (void)s;
    return intv_host_is_running();
}

const char *intvsession_last_error(const intvsession *s)
{
    return s->last_error;
}

int intvsession_copy_frame(intvsession *s, uint32_t *dst,
                           uint64_t *serial_inout)
{
    (void)s;
    return intv_frame_copy(dst, serial_inout);
}

void intvsession_pad_key(intvsession *s, intvsession_pad_side side,
                         intvsession_key key, int pressed)
{
    (void)s;
    intv_host_pad_key((intv_pad_side)side, (intv_pad_key)key, pressed);
}

void intvsession_pad_disc(intvsession *s, intvsession_pad_side side,
                          int direction)
{
    (void)s;
    intv_host_pad_disc((intv_pad_side)side, direction);
}
