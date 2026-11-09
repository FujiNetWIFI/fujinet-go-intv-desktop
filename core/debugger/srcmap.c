/*
 * intvsrcmap -- see srcmap.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "srcmap.h"

#define MAX_FILES    4000   /* file_of[] holds index+1 in 16 bits */
#define MAX_DEPTH    8      /* folder recursion */
#define MAX_INCLUDE  32     /* INCLUDE nesting */
#define PATH_BUF     4096

typedef struct {
    char    *key;       /* canonical path, '/' separators; the lookup key */
    char    *name;      /* what the UI shows */
    char    *text;      /* the file's bytes, NUL-terminated (NULL: not found) */
    char   **lines;     /* pointers into text */
    int      nlines;
    int      maxline;   /* highest line the map placed on this file */
    int32_t *first;     /* [1..first_n): lowest line-start address, or -1 */
    int      first_n;
} sfile;

struct intvsrcmap {
    pthread_mutex_t lock;
    sfile   *files;
    int      nfiles, cap;
    char     root[PATH_BUF];
    unsigned gen;
    uint16_t file_of[65536];    /* file index + 1, 0: not BASIC */
    uint16_t line_of[65536];
    uint8_t  start[65536];
};

/* ---- small helpers ----------------------------------------------------- */

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void norm_slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\')
            *s = '/';
}

static int ci_eq_n(const char *a, const char *b, size_t n)
{
    while (n--) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        if (!*a)
            return 1;
        a++;
        b++;
    }
    return 1;
}

static int ci_prefix(const char *s, const char *word)
{
    return ci_eq_n(s, word, strlen(word));
}

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && ci_eq_n(name + n - e, ext, e);
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (b > s)
        s = b;
    return s ? s + 1 : path;
}

/* Directory part of a '/'-normalised path, "" if none. */
static void dir_of(const char *path, char *dst, size_t n)
{
    const char *b = base_of(path);
    size_t len = (size_t)(b - path);
    if (len > 0)
        len--; /* the slash */
    if (len >= n)
        len = n - 1;
    memcpy(dst, path, len);
    dst[len] = '\0';
}

static void canon_key(const char *p, char *out, size_t n)
{
    char tmp[PATH_BUF];
    size_t len;
#ifdef _WIN32
    if (!_fullpath(tmp, p, sizeof(tmp)))
        snprintf(tmp, sizeof(tmp), "%s", p);
#else
    if (!realpath(p, tmp))
        snprintf(tmp, sizeof(tmp), "%s", p);
#endif
    norm_slashes(tmp);
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    snprintf(out, n, "%s", tmp);
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Whole file -> NUL-terminated buffer, split in place into lines. */
static int read_lines(const char *path, char **text, char ***lines, int *n)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf, **ls, *p;
    int count = 0, i;
    size_t got;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 64L * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    for (p = buf; *p; p++)
        if (*p == '\n')
            count++;
    if (got > 0 && buf[got - 1] != '\n')
        count++;
    ls = malloc((size_t)(count ? count : 1) * sizeof(char *));
    if (!ls) {
        free(buf);
        return 0;
    }
    p = buf;
    for (i = 0; i < count; i++) {
        char *e = strchr(p, '\n');
        ls[i] = p;
        if (e) {
            *e = '\0';
            if (e > p && e[-1] == '\r')
                e[-1] = '\0';
            p = e + 1;
        } else {
            size_t l = strlen(p);
            if (l && p[l - 1] == '\r')
                p[l - 1] = '\0';
        }
    }
    *text = buf;
    *lines = ls;
    *n = count;
    return 1;
}

/* ---- lifecycle --------------------------------------------------------- */

static void free_files(intvsrcmap *m)
{
    int i;
    for (i = 0; i < m->nfiles; i++) {
        free(m->files[i].key);
        free(m->files[i].name);
        free(m->files[i].text);
        free(m->files[i].lines);
        free(m->files[i].first);
    }
    free(m->files);
    m->files = NULL;
    m->nfiles = m->cap = 0;
}

static void reset_tables(intvsrcmap *m)
{
    memset(m->file_of, 0, sizeof(m->file_of));
    memset(m->line_of, 0, sizeof(m->line_of));
    memset(m->start, 0, sizeof(m->start));
    m->root[0] = '\0';
}

