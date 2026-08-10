/*
 * intvsession path layout: XDG config/data directories and ROM
 * materialisation (delegated to intv_host_provision_roms -- see
 * core/jzintv/intv_host.h, which owns the embedded-ROM table already).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "intv_host.h"
#include "session_internal.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Compiled-in development fallbacks (set by CMake to paths in the source /
 * build trees) so a git checkout runs without an install step. */
#ifndef INTV_DEV_FUJINET_OUT
#define INTV_DEV_FUJINET_OUT ""
#endif
#ifndef INTV_INSTALL_DATADIR
#define INTV_INSTALL_DATADIR ""
#endif
#ifndef INTV_INSTALL_LIBDIR
#define INTV_INSTALL_LIBDIR ""
#endif

static int is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int make_dir(const char *path)
{
#if defined(_WIN32)
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int mkdir_p(const char *path)
{
    char buf[INTV_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (is_sep(*p)) {
            char save = *p;
            *p = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) return -1;
            *p = save;
        }
    }
    if (make_dir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int is_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char from[INTV_PATH_MAX], to[INTV_PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            rc |= copy_tree(from, to);
        else if (S_ISREG(st.st_mode))
            rc |= copy_file(from, to);
    }
    closedir(d);
    return rc;
}

/* Directory holding the running executable, or "" when it cannot be
 * determined -- the first place a Windows install (a folder you copy
 * around) is checked for the runtime, since the exe and fujinet.dll sit
 * side by side there. */
static const char *exe_dir(void)
{
    static char dir[INTV_PATH_MAX];
#if defined(_WIN32)
    static int resolved;
    if (!resolved) {
        resolved = 1;
        if (GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir)) == 0) {
            dir[0] = '\0';
        } else {
            dir[sizeof(dir) - 1] = '\0';
            char *p = strrchr(dir, '\\');
            if (!p) p = strrchr(dir, '/');
            if (p) *p = '\0'; else dir[0] = '\0';
        }
    }
#endif
    return dir;
}

/* Per-user config/data directory. On Windows this is %APPDATA% (config) or
 * %LOCALAPPDATA% (data); elsewhere the XDG variable, then $HOME/suffix. */
static void default_dir(char *dst, size_t dstsz, const char *xdg_env,
                        const char *win_env, const char *home_suffix)
{
#if defined(_WIN32)
    const char *v = getenv(win_env);
    (void)xdg_env;
    (void)home_suffix;
    snprintf(dst, dstsz, "%s\\fujinet-go-intv", (v && *v) ? v : ".");
#else
    const char *v = getenv(xdg_env);
    (void)win_env;
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-intv", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-intv", home ? home : ".",
                 home_suffix);
    }
#endif
}

static int is_absolute(const char *p)
{
    if (!p || !*p) return 0;
#if defined(_WIN32)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && is_sep(p[2]))
        return 1;
    return is_sep(p[0]) && is_sep(p[1]);
#else
    return p[0] == '/';
#endif
}

static void make_absolute(char *path, size_t size)
{
    char cwd[INTV_PATH_MAX];
    char joined[INTV_PATH_MAX];

    if (is_absolute(path))
        return;
    if (!getcwd(cwd, sizeof(cwd)))
        return;
    snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    snprintf(path, size, "%s", joined);
}

int paths_init(struct intvsession *s, const char *config_dir,
              const char *data_dir)
{
    if (config_dir && *config_dir)
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", config_dir);
    else
        default_dir(s->config_dir, sizeof(s->config_dir), "XDG_CONFIG_HOME",
                    "APPDATA", ".config");

    if (data_dir && *data_dir)
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", data_dir);
    else
        default_dir(s->data_dir, sizeof(s->data_dir), "XDG_DATA_HOME",
                    "LOCALAPPDATA", ".local/share");

    make_absolute(s->config_dir, sizeof(s->config_dir));
    make_absolute(s->data_dir, sizeof(s->data_dir));

    if (mkdir_p(s->config_dir) != 0 || mkdir_p(s->data_dir) != 0)
        return -1;

    snprintf(s->settings_file, sizeof(s->settings_file), "%s/settings.ini",
             s->config_dir);

    {
        const char *env = getenv("INTV_ROM_DIR");
        if (env && *env)
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s", env);
        else
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s/roms", s->data_dir);
    }
    make_absolute(s->roms_dir, sizeof(s->roms_dir));
    if (mkdir_p(s->roms_dir) != 0)
        return -1;

    intv_host_provision_roms(s->roms_dir);

    /* The FujiNet runtime tree. Pure derivations of data_dir, settled here
     * (rather than only when the runtime is actually started) so a session
     * with FujiNet unavailable still reports sensible paths instead of
     * empty strings -- see cocosession's own paths_init for the same
     * reasoning. */
    snprintf(s->fujinet_root, sizeof(s->fujinet_root), "%s/fujinet",
             s->data_dir);
    snprintf(s->fujinet_config, sizeof(s->fujinet_config), "%s/fnconfig.ini",
             s->fujinet_root);
    snprintf(s->fujinet_sd, sizeof(s->fujinet_sd), "%s/SD", s->fujinet_root);
    snprintf(s->fujinet_data, sizeof(s->fujinet_data), "%s/data",
             s->fujinet_root);
    s->fujinet_lib[0] = '\0'; /* resolved lazily in paths_provision_fujinet */

    snprintf(s->webui_url, sizeof(s->webui_url), "http://127.0.0.1:%d/",
             INTVSESSION_WEBUI_PORT);

    return 0;
}

