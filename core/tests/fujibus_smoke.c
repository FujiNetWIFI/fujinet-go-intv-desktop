/*
 * fujibus_smoke -- brings up the real FujiNet runtime (libfujinet.so, built
 * by cmake/FujiNetRuntime.cmake -DWITH_FUJINET=ON) alongside the emulator
 * and confirms actual FujiBus traffic crossed the wire: the embedded
 * config ROM queries FujiNet (GET WIFI STATUS / READ HOST SLOTS) within a
 * couple of seconds of boot, without any cartridge or user input. The
 * debugger's FujiNet trace must have seen it too, both ways.
 *
 * Needs libfujinet.so available (FUJINET_LIB env var, or beside the test
 * binary, or in tools/fujinet/work/out -- see paths_provision_fujinet) and
 * embedded ROMs (-DWITH_INTV_ROMS=ON); skipped (exit 77) otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intvdebug.h"
#include "intvsession.h"
#include "test_tmpdir.h"

int main(void)
{
    char config_dir[1024], data_dir[1024];
    test_tmp_template(config_dir, sizeof(config_dir), "intv-fujibus-cfg-");
    test_tmp_template(data_dir, sizeof(data_dir), "intv-fujibus-data-");
    if (!mkdtemp(config_dir) || !mkdtemp(data_dir)) {
        perror("mkdtemp");
        return 1;
    }

    intvsession_paths paths = { .config_dir = config_dir, .data_dir = data_dir };
    intvsession *s = intvsession_new(&paths);
    if (!s) {
        fprintf(stderr, "fujibus_smoke: intvsession_new failed\n");
        return 1;
    }

    if (!intvsession_has_system_roms(s)) {
        fprintf(stderr, "fujibus_smoke: no system ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        intvsession_free(s);
        return 77;
    }

    if (intvsession_start(s, NULL) != 0) {
        fprintf(stderr, "fujibus_smoke: intvsession_start failed: %s\n",
                intvsession_last_error(s));
        intvsession_free(s);
        return 1;
    }

    if (!intvsession_fujinet_running(s)) {
        fprintf(stderr, "fujibus_smoke: FujiNet runtime did not start "
                        "(FUJINET_LIB not set and no runtime found) -- "
                        "SKIP\n");
        intvsession_stop(s);
        intvsession_free(s);
        return 77;
    }

    /* The config ROM queries FujiNet's Fuji device (GET_WIFI_STATUS /
     * GET_ADAPTERCONFIG-ish READ_HOST_SLOTS) within its first frames after
     * boot; poll the runtime's own log for evidence of that round trip
     * rather than guessing a fixed sleep. */
    char log[16384];
    int saw_traffic = 0;
    for (int i = 0; i < 300 && !saw_traffic; i++) {
        intvsession_fujinet_copy_log(s, log, sizeof(log));
        if (strstr(log, "BoIPChannel connected") &&
            (strstr(log, "Fuji cmd") || strstr(log, "rs232_process_cmd")))
            saw_traffic = 1;
        usleep(10000);
    }

    /* The trace is always on, so it saw that round trip: at least one
     * request and one reply. */
    static intvdebug_fuji_packet trace[INTVDEBUG_FUJI_TRACE_MAX];
    int sent = 0, received = 0, n = 0;
    for (int i = 0; i < 200 && saw_traffic && !(sent && received); i++) {
        n = intvdebug_fujinet_trace(intvdebug_get(), trace,
                                    INTVDEBUG_FUJI_TRACE_MAX);
        for (int k = 0; k < n; k++) {
            if (trace[k].to_host)
                received = 1;
            else
                sent = 1;
        }
        if (!(sent && received))
            usleep(10000);
    }

    intvsession_stop(s);
    intvsession_free(s);

    if (!saw_traffic) {
        fprintf(stderr, "fujibus_smoke: no FujiBus traffic observed within "
                        "3s\n---- last FujiNet log ----\n%s\n", log);
        return 1;
    }
    if (!sent || !received) {
        fprintf(stderr, "fujibus_smoke: the FujiNet trace has %d packets, "
                        "requests %s, replies %s\n", n, sent ? "yes" : "no",
                received ? "yes" : "no");
        return 1;
    }

    printf("fujibus_smoke: OK (real BoIP connection + FujiBus traffic, "
           "%d traced packets)\n", n);
    return 0;
}