intvsrcmap *intvsrcmap_create(void)
{
    intvsrcmap *m = calloc(1, sizeof(*m));
    if (m)
        pthread_mutex_init(&m->lock, NULL);
    return m;
}

void intvsrcmap_destroy(intvsrcmap *m)
{
    if (!m)
        return;
    free_files(m);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

intvsrcmap *intvsrcmap_shared(void)
{
    static intvsrcmap *shared;
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (!shared)
        shared = intvsrcmap_create();
    pthread_mutex_unlock(&once);
    return shared;
}

void intvsrcmap_clear(intvsrcmap *m)
{
    pthread_mutex_lock(&m->lock);
    free_files(m);
    reset_tables(m);
    m->gen++;
    pthread_mutex_unlock(&m->lock);
}

unsigned intvsrcmap_generation(const intvsrcmap *m)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    unsigned g;
    pthread_mutex_lock(&mm->lock);
    g = mm->gen;
    pthread_mutex_unlock(&mm->lock);
    return g;
}

/* ---- files (no locking below: callers hold the lock or own the map) ---- */

static int push_file(intvsrcmap *m, const char *key, const char *name)
{
    sfile *f;
    if (m->nfiles >= MAX_FILES)
        return -1;
    if (m->nfiles == m->cap) {
        int ncap = m->cap ? m->cap * 2 : 16;
        sfile *nf = realloc(m->files, (size_t)ncap * sizeof(sfile));
        if (!nf)
            return -1;
        m->files = nf;
        m->cap = ncap;
    }
    f = &m->files[m->nfiles];
    memset(f, 0, sizeof(*f));
    f->key = dupstr(key);
    f->name = dupstr(name);
    if (!f->key || !f->name) {
        free(f->key);
        free(f->name);
        return -1;
    }
    return m->nfiles++;
}

static int find_key(const intvsrcmap *m, const char *key)
{
    int i;
    for (i = 0; i < m->nfiles; i++)
        if (strcmp(m->files[i].key, key) == 0)
            return i;
    return -1;
}

/* Does the file's key end with `suffix` on a path-component boundary? */
static int key_has_suffix(const char *key, const char *suffix)
{
    size_t kl = strlen(key), sl = strlen(suffix);
    if (!sl || sl > kl)
        return 0;
    if (!ci_eq_n(key + kl - sl, suffix, sl))
        return 0;
    return sl == kl || key[kl - sl - 1] == '/';
}

/* Longest-suffix-first: "/other/box/util.bas" tries that, then
 * "other/box/util.bas", "box/util.bas", "util.bas". A map built on another
 * machine still lands on the right file, even with two util.bas loaded. */
static int match_loaded(const intvsrcmap *m, const char *name)
{
    const char *p = name;
    while (*p) {
        int i;
        for (i = 0; i < m->nfiles; i++)
            if (key_has_suffix(m->files[i].key, p))
                return i;
        p = strchr(p, '/');
        if (!p)
            break;
        p++;
    }
    return -1;
}

/* `INCLUDE "x.bas"` at the start of a statement, not inside a comment. */
static int parse_include(const char *line, char *out, size_t n)
{
    const char *p = line, *q;
    size_t len;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!ci_prefix(p, "INCLUDE") || (p[7] != ' ' && p[7] != '\t'))
        return 0;
    p += 7;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return 0;
    p++;
    q = strchr(p, '"');
    if (!q || q == p)
        return 0;
    len = (size_t)(q - p);
    if (len >= n)
        return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    norm_slashes(out);
    return 1;
}

static void display_name(const intvsrcmap *m, const char *key, char *out, size_t n)
{
    size_t rl = strlen(m->root);
    if (rl && strncmp(key, m->root, rl) == 0 && key[rl] == '/')
        snprintf(out, n, "%s", key + rl + 1);
    else
        snprintf(out, n, "%s", base_of(key));
}

