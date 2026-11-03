/*
 * exe_dir_lib_test -- the FujiNet runtime bundled beside the executable is
 * the one paths_provision_fujinet finds. macOS only: a .app keeps
 * libfujinet.dylib and fujinet/ in Contents/MacOS, and exe_dir() once had no
 * macOS branch, so the bundled runtime was never found (CONFIG then sat on
 * SELECT NETWORK). CI's runner has the dev build output on disk, which hid
 * it, so this plants a dummy copy beside the test and checks that it wins.
 * Elsewhere it skips.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"
#include "test_tmpdir.h"

#ifndef __APPLE__

int main(void)
{
    printf("exe_dir_lib_test: skipped (macOS bundle layout only)\n");
    return 77;
}

#else

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TEST_EXE_DIR
#error "TEST_EXE_DIR must name the directory holding this test binary"
#endif

static int touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("x", f);
    fclose(f);
    return 0;
}

int main(void)
{
    static struct intvsession s;
    char lib[INTV_PATH_MAX], rt[INTV_PATH_MAX], ini[INTV_PATH_MAX];
    char cfg[INTV_PATH_MAX], data[INTV_PATH_MAX];
    char want[PATH_MAX], got[PATH_MAX];
    int rc, failed = 0;

    snprintf(lib, sizeof lib, "%s/libfujinet.dylib", TEST_EXE_DIR);
    snprintf(rt, sizeof rt, "%s/fujinet", TEST_EXE_DIR);
    snprintf(ini, sizeof ini, "%s/fnconfig.ini", rt);
    mkdir(rt, 0755);
    if (touch(lib) || touch(ini)) {
        fprintf(stderr, "FAIL: cannot plant files in %s\n", TEST_EXE_DIR);
        return 1;
    }

    test_tmp_template(cfg, sizeof cfg, "intv-exedir-cfg-");
    test_tmp_template(data, sizeof data, "intv-exedir-data-");
    if (!mkdtemp(cfg) || !mkdtemp(data)) {
        perror("mkdtemp");
        return 1;
    }

    unsetenv("FUJINET_LIB");
    if (paths_init(&s, cfg, data) != 0) {
        fprintf(stderr, "FAIL: paths_init\n");
        failed = 1;
    } else if ((rc = paths_provision_fujinet(&s)) != 0) {
        fprintf(stderr, "FAIL: paths_provision_fujinet returned %d\n", rc);
        failed = 1;
    } else if (!realpath(lib, want) || !realpath(s.fujinet_lib, got) ||
               strcmp(want, got) != 0) {
        fprintf(stderr, "FAIL: found \"%s\", wanted the bundled \"%s\"\n",
                s.fujinet_lib, lib);
        failed = 1;
    }

    remove(ini);
    rmdir(rt);
    remove(lib);
    if (!failed) printf("exe_dir_lib_test: ok\n");
    return failed;
}

#endif