/* Locate libfujinet.{so,dylib}/fujinet.dll and provision the runtime tree
 * (fnconfig.ini + data/ + SD/) on first run, copied from the newest
 * available source: the installed share dir or the dev build output.
 * Modelled closely on fujinet-go-coco-desktop's paths_provision_fujinet. */
int paths_provision_fujinet(struct intvsession *s)
{
    const char *env;
    char src_root[INTV_PATH_MAX];
    char probe[INTV_PATH_MAX];

    if (!s->fujinet_lib[0]) {
        static const char *const names[] = {
            "libfujinet.so", "libfujinet.dylib", "fujinet.dll"};
        const char *const dirs[] = {exe_dir(), INTV_INSTALL_LIBDIR,
                                    INTV_DEV_FUJINET_OUT};
        const size_t nnames = sizeof(names) / sizeof(names[0]);
        const size_t ndirs = sizeof(dirs) / sizeof(dirs[0]);
        size_t di, ni;
        env = getenv("FUJINET_LIB");
        if (env && is_file(env)) {
            snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", env);
        } else {
            for (di = 0; di < ndirs && !s->fujinet_lib[0]; di++)
                for (ni = 0; ni < nnames && !s->fujinet_lib[0]; ni++) {
                    if (!dirs[di][0])
                        continue;
                    snprintf(probe, sizeof(probe), "%s/%s", dirs[di],
                             names[ni]);
                    if (is_file(probe))
                        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib),
                                 "%s", probe);
                }
        }
    }
    if (!s->fujinet_lib[0])
        return -1; /* no runtime available; session runs without FujiNet */

    if (!is_file(s->fujinet_config)) {
        src_root[0] = '\0';
        if (!src_root[0] && exe_dir()[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     exe_dir());
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet", exe_dir());
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     INTV_INSTALL_DATADIR);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet",
                         INTV_INSTALL_DATADIR);
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini",
                     INTV_DEV_FUJINET_OUT);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s",
                         INTV_DEV_FUJINET_OUT);
        }
        if (!src_root[0]) {
            session_set_error(s, "FujiNet runtime data not found (looked in "
                              "%s and %s)", INTV_INSTALL_DATADIR,
                              INTV_DEV_FUJINET_OUT);
            s->fujinet_lib[0] = '\0';
            return -1;
        }
        mkdir_p(s->fujinet_root);
        snprintf(probe, sizeof(probe), "%s/fnconfig.ini", src_root);
        copy_file(probe, s->fujinet_config);
        snprintf(probe, sizeof(probe), "%s/data", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_data);
        snprintf(probe, sizeof(probe), "%s/SD", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_sd);
    }
    mkdir_p(s->fujinet_sd);
    mkdir_p(s->fujinet_data);
    return 0;
}

int intvsession_has_system_roms(const intvsession *s)
{
    char exec_path[INTV_PATH_MAX], grom_path[INTV_PATH_MAX];
    snprintf(exec_path, sizeof(exec_path), "%s/exec.bin", s->roms_dir);
    snprintf(grom_path, sizeof(grom_path), "%s/grom.bin", s->roms_dir);
    return is_file(exec_path) && is_file(grom_path);
}

const char *intvsession_roms_path(const intvsession *s)
{
    return s->roms_dir;
}

const char *intvsession_config_path(const intvsession *s)
{
    return s->config_dir;
}

const char *intvsession_data_path(const intvsession *s)
{
    return s->data_dir;
}

void session_set_error(struct intvsession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
}
