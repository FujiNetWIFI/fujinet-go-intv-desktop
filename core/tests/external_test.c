/*
 * external_test -- "Use external FujiNet": with the switch on, the session
 * starts no runtime of its own and jzIntv dials the given address. A
 * stand-in listener on a spare port plays the FujiNet and must see the
 * connection. The EXEC and GROM files it needs are blank images of the
 * right size: the mailbox dials whatever the CPU is doing. SKIPs (77) on
 * Windows, which has no BSD sockets here, and on macOS, where the headless
 * CI runner leaves jzIntv halted after its first instruction and it never
 * dials (cause not found; the dial path is the built-in runtime's).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__APPLE__)
int main(void)
{
    printf("external_test: SKIP: not run on this platform\n");
    return 77;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "intvsession.h"
#include "test_tmpdir.h"

static int failures;
static void check(const char *what, int passed)
{
    if (!passed) {
        fprintf(stderr, "external_test: FAILED: %s\n", what);
        failures = 1;
    }
}

static void blank_rom(const char *dir, const char *name, size_t size)
{
    char path[1100];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "wb");
    if (file) {
        char *zeros = calloc(1, size);
        fwrite(zeros, 1, size, file);
        free(zeros);
        fclose(file);
    }
}

int main(void)
{
    char config_dir[1024], data_dir[1024], addr[64];
    intvsession_paths paths;
    intvsession *session;
    intvsession_start_opts opts;
    struct sockaddr_in sin;
    socklen_t sinlen = sizeof(sin);
    int listener, conn = -1;

    test_tmp_template(config_dir, sizeof(config_dir), "intv-external-cfg-");
    test_tmp_template(data_dir, sizeof(data_dir), "intv-external-data-");
    if (!mkdtemp(config_dir) || !mkdtemp(data_dir)) {
        perror("mkdtemp");
        return 1;
    }
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = config_dir;
    paths.data_dir = data_dir;
    session = intvsession_new(&paths);
    if (!session)
        return 1;
    blank_rom(intvsession_roms_path(session), "exec.bin", 8192);
    blank_rom(intvsession_roms_path(session), "grom.bin", 2048);
    if (!intvsession_has_system_roms(session)) {
        printf("external_test: SKIP: cannot stage the system ROM files\n");
        intvsession_free(session);
        return 77;
    }

    listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&sin, sizeof(sin)) ||
        listen(listener, 1) ||
        getsockname(listener, (struct sockaddr *)&sin, &sinlen))
        return 1;
    snprintf(addr, sizeof(addr), "localhost:%d", ntohs(sin.sin_port));

    intvsession_default_opts(session, &opts);
    opts.external_fujinet = 1;
    opts.fujinet_addr = "not an address";
    check("a bad address refuses to start",
          intvsession_start(session, &opts) != 0);
    check("...and nothing runs", !intvsession_is_running(session));

    opts.fujinet_addr = addr;
    check("starts with the external address",
          intvsession_start(session, &opts) == 0);
    check("the session reports external",
          intvsession_fujinet_external(session) == 1);
    check("no built-in runtime was started",
          intvsession_fujinet_running(session) == 0);
    check("the endpoint is the address",
          strcmp(intvsession_boip_endpoint(session), addr) == 0);
    check("no web UI url", intvsession_fujinet_webui_url(session)[0] == '\0');

    {
        struct pollfd pfd = { listener, POLLIN, 0 };
        if (poll(&pfd, 1, 15000) > 0)
            conn = accept(listener, NULL, NULL);
    }
    check("jzIntv dialled the stand-in FujiNet", conn >= 0);
    if (conn >= 0)
        close(conn);

    intvsession_stop(session);
    check("stopping clears external",
          intvsession_fujinet_external(session) == 0);
    intvsession_free(session);
    close(listener);
    return failures;
}
#endif
