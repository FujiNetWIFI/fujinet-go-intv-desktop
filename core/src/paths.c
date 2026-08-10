/*
 * intvsession path layout: XDG config/data directories and ROM
 * materialisation (delegated to intv_host_provision_roms -- see
 * core/jzintv/intv_host.h, which owns the embedded-ROM table already).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
#else
#include <unistd.h>
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
