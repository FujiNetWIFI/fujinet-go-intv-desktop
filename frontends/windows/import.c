/*
 * Importing the BIOS (exec.bin, grom.bin, ecs.bin) and cartridges for the SD
 * host: the common file dialog (several files at once) or the folder
 * browser, each path through the session, then one message box saying what
 * came of it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "import.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "win_common.h"

static const char k_bios_filter[] =
    "BIOS images and ROM sets\0*.zip;*.rom;*.bin\0All files\0*.*\0\0";
static const char k_cart_filter[] =
    "Cartridges and ROM sets\0*.rom;*.bin;*.int;*.itv;*.cfg;*.zip\0All files\0*.*\0\0";

/* A GetOpenFileName multi-select buffer: "dir\0name\0name\0\0", or one full
 * path when a single file was chosen. Calls fn for each full path. */
static int pick_each(HWND owner, const char *filter, const char *title, int single,
                     void (*fn)(const char *path, void *ctx), void *ctx)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                (single ? 0 : OFN_ALLOWMULTISELECT);
    if (!GetOpenFileNameA(&ofn))
        return 0;
    p = buf + strlen(buf) + 1;
    if (single || !*p) {
        fn(buf, ctx);
        return 1;
    }
    for (; *p; p += strlen(p) + 1) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\%s", buf, p);
        fn(path, ctx);
    }
    return 1;
}

/* ---- BIOS ---- */

typedef struct {
    intvsession *session;
    char log[4096];
    int imported;
} import_log;

static void import_one(const char *path, void *ctx)
{
    import_log *l = ctx;
    char msg[1024];
    size_t len = strlen(l->log);
    msg[0] = '\0';
    if (intvsession_rom_import(l->session, path, msg, sizeof msg) == 0)
        l->imported++;
    snprintf(l->log + len, sizeof l->log - len, "%s%s", len ? "\n" : "", msg);
}

static int pick_folder(HWND owner, const char *title, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListA(pidl, dir))
        dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

int intv_import_bios(HWND owner, intvsession *session, int folder, int single)
{
    static import_log l;
    memset(&l, 0, sizeof l);
    l.session = session;
    if (folder) {
        char dir[MAX_PATH];
        if (!pick_folder(owner, "Import BIOS from a Folder (MAME's intv set works)", dir, sizeof dir))
            return -1;
        import_one(dir, &l);
    } else if (!pick_each(owner, k_bios_filter, "Import BIOS", single, import_one, &l)) {
        return -1;
    }
    if (l.log[0])
        intv_message(owner, l.log, l.imported ? "BIOS Imported" : "Nothing Imported",
                     (l.imported ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    return l.imported;
}

/* ---- cartridges for the SD host ---- */

typedef struct {
    intvsession *session;
    int ok, failed;
    char first_name[MAX_PATH];
    char errors[2048];
} cart_log;

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : path;
}

static void import_cart(const char *path, void *ctx)
{
    cart_log *l = ctx;
    char dest[1024];
    dest[0] = '\0';
    if (intvsession_import_cart_to_sd(l->session, path, dest, sizeof dest) == 0) {
        if (!l->ok)
            snprintf(l->first_name, sizeof l->first_name, "%s", base_name(dest[0] ? dest : path));
        l->ok++;
    } else {
        size_t len = strlen(l->errors);
        l->failed++;
        snprintf(l->errors + len, sizeof l->errors - len, "%s%s", len ? "\n" : "",
                 intvsession_last_error(l->session));
    }
}

int intv_import_carts(HWND owner, intvsession *session)
{
    static cart_log l;
    char note[2400];
    memset(&l, 0, sizeof l);
    l.session = session;
    if (!pick_each(owner, k_cart_filter, "Import Cartridge to SD", 0, import_cart, &l))
        return -1;
    if (l.failed) {
        if (l.ok)
            snprintf(note, sizeof note, "%d imported, %d failed.\n%s", l.ok, l.failed, l.errors);
        else
            snprintf(note, sizeof note, "%s", l.errors);
        intv_message(owner, note, "Import failed", MB_ICONWARNING | MB_OK);
    } else if (l.ok == 1) {
        snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", l.first_name);
        intv_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    } else if (l.ok > 1) {
        snprintf(note, sizeof note, "%d files are on the SD host. Boot them from the CONFIG client.", l.ok);
        intv_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    }
    return l.ok;
}

void intv_open_sd_folder(HWND owner, intvsession *session)
{
    const char *sd = intvsession_sd_path(session);
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s", sd ? sd : "");
    for (char *c = path; *c; c++)
        if (*c == '/') *c = '\\';
    if (!path[0])
        return;
    SHCreateDirectoryExA(NULL, path, NULL);     /* it may not exist yet */
    ShellExecuteA(owner, "explore", path, NULL, NULL, SW_SHOWNORMAL);
}
