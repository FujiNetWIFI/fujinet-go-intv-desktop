/*
 * intvsession FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-rs232 plus
 * the in-process entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp).
 * Port of fujinet-go-coco-desktop's own fujinet_runtime.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);
typedef void (*clear_audio_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the CoCo/Android wrappers. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static intv_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static read_audio_fn g_read_audio;
static clear_audio_fn g_clear_audio;
static copy_log_fn g_copy_log;

static int load_library_locked(struct intvsession *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = intv_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          intv_dynlib_error(errbuf, sizeof(errbuf)));
        return -1;
    }
    g_start = (start_runtime_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_last_error_message");
    g_read_audio = (read_audio_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_read_audio");
    g_clear_audio = (clear_audio_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_clear_audio");
    g_copy_log = (copy_log_fn)intv_dynlib_sym(
        g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(struct intvsession *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "intvsession: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    /* fnFsSPIFFS's flash base and mgHttpClient's CA bundle (see
     * tools/fujinet/build-fujinet-desktop.sh) root themselves from this
     * rather than from the CWD, so FujiNet keeps working if anything ever
     * moves the process's working directory out from under it. Both fall
     * back to a relative path when it is unset, which is why leaving it
     * unset fails quietly rather than loudly: the CA store simply loads
     * empty and every https:// fetch starts failing like a network fault. */
    intv_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, INTVSESSION_BOIP_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

/* Wait until something is actually accepting on the BoIP port.
 *
 * fujinet_start() returning is NOT enough -- main_setup() comes back as
 * soon as the devices are constructed, but the RS232 bus's BoIP channel
 * then logs "No WiFi!" and suspends itself briefly before it begins
 * listening (see lib/hardware/BoIPChannel.cpp in the firmware tree; the
 * RS232 bus's BoIPConfig defaults to listening=true, exactly the direction
 * jzIntv's own fn_sock.c -- a BoIP client -- needs). Start the emulator in
 * that window and its first --fujinet connection attempt finds nothing.
 * fn_sock.c does retry non-blockingly (see intv_host.h), so missing this
 * window is not fatal the way it is on the CoCo port's DriveWire boot --
 * but the FujiNet mailbox stays disconnected until the next retry, so
 * probing here still avoids a needless multi-second wait for the first
 * exchange. Returns 0 once the listener is up, -1 on timeout. */
int fujinet_wait_for_boip(struct intvsession *s, int timeout_ms)
{
    int waited = 0;

    if (!s->fujinet_running)
        return -1;

    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr;
            int ok;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(INTVSESSION_BOIP_PORT);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            intv_closesocket(fd);
            if (ok)
                return 0;
        }
        if (waited >= timeout_ms) {
            fprintf(stderr, "intvsession: the FujiNet BoIP listener did not "
                            "come up within %d ms; --fujinet will keep "
                            "retrying on its own\n", timeout_ms);
            return -1;
        }
        intv_sleep_ms(25);
        waited += 25;
    }
}

void fujinet_stop(struct intvsession *s)
{
    stop_runtime_fn stop = NULL;
    clear_audio_fn clear = NULL;

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        clear = g_clear_audio;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stop();
    if (clear) clear();
}

int fujinet_copy_log(struct intvsession *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

int intvsession_fujinet_running(const intvsession *s)
{
    return s->fujinet_running;
}

const char *intvsession_fujinet_webui_url(const intvsession *s)
{
    return s->webui_url;
}

int intvsession_fujinet_copy_log(intvsession *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}

/* Overlay FujiNet (SAM speech) audio onto the emulator mix, saturating.
 * Both buf and the runtime's own output are mono -- unlike the CoCo port,
 * which widens a mono runtime stream into a stereo XRoar mix, jzIntv's own
 * PSG mixer is mono too (see core/jzintv/intv_audio.h), so no channel
 * widening is needed here. */
void fujinet_mix_audio(struct intvsession *s, int16_t *buf, int nsamples,
                       int rate)
{
    static int16_t overlay[2048];
    read_audio_fn fn;
    int samples_left;

    if (!s->fujinet_running || nsamples <= 0) return;
    pthread_mutex_lock(&g_mtx);
    fn = g_read_audio;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return;

    samples_left = nsamples;

    while (samples_left > 0) {
        int chunk = samples_left > (int)(sizeof(overlay) / sizeof(overlay[0]))
                        ? (int)(sizeof(overlay) / sizeof(overlay[0]))
                        : samples_left;
        int produced = fn(overlay, chunk, rate);
        int i;
        if (produced <= 0) return;
        for (i = 0; i < produced; i++) {
            int mixed = (int)buf[i] + (int)overlay[i];
            if (mixed > INT16_MAX) mixed = INT16_MAX;
            if (mixed < INT16_MIN) mixed = INT16_MIN;
            buf[i] = (int16_t)mixed;
        }
        if (produced < chunk) return;
        buf += produced;
        samples_left -= produced;
    }
}
