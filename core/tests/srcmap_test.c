/*
 * srcmap_test -- pure function test, no emulator: the IntyBASIC source map.
 *
 * The fixtures in tests/data/srcmap are real toolchain output (intybasic
 * 1.4.2 + as1600): main.bas INCLUDEs inc.bas, and main.smap / main.lst are
 * what `as1600 -j -l` wrote for them. The .lst is trimmed to the BASIC part.
 * SRCMAP_FIXTURE_DIR is set by CMake.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0700)
#endif

#include "srcmap.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "srcmap_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static int find_file(const intvsrcmap *m, const char *name)
{
    int i;
    for (i = 0; i < intvsrcmap_file_count(m); i++)
        if (strcmp(intvsrcmap_file_name(m, i), name) == 0)
            return i;
    return -1;
}

static int write_text(const char *dir, const char *name, const char *text)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

static int copy_fixture(const char *dir, const char *name)
{
    char src[1100], text[8192];
    FILE *f;
    size_t n;
    snprintf(src, sizeof(src), "%s/%s", SRCMAP_FIXTURE_DIR, name);
    f = fopen(src, "rb");
    if (!f)
        return 0;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return write_text(dir, name, text);
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

static void rm(const char *dir, const char *name)
{
    char p[1100];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    remove(p);
}

int main(void)
{
    intvsrcmap *m = intvsrcmap_create();
    intvsymtab *sym = intvsymtab_create();
    intvsrcmap_import_result r;
    int mainf, incf, f, l, a;

    check("create", m && sym);

    /* ---- folder import: both files, the .smap, the .sym beside it ---- */
    check("folder import", intvsrcmap_import(m, SRCMAP_FIXTURE_DIR, sym, &r) == 2);
    check("two files", r.files == 2 && intvsrcmap_file_count(m) == 2);
    check("picked the smap", ends_with(r.map_path, "main.smap"));
    check("ranges placed", r.mapped >= 10);
    check("symbols loaded", r.symbols > 10);
    check("a loaded symbol resolves",
          intvsymtab_lookup_name(sym, "label_LOOP", &(uint16_t){0}) ||
          intvsymtab_lookup_name(sym, "var_X", &(uint16_t){0}));
    mainf = find_file(m, "main.bas");
    incf = find_file(m, "inc.bas");
    check("main.bas loaded", mainf >= 0);
    check("inc.bas loaded", incf >= 0);
    check("bogus file index", intvsrcmap_file_name(m, 99) == NULL &&
                              intvsrcmap_line_text(m, 99, 1) == NULL);

    /* ---- text ---- */
    check("line count", intvsrcmap_line_count(m, mainf) == 9);
    check("line text", intvsrcmap_line_text(m, mainf, 4) &&
          strstr(intvsrcmap_line_text(m, mainf, 4), "x = x + 1"));
    check("line 0 and past the end are NULL",
          !intvsrcmap_line_text(m, mainf, 0) &&
          !intvsrcmap_line_text(m, mainf, 10));

    /* ---- address -> line: the values as1600 wrote in main.smap ---- */
    check("0x50C5 is main.bas:2", intvsrcmap_lookup(m, 0x50C5, &f, &l) &&
          f == mainf && l == 2);
    check("0x50C6 is still line 2", intvsrcmap_lookup(m, 0x50C6, &f, &l) &&
          f == mainf && l == 2);
    check("0x50D5 is inc.bas:2", intvsrcmap_lookup(m, 0x50D5, &f, &l) &&
          f == incf && l == 2);
    check("the prologue is not BASIC", !intvsrcmap_lookup(m, 0x5000, &f, &l));
    check("the epilogue is not BASIC", !intvsrcmap_lookup(m, 0x50E2, &f, &l));
    check("NULL out pointers are fine", intvsrcmap_lookup(m, 0x50C5, NULL, NULL));

    /* ---- line starts: the first word of each line, not the rest ---- */
    check("0x50C5 starts a line", intvsrcmap_is_line_start(m, 0x50C5));
    check("0x50C6 does not", !intvsrcmap_is_line_start(m, 0x50C6));
    check("0x50C8 starts line 4", intvsrcmap_is_line_start(m, 0x50C8));
    check("0x50CA is mid-line 4", !intvsrcmap_is_line_start(m, 0x50CA));
    check("0x50D5 starts inc.bas:2", intvsrcmap_is_line_start(m, 0x50D5));
    check("unmapped is no start", !intvsrcmap_is_line_start(m, 0x5000));

    /* ---- line -> address (breakpoints) ---- */
    check("first_addr main:4", intvsrcmap_first_addr(m, mainf, 4) == 0x50C8);
    check("first_addr main:5 (GOSUB)", intvsrcmap_first_addr(m, mainf, 5) == 0x50CD);
    check("a label has no code", intvsrcmap_first_addr(m, mainf, 3) == -1);
    check("a comment has no code", intvsrcmap_first_addr(m, mainf, 1) == -1);
    check("first_addr inc:3", intvsrcmap_first_addr(m, incf, 3) == 0x50D6);
    check("first_addr out of range", intvsrcmap_first_addr(m, mainf, 5000) == -1 &&
                                     intvsrcmap_first_addr(m, 77, 1) == -1);

    /* ---- generation moves on every change ---- */
    {
        unsigned g = intvsrcmap_generation(m);
        intvsrcmap_clear(m);
        check("clear bumps generation", intvsrcmap_generation(m) != g);
        check("clear empties", intvsrcmap_file_count(m) == 0 &&
                               !intvsrcmap_is_line_start(m, 0x50C5));
    }

    /* ---- single file: INCLUDE pulls in inc.bas, sibling .smap found ---- */
    {
        char one[1100];
        snprintf(one, sizeof(one), "%s/main.bas", SRCMAP_FIXTURE_DIR);
        check("file import", intvsrcmap_import(m, one, NULL, &r) == 2);
        check("INCLUDE followed", find_file(m, "inc.bas") >= 0);
        check("sibling smap", ends_with(r.map_path, "main.smap") && r.mapped >= 10);
    }

    /* ---- the .lst reads to exactly the same map as the .smap ---- */
    {
        intvsrcmap *lm = intvsrcmap_create();
        char tmp[1024];
        int mismatches = 0, lines_seen = 0;
        test_tmp_template(tmp, sizeof(tmp), "intv-srcmap-lst-");
        check("mkdtemp", mkdtemp(tmp) != NULL);
        check("copy", copy_fixture(tmp, "main.bas") && copy_fixture(tmp, "inc.bas") &&
                      copy_fixture(tmp, "main.lst"));
        check("lst import", intvsrcmap_import(lm, tmp, NULL, &r) == 2);
        check("picked the lst", ends_with(r.map_path, "main.lst") && r.mapped >= 10);
        for (a = 0; a < 0x10000; a++) {
            int f1, l1, f2, l2;
            int h1 = intvsrcmap_lookup(m, (uint16_t)a, &f1, &l1);
            int h2 = intvsrcmap_lookup(lm, (uint16_t)a, &f2, &l2);
            if (h1 != h2 ||
                (h1 && (l1 != l2 || strcmp(intvsrcmap_file_name(m, f1),
                                           intvsrcmap_file_name(lm, f2)) != 0)))
                mismatches++;
            if (h1)
                lines_seen++;
            if (intvsrcmap_is_line_start(m, (uint16_t)a) !=
                intvsrcmap_is_line_start(lm, (uint16_t)a))
                mismatches++;
        }
        check("smap and lst agree on every address", mismatches == 0);
        check("and there was something to compare", lines_seen > 20);
        intvsrcmap_destroy(lm);
        rm(tmp, "main.bas"); rm(tmp, "inc.bas"); rm(tmp, "main.lst");
        rmdir(tmp);
    }

    /* ---- Load Map on an empty map reads the sources from the map's folder */
    {
        intvsrcmap *e = intvsrcmap_create();
        char mp[1100];
        snprintf(mp, sizeof(mp), "%s/main.smap", SRCMAP_FIXTURE_DIR);
        check("load_map", intvsrcmap_load_map(e, mp) >= 10);
        check("sources found beside the map", intvsrcmap_file_count(e) == 2 &&
              find_file(e, "main.bas") >= 0 && find_file(e, "inc.bas") >= 0);
        check("with their text", intvsrcmap_line_count(e, find_file(e, "inc.bas")) == 5);
        check("unreadable map", intvsrcmap_load_map(e, "/no/such/file.smap") == -1);
        intvsrcmap_destroy(e);
    }

    /* ---- names from another machine, and files that are gone ---- */
    {
        intvsrcmap *e = intvsrcmap_create();
        char tmp[1024], dir[1100];
        snprintf(dir, sizeof(dir), "%s", SRCMAP_FIXTURE_DIR);
        check("import", intvsrcmap_import(e, dir, NULL, NULL) == 2);
        test_tmp_template(tmp, sizeof(tmp), "intv-srcmap-foreign-");
        check("mkdtemp", mkdtemp(tmp) != NULL);
        check("write foreign map", write_text(tmp, "other.smap",
              "CWD /somewhere/else\nPATH .\n"
              "FILE /somewhere/else/proj/inc.bas\n6000 6001 10 3 0\n"
              "FILE ..\\lib\\vanished.bas\n6002 6003 10 7 0\n"
              "FILE game.asm\n6004 6004 10 1 0\n"));
        {
            char mp[1100];
            snprintf(mp, sizeof(mp), "%s/other.smap", tmp);
            check("foreign map placed 2 BASIC ranges",
                  intvsrcmap_load_map(e, mp) == 2);
        }
        incf = find_file(e, "inc.bas");
        check("matched by base name", intvsrcmap_lookup(e, 0x6000, &f, &l) &&
              f == incf && l == 3);
        check("a vanished file still maps", intvsrcmap_lookup(e, 0x6002, &f, &l) &&
              l == 7 && intvsrcmap_file_count(e) == 3);
        check("with no text", intvsrcmap_line_text(e, f, 7) == NULL &&
                              intvsrcmap_line_count(e, f) == 0);
        check(".asm is not BASIC", !intvsrcmap_lookup(e, 0x6004, &f, &l));
        intvsrcmap_destroy(e);
        rm(tmp, "other.smap");
        rmdir(tmp);
    }

    /* ---- folders: recursion, sub-folder INCLUDE, cycles ---- */
    {
        intvsrcmap *e = intvsrcmap_create();
        char tmp[1024], sub[1100], deep[1100];
        test_tmp_template(tmp, sizeof(tmp), "intv-srcmap-tree-");
        check("mkdtemp", mkdtemp(tmp) != NULL);
        snprintf(sub, sizeof(sub), "%s/lib", tmp);
        snprintf(deep, sizeof(deep), "%s/lib/deep", tmp);
        check("mkdir", MKDIR(sub) == 0 && MKDIR(deep) == 0);
        check("write", write_text(tmp, "main.bas",
                  "include \"lib/util.bas\"\nINCLUDE \"lib/util.bas\"\n"
                  "REM INCLUDE \"nope.bas\"\n  Include \"lib/deep/d.bas\"\n") &&
              write_text(sub, "util.bas",
                  "x = 1\nINCLUDE \"../main.bas\"\n") /* a cycle */ &&
              write_text(deep, "d.bas", "y = 2\n") &&
              write_text(tmp, "notes.txt", "INCLUDE \"x\"\n"));

        check("folder import finds every .bas, INCLUDEs and the cycle once each",
              intvsrcmap_import(e, tmp, NULL, &r) == 3);
        check("recursively, named relative to the root", r.files == 3 &&
              find_file(e, "main.bas") >= 0 && find_file(e, "lib/util.bas") >= 0 &&
              find_file(e, "lib/deep/d.bas") >= 0);
        check("no map is reported as none", r.mapped == 0 && r.map_path[0] == '\0');

        {
            char one[1100];
            snprintf(one, sizeof(one), "%s/main.bas", tmp);
            check("file import: includes and the cycle",
                  intvsrcmap_import(e, one, NULL, &r) == 3);
            check("names are relative to the import root",
                  find_file(e, "lib/util.bas") >= 0);
        }

        /* a failed import leaves the old map alone */
        check("missing path", intvsrcmap_import(e, "/no/such/place", NULL, NULL) == -1);
        check("still loaded", intvsrcmap_file_count(e) == 3);
        check("a sub-folder alone", intvsrcmap_import(e, deep, NULL, NULL) == 1 &&
                                    find_file(e, "d.bas") >= 0);
        rm(deep, "d.bas");
        check("a folder with no .bas", intvsrcmap_import(e, deep, NULL, NULL) == -1);
        check("the previous map survived", intvsrcmap_file_count(e) == 1 &&
                                           find_file(e, "d.bas") >= 0);

        intvsrcmap_destroy(e);
        rm(tmp, "main.bas"); rm(tmp, "notes.txt"); rm(sub, "util.bas");
        rmdir(deep); rmdir(sub); rmdir(tmp);
    }

    intvsrcmap_destroy(m);
    intvsymtab_destroy(sym);

    if (failed) {
        fprintf(stderr, "srcmap_test: FAILED\n");
        return 1;
    }
    printf("srcmap_test: OK\n");
    return 0;
}