static int add_file(intvsrcmap *m, const char *path, int depth)
{
    char key[PATH_BUF], name[PATH_BUF], dir[PATH_BUF];
    char *text, **lines;
    int n, idx, i;

    canon_key(path, key, sizeof(key));
    idx = find_key(m, key);
    if (idx >= 0 && m->files[idx].text)
        return idx;
    if (!read_lines(key, &text, &lines, &n))
        return -1;

    if (idx < 0) {
        display_name(m, key, name, sizeof(name));
        idx = push_file(m, key, name);
        if (idx < 0) {
            free(text);
            free(lines);
            return -1;
        }
    }
    m->files[idx].text = text;
    m->files[idx].lines = lines;
    m->files[idx].nlines = n;

    /* INCLUDEd files: beside this one first, then beside the import root.
     * (The compiler also searches its library folder; a map names those
     * files, and intvsrcmap_load_map finds them from the names.) */
    dir_of(key, dir, sizeof(dir));
    for (i = 0; i < n && depth < MAX_INCLUDE; i++) {
        char inc[PATH_BUF], cand[PATH_BUF];
        if (!parse_include(lines[i], inc, sizeof(inc)))
            continue;
        snprintf(cand, sizeof(cand), "%s/%s", dir, inc);
        if (!is_file(cand) && m->root[0])
            snprintf(cand, sizeof(cand), "%s/%s", m->root, inc);
        if (is_file(cand))
            add_file(m, cand, depth + 1);
        /* lines/files may have moved: re-read through the index */
        lines = m->files[idx].lines;
    }
    return idx;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(intvsrcmap *m, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    int n = 0, cap = 0, i;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (n == cap) {
            char **nn = realloc(names, (size_t)(cap ? cap * 2 : 32) * sizeof(char *));
            if (!nn)
                break;
            names = nn;
            cap = cap ? cap * 2 : 32;
        }
        names[n] = dupstr(e->d_name);
        if (names[n])
            n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_names);
    for (i = 0; i < n; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (is_dir(path)) {
            if (depth < MAX_DEPTH)
                walk(m, path, depth + 1);
        } else if (has_ext(names[i], ".bas")) {
            add_file(m, path, 0);
        }
        free(names[i]);
    }
    free(names);
}

/* ---- the map ------------------------------------------------------------ */

/* A map name -> file index; loads it from disk if it isn't loaded yet, and
 * as a last resort makes a text-less entry so the lines still resolve to
 * "file:line" in the UI. -1 for the assembler's own files. */
static int resolve_name(intvsrcmap *m, const char *raw, const char *mapdir)
{
    char name[1024], cand[PATH_BUF];
    const char *tries[4];
    int idx, i;
    const char *nm = name;

    snprintf(name, sizeof(name), "%s", raw);
    norm_slashes(name);
    while (nm[0] == '.' && nm[1] == '/')
        nm += 2;
    if (!*nm || has_ext(nm, ".asm"))
        return -1;

    idx = match_loaded(m, nm);
    if (idx >= 0)
        return idx;

    snprintf(cand, sizeof(cand), "%s/%s", mapdir, nm);
    tries[0] = nm;
    tries[1] = cand;
    for (i = 0; i < 2; i++) {
        if (is_file(tries[i])) {
            idx = add_file(m, tries[i], 0);
            if (idx >= 0)
                return idx;
        }
    }
    snprintf(cand, sizeof(cand), "%s/%s", mapdir, base_of(nm));
    if (is_file(cand) && (idx = add_file(m, cand, 0)) >= 0)
        return idx;
    if (m->root[0]) {
        snprintf(cand, sizeof(cand), "%s/%s", m->root, nm);
        if (is_file(cand) && (idx = add_file(m, cand, 0)) >= 0)
            return idx;
    }
    return push_file(m, nm, nm);
}

static int apply_range(intvsrcmap *m, int file, long line, unsigned lo, unsigned hi)
{
    unsigned a;
    if (file < 0 || line < 1 || line > 65535 || lo > hi || hi > 0xFFFF)
        return 0;
    for (a = lo; a <= hi; a++) {
        m->file_of[a] = (uint16_t)(file + 1);
        m->line_of[a] = (uint16_t)line;
    }
    if (line > m->files[file].maxline)
        m->files[file].maxline = (int)line;
    return 1;
}

static int hex4(const char *s)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return s[4] == '\0' || s[4] == ' ' || s[4] == '\t';
}

/* The number of consecutive 4-digit hex words before the listing's source
 * column (which starts at the first tab). */
