/*
 * test_tmp_template: builds a portable mkdtemp/mkstemp template.
 *
 * A hardcoded "/tmp/..." template breaks under MinGW-w64/UCRT64 (Windows):
 * a program linked against the native UCRT runtime, unlike a full MSYS-
 * space build, gets no POSIX path translation, so "/tmp" resolves
 * literally to a non-existent root-relative folder rather than a real
 * temp directory -- mkdtemp/mkstemp then fail with ENOENT. Caught by the
 * Windows CI job (windows.yml): configure and build succeeded, but
 * session/fujibus_smoke/symtab all failed with exactly that error, the
 * ROM-gated tests (boot_smoke, pad_test, audio, debug, stic) would have
 * hit the same bug the moment WITH_INTV_ROMS=ON let them actually run.
 *
 * TMPDIR/TEMP/TMP are what every platform's own temp-directory convention
 * actually sets (TMPDIR on POSIX, TEMP/TMP on Windows -- MSYS2 itself sets
 * TMP for native Windows tools it launches); "/tmp" is kept only as the
 * last-resort fallback for a minimal environment that sets none of them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTV_TEST_TMPDIR_H
#define INTV_TEST_TMPDIR_H

#include <stdio.h>
#include <stdlib.h>

static inline const char *test_tmp_base(void)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = getenv("TEMP");
    if (!t || !*t) t = getenv("TMP");
    if (!t || !*t) t = "/tmp";
    return t;
}

/* Writes "<base>/<prefix>XXXXXX" into buf (sized bufsz), ready for
 * mkdtemp/mkstemp. prefix should already end in a separator-free name --
 * this does not insert one of its own beyond the '/' after <base>. */
static inline void test_tmp_template(char *buf, size_t bufsz,
                                     const char *prefix)
{
    snprintf(buf, bufsz, "%s/%sXXXXXX", test_tmp_base(), prefix);
}

#endif /* INTV_TEST_TMPDIR_H */