static int count_words(const char *p)
{
    int n = 0;
    while (n < 3) {
        while (*p == ' ')
            p++;
        if (!hex4(p) || *p == '\0')
            break;
        n++;
        p += 4;
    }
    return n;
}

/* Reads either kind; returns ranges placed, or -1 if unreadable. */
static int parse_map(intvsrcmap *m, const char *path)
{
    char *text, **lines;
    int n, i, cur = -1, placed = 0, is_smap = 0;
    long curline = 0;
    char mapdir[PATH_BUF], key[PATH_BUF];

    if (!read_lines(path, &text, &lines, &n))
        return -1;
    canon_key(path, key, sizeof(key));
    dir_of(key, mapdir, sizeof(mapdir));

    for (i = 0; i < n && !is_smap; i++)
        if (strncmp(lines[i], "FILE ", 5) == 0)
            is_smap = 1;

    for (i = 0; i < n; i++) {
        const char *l = lines[i];
        if (is_smap) {
            unsigned lo, hi, flags;
            int sl, ll;
            if (strncmp(l, "FILE ", 5) == 0) {
                const char *nm = l + 5;
                while (*nm == ' ')
                    nm++;
                cur = resolve_name(m, nm, mapdir);
            } else if (sscanf(l, "%x %x %x %d %d", &lo, &hi, &flags, &sl, &ll) >= 4) {
                placed += apply_range(m, cur, sl, lo, hi);
            }
        } else {
            const char *s = strstr(l, "SRCFILE");
            if (s && strspn(l, " \t") == (size_t)(s - l)) {
                const char *q = strchr(s, '"');
                const char *e = q ? strchr(q + 1, '"') : NULL;
                if (q && e) {
                    char nm[1024];
                    size_t len = (size_t)(e - q - 1);
                    if (len >= sizeof(nm))
                        len = sizeof(nm) - 1;
                    memcpy(nm, q + 1, len);
                    nm[len] = '\0';
                    curline = strtol(e + 1 + (e[1] == ',' ? 1 : 0), NULL, 10);
                    cur = nm[0] ? resolve_name(m, nm, mapdir) : -1;
                }
            } else if (cur >= 0 && hex4(l) && l[4] == ' ') {
                const char *tab = strchr(l, '\t');
                char region[64];
                size_t len = tab ? (size_t)(tab - l) : strlen(l);
                int words;
                if (len < 5)
                    continue;
                if (len >= sizeof(region))
                    len = sizeof(region) - 1;
                memcpy(region, l + 5, len - 5);
                region[len - 5] = '\0';
                words = count_words(region);
                if (words > 0) {
                    unsigned addr = (unsigned)strtoul(l, NULL, 16);
                    placed += apply_range(m, cur, curline, addr,
                                          addr + (unsigned)words - 1);
                }
            }
        }
    }
    free(text);
    free(lines);
    return placed;
}

/* Line starts and the per-file first-address tables, from file_of/line_of. */
static void rebuild(intvsrcmap *m)
{
    unsigned a;
    int i;

    for (a = 0; a < 65536; a++) {
        m->start[a] = m->file_of[a] != 0 &&
                      (a == 0 || m->file_of[a - 1] != m->file_of[a] ||
                       m->line_of[a - 1] != m->line_of[a]);
    }
    for (i = 0; i < m->nfiles; i++) {
        sfile *f = &m->files[i];
        int cap = (f->nlines > f->maxline ? f->nlines : f->maxline) + 2, k;
        free(f->first);
        f->first = malloc((size_t)cap * sizeof(int32_t));
        f->first_n = f->first ? cap : 0;
        for (k = 0; k < f->first_n; k++)
            f->first[k] = -1;
    }
    for (a = 0; a < 65536; a++) {
        sfile *f;
        if (!m->start[a])
            continue;
        f = &m->files[m->file_of[a] - 1];
        if (f->first && m->line_of[a] < f->first_n && f->first[m->line_of[a]] < 0)
            f->first[m->line_of[a]] = (int32_t)a;
    }
}

int intvsrcmap_load_map(intvsrcmap *m, const char *path)
{
    int placed;
    pthread_mutex_lock(&m->lock);
    placed = parse_map(m, path);
    if (placed >= 0) {
        rebuild(m);
        m->gen++;
    }
    pthread_mutex_unlock(&m->lock);
    return placed;
}

/* ---- import -------------------------------------------------------------- */

/* How many distinct loaded files does this map name? */
static int score_map(const intvsrcmap *m, const char *path)
{
    char *text, **lines;
    int n, i, score = 0;
    unsigned char *seen;

    if (!m->nfiles || !read_lines(path, &text, &lines, &n))
        return 0;
    seen = calloc((size_t)m->nfiles, 1);
    for (i = 0; seen && i < n; i++) {
        const char *l = lines[i], *s;
        char nm[1024];
        int idx;
        if (strncmp(l, "FILE ", 5) == 0) {
            s = l + 5;
            while (*s == ' ')
                s++;
            snprintf(nm, sizeof(nm), "%s", s);
        } else if ((s = strstr(l, "SRCFILE")) != NULL && (s = strchr(s, '"')) != NULL) {
            const char *e = strchr(s + 1, '"');
            size_t len = e ? (size_t)(e - s - 1) : 0;
            if (!len || len >= sizeof(nm))
                continue;
            memcpy(nm, s + 1, len);
            nm[len] = '\0';
        } else {
            continue;
        }
        norm_slashes(nm);
        idx = has_ext(nm, ".asm") ? -1 : match_loaded(m, nm);
        if (idx >= 0 && !seen[idx]) {
            seen[idx] = 1;
            score++;
        }
    }
    free(seen);
    free(text);
    free(lines);
    return score;
}

/* Siblings of the imported file first, then every map in its folder. */
static int find_map(const intvsrcmap *m, const char *file_key, const char *dir,
                    char *out, size_t outsz)
{
    char cand[PATH_BUF];
    int have = 0, best_score = 0, pass;
    DIR *d;
    struct dirent *e;

    out[0] = '\0';
    if (file_key) {
        static const char *const exts[] = { ".smap", ".lst" };
        char stem[PATH_BUF];
        char *dot;
        int i;
        snprintf(stem, sizeof(stem), "%s", file_key);
        dot = strrchr(stem, '.');
        if (dot && dot > base_of(stem))
            *dot = '\0';
        for (i = 0; i < 2; i++) {
            snprintf(cand, sizeof(cand), "%s%s", stem, exts[i]);
            if (is_file(cand)) {
                int sc = score_map(m, cand);
                if (!have || sc > best_score) {
                    have = 1;
                    best_score = sc;
                    snprintf(out, outsz, "%s", cand);
                }
            }
        }
    }
    for (pass = 0; pass < 2; pass++) {
        const char *ext = pass == 0 ? ".smap" : ".lst";
        d = opendir(dir);
        if (!d)
            break;
        while ((e = readdir(d)) != NULL) {
            int sc;
            if (!has_ext(e->d_name, ext))
                continue;
            snprintf(cand, sizeof(cand), "%s/%s", dir, e->d_name);
            sc = score_map(m, cand);
            if (sc > 0 && (!have || sc > best_score)) {
                have = 1;
                best_score = sc;
                snprintf(out, outsz, "%s", cand);
            }
        }
        closedir(d);
    }
    return have;
}

int intvsrcmap_load_sibling_sym(const char *map_path, intvsymtab *sym)
{
    char sympath[PATH_BUF], *dot;
    int n;

    if (!sym || !map_path || !*map_path)
        return 0;
    snprintf(sympath, sizeof(sympath), "%s", map_path);
    dot = strrchr(sympath, '.');
    if (!dot || dot < base_of(sympath) ||
        strlen(".sym") >= sizeof(sympath) - (size_t)(dot - sympath))
        return 0;
    strcpy(dot, ".sym");
    if (!is_file(sympath))
        return 0;
    n = intvsymtab_load(sym, sympath);
    return n > 0 ? n : 0;
}

int intvsrcmap_import(intvsrcmap *m, const char *path, intvsymtab *sym,
                      intvsrcmap_import_result *r)
{
    intvsrcmap *t;
    char key[PATH_BUF], dir[PATH_BUF], mapfile[PATH_BUF];
    int isdir, mapped = 0, symbols = 0, files;

    if (r)
        memset(r, 0, sizeof(*r));
    if (!path)
        return -1;
    isdir = is_dir(path);
    if (!isdir && !is_file(path))
        return -1;
    t = intvsrcmap_create();
    if (!t)
        return -1;

    canon_key(path, key, sizeof(key));
    if (isdir) {
        snprintf(t->root, sizeof(t->root), "%s", key);
        snprintf(dir, sizeof(dir), "%s", key);
        walk(t, key, 0);
    } else {
        dir_of(key, t->root, sizeof(t->root));
        snprintf(dir, sizeof(dir), "%s", t->root);
        add_file(t, key, 0);
    }
    if (t->nfiles == 0) {
        intvsrcmap_destroy(t);
        return -1;
    }

    mapfile[0] = '\0';
    if (find_map(t, isdir ? NULL : key, dir, mapfile, sizeof(mapfile))) {
        mapped = parse_map(t, mapfile);
        if (mapped < 0)
            mapped = 0;
        symbols = intvsrcmap_load_sibling_sym(mapfile, sym);
    }
    rebuild(t);
    files = t->nfiles;

    /* Swap the finished map in under the real map's lock. */
    pthread_mutex_lock(&m->lock);
    free_files(m);
    m->files = t->files;
    m->nfiles = t->nfiles;
    m->cap = t->cap;
    memcpy(m->file_of, t->file_of, sizeof(m->file_of));
    memcpy(m->line_of, t->line_of, sizeof(m->line_of));
    memcpy(m->start, t->start, sizeof(m->start));
    snprintf(m->root, sizeof(m->root), "%s", t->root);
    m->gen++;
    pthread_mutex_unlock(&m->lock);
    t->files = NULL;
    t->nfiles = t->cap = 0;
    intvsrcmap_destroy(t);

    if (r) {
        r->files = files;
        r->mapped = mapped;
        r->symbols = symbols;
        snprintf(r->map_path, sizeof(r->map_path), "%s", mapfile);
    }
    return files;
}

/* ---- queries ------------------------------------------------------------- */

int intvsrcmap_file_count(const intvsrcmap *m)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    int n;
    pthread_mutex_lock(&mm->lock);
    n = mm->nfiles;
    pthread_mutex_unlock(&mm->lock);
    return n;
}

const char *intvsrcmap_file_name(const intvsrcmap *m, int file)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    const char *s = NULL;
    pthread_mutex_lock(&mm->lock);
    if (file >= 0 && file < mm->nfiles)
        s = mm->files[file].name;
    pthread_mutex_unlock(&mm->lock);
    return s;
}

int intvsrcmap_line_count(const intvsrcmap *m, int file)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    int n = 0;
    pthread_mutex_lock(&mm->lock);
    if (file >= 0 && file < mm->nfiles)
        n = mm->files[file].nlines;
    pthread_mutex_unlock(&mm->lock);
    return n;
}

const char *intvsrcmap_line_text(const intvsrcmap *m, int file, int line)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    const char *s = NULL;
    pthread_mutex_lock(&mm->lock);
    if (file >= 0 && file < mm->nfiles && line >= 1 &&
        line <= mm->files[file].nlines)
        s = mm->files[file].lines[line - 1];
    pthread_mutex_unlock(&mm->lock);
    return s;
}

int intvsrcmap_lookup(const intvsrcmap *m, uint16_t addr, int *file, int *line)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    int hit;
    pthread_mutex_lock(&mm->lock);
    hit = mm->file_of[addr] != 0;
    if (hit) {
        if (file)
            *file = mm->file_of[addr] - 1;
        if (line)
            *line = mm->line_of[addr];
    }
    pthread_mutex_unlock(&mm->lock);
    return hit;
}

int intvsrcmap_is_line_start(const intvsrcmap *m, uint16_t addr)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    int hit;
    pthread_mutex_lock(&mm->lock);
    hit = mm->start[addr];
    pthread_mutex_unlock(&mm->lock);
    return hit;
}

int intvsrcmap_first_addr(const intvsrcmap *m, int file, int line)
{
    intvsrcmap *mm = (intvsrcmap *)m;
    int a = -1;
    pthread_mutex_lock(&mm->lock);
    if (file >= 0 && file < mm->nfiles && line >= 1 &&
        line < mm->files[file].first_n)
        a = mm->files[file].first[line];
    pthread_mutex_unlock(&mm->lock);
    return a;
}
