/*
 * symtab_test -- pure function test, no emulator boot needed: built-in
 * entries, add/lookup both directions, loading a real as1600-format .sym
 * file (including its undefined-symbol and out-of-range skip rules), tie-
 * breaking (newest wins), clear_user leaving the built-ins alone, the
 * region map, nearest-symbol lookup, and disassembly-line annotation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symbols.h"
#include "test_tmpdir.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "symtab_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    intvsymtab *t = intvsymtab_create();
    check("create", t != NULL);

    /* ---- built-ins (spot-check; see symbols.c's header for provenance) */
    char name[64];
    check("builtin EXEC_ENTRY",
          intvsymtab_lookup_addr(t, 0x1000, name, sizeof(name)) == 1 &&
          strcmp(name, "EXEC_ENTRY") == 0);
    check("builtin FUJINET_MAILBOX",
          intvsymtab_lookup_addr(t, 0x9C00, name, sizeof(name)) == 1 &&
          strcmp(name, "FUJINET_MAILBOX") == 0);
    check("builtin X_MAIN_LOOP",
          intvsymtab_lookup_addr(t, 0x108F, name, sizeof(name)) == 1 &&
          strcmp(name, "X_MAIN_LOOP") == 0);
    check("builtin X_RAND2",
          intvsymtab_lookup_addr(t, 0x169E, name, sizeof(name)) == 1 &&
          strcmp(name, "X_RAND2") == 0);
    check("builtin EXEC_HTBL",
          intvsymtab_lookup_addr(t, 0x035D, name, sizeof(name)) == 1 &&
          strcmp(name, "EXEC_HTBL") == 0);
    check("builtin CART_DISP_MODE",
          intvsymtab_lookup_addr(t, 0x500E, name, sizeof(name)) == 1 &&
          strcmp(name, "CART_DISP_MODE") == 0);
    check("no symbol at an untouched address",
          intvsymtab_lookup_addr(t, 0x2222, name, sizeof(name)) == 0);

    /* ---- region map ---- */
    check("region STIC", intvsym_region(0x0000) != NULL &&
          strcmp(intvsym_region(0x0000), "STIC") == 0);
    check("region STIC upper bound", intvsym_region(0x003F) != NULL &&
          strcmp(intvsym_region(0x003F), "STIC") == 0);
    check("no region just past STIC", intvsym_region(0x0040) == NULL);
    check("region PSG lower bound", intvsym_region(0x01F0) != NULL &&
          strcmp(intvsym_region(0x01F0), "PSG") == 0);
    check("region BACKTAB just after PSG", intvsym_region(0x0200) != NULL &&
          strcmp(intvsym_region(0x0200), "BACKTAB") == 0);
    check("region EXEC object table lower bound",
          intvsym_region(0x031D) != NULL &&
          strcmp(intvsym_region(0x031D), "EXEC object table") == 0);
    check("region EXEC globals right after object table",
          intvsym_region(0x035D) != NULL &&
          strcmp(intvsym_region(0x035D), "EXEC globals") == 0);
    check("region STIC alias at $8000", intvsym_region(0x8000) != NULL &&
          strcmp(intvsym_region(0x8000), "STIC alias") == 0);
    check("no region in the gap above STIC alias",
          intvsym_region(0x8001) == NULL);

    /* ---- nearest-symbol lookup ---- */
    int delta = -1;
    check("nearest exact match",
          intvsymtab_lookup_nearest(t, 0x169E, 64, name, sizeof(name),
                                    &delta) == 1 &&
          strcmp(name, "X_RAND2") == 0 && delta == 0);
    check("nearest within window",
          intvsymtab_lookup_nearest(t, 0x169E + 3, 64, name, sizeof(name),
                                    &delta) == 1 &&
          strcmp(name, "X_RAND2") == 0 && delta == 3);
    check("nearest outside window misses",
          intvsymtab_lookup_nearest(t, 0x169E + 100, 64, name, sizeof(name),
                                    &delta) == 0);
    check("nearest finds the closest symbol at or below addr",
          intvsymtab_lookup_nearest(t, 0x169D, 64, name, sizeof(name),
                                    &delta) == 1 &&
          strcmp(name, "X_RAND1") == 0 && delta == (0x169D - 0x167D));

    /* ---- disassembly-line annotation ---- */
    {
        intvdebug_dasm_line line;
        char annot[96];

        memset(&line, 0, sizeof(line));
        line.addr = 0x109A; /* inside X_MAIN_LOOP, no exact symbol itself */
        snprintf(line.text, sizeof(line.text), "JSR     R5,$169E");
        check("annotate operand resolves to X_RAND2",
              intvsym_annotate_line(t, &line, annot, sizeof(annot)) > 0 &&
              strstr(annot, "X_RAND2") != NULL);

        line.addr = 0x108F; /* exact hit on the line's own address */
        snprintf(line.text, sizeof(line.text), "EIS");
        check("annotate own address resolves to X_MAIN_LOOP",
              intvsym_annotate_line(t, &line, annot, sizeof(annot)) > 0 &&
              strstr(annot, "X_MAIN_LOOP") != NULL);

        line.addr = 0x5100; /* no symbol here or nearby -- unlabeled */
        snprintf(line.text, sizeof(line.text), "MVO     R1,$0201");
        check("annotate operand falls back to region",
              intvsym_annotate_line(t, &line, annot, sizeof(annot)) > 0 &&
              strstr(annot, "BACKTAB") != NULL);

        line.addr = 0x5100;
        snprintf(line.text, sizeof(line.text), "MVI     R0,#5");
        check("annotate with nothing to say writes empty string",
              intvsym_annotate_line(t, &line, annot, sizeof(annot)) == 0 &&
              annot[0] == 0);
    }

    /* ---- direct add + both lookup directions ---- */
    check("add", intvsymtab_add(t, 0x5000, "MY_ROUTINE") == 1);
    check("lookup by addr",
          intvsymtab_lookup_addr(t, 0x5000, name, sizeof(name)) == 1 &&
          strcmp(name, "MY_ROUTINE") == 0);
    uint16_t addr = 0;
    check("lookup by name",
          intvsymtab_lookup_name(t, "MY_ROUTINE", &addr) == 1 &&
          addr == 0x5000);
    check("unknown name misses",
          intvsymtab_lookup_name(t, "NOPE", &addr) == 0);

    /* ---- newest wins on a tie ---- */
    check("shadow same address", intvsymtab_add(t, 0x5000, "NEWER_NAME") == 1);
    check("newest shadows oldest",
          intvsymtab_lookup_addr(t, 0x5000, name, sizeof(name)) == 1 &&
          strcmp(name, "NEWER_NAME") == 0);

    /* ---- load a real as1600 -s style file ----
     * Format confirmed against jzIntv's own loader (src/debug/debug.c):
     * "hexaddr name" per line, undefined symbols marked with a leading '?'
     * and skipped, decimal-looking addresses above $FFFF skipped too. */
    char path[1024];
    test_tmp_template(path, sizeof(path), "intv-symtab-test-");
    int fd = mkstemp(path);
    check("mkstemp", fd >= 0);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        fprintf(f, "???????? DOIDLEHST\n");
        fprintf(f, "???????? IDLEHST\n");
        fprintf(f, "0000a000 _GFX\n");
        fprintf(f, "0000a000 TOPMOUNT\n");
        fprintf(f, "00001014 GAME_START\n");
        fprintf(f, "00020000 OUT_OF_RANGE\n"); /* > $FFFF: must be skipped */
        fclose(f);

        int n = intvsymtab_load(t, path);
        check("load returns 3 real entries (2 undefined + 1 out-of-range "
              "skipped)", n == 3);
        check("loaded _GFX",
              intvsymtab_lookup_addr(t, 0xa000, name, sizeof(name)) == 1);
        /* Both _GFX and TOPMOUNT share $a000; newest (TOPMOUNT, added
         * after _GFX in the file) should win the lookup. */
        check("newest of the two $a000 entries wins",
              strcmp(name, "TOPMOUNT") == 0);
        check("loaded GAME_START",
              intvsymtab_lookup_name(t, "GAME_START", &addr) == 1 &&
              addr == 0x1014);
        check("out-of-range address never got added",
              intvsymtab_lookup_name(t, "OUT_OF_RANGE", &addr) == 0);

        remove(path);
    }

    check("missing file returns -1", intvsymtab_load(t, "/nonexistent/x") == -1);

    /* ---- sort index: binary-search results must match the pre-index
     * linear scan, both right after a load (index clean) and after an
     * intvsymtab_add that leaves it dirty. intvsymtab_load already ran
     * above, so the index is clean and lookup_addr/_nearest are exercising
     * the binary-search path for every check from here up to this point --
     * this section targets it directly, plus the tie behaviour again since
     * that's the part a sort is most likely to get wrong. */
    check("indexed lookup: _GFX/TOPMOUNT tie still newest-wins",
          intvsymtab_lookup_addr(t, 0xa000, name, sizeof(name)) == 1 &&
          strcmp(name, "TOPMOUNT") == 0);
    check("indexed lookup: exact GAME_START",
          intvsymtab_lookup_addr(t, 0x1014, name, sizeof(name)) == 1 &&
          strcmp(name, "GAME_START") == 0);
    check("indexed lookup: miss well away from anything",
          intvsymtab_lookup_addr(t, 0x2222, name, sizeof(name)) == 0);
    {
        int delta = -1;
        check("indexed nearest: a few words past GAME_START",
              intvsymtab_lookup_nearest(t, 0x101A, 64, name, sizeof(name),
                                        &delta) == 1 &&
              strcmp(name, "GAME_START") == 0 && delta == 0x101A - 0x1014);
        check("indexed nearest: exact hit reports delta 0",
              intvsymtab_lookup_nearest(t, 0xa000, 64, name, sizeof(name),
                                        &delta) == 1 &&
              strcmp(name, "TOPMOUNT") == 0 && delta == 0);
        check("indexed nearest: outside window misses",
              intvsymtab_lookup_nearest(t, 0x1014 + 65, 64, name,
                                        sizeof(name), &delta) == 0);
    }

    /* intvsymtab_add alone (no load in between) must dirty the index and
     * still be found -- exercises the "index present but stale, fall back
     * to the linear scan" path, not just "no index yet". */
    check("add after load still findable (index now stale)",
          intvsymtab_add(t, 0x2000, "POST_LOAD_SYM") == 1 &&
          intvsymtab_lookup_addr(t, 0x2000, name, sizeof(name)) == 1 &&
          strcmp(name, "POST_LOAD_SYM") == 0);

    /* ---- enumeration: address-ascending, oldest-first on ties (the
     * opposite tiebreak from lookup_addr, since a browser should show every
     * shadowed name) ---- */
    {
        int n = intvsymtab_count(t);
        uint16_t eaddr;
        char ename[64], enote[64];
        int i, saw_gfx = -1, saw_topmount = -1;
        uint16_t prev = 0;
        int have_prev = 0;

        check("count matches what was loaded", n > 0);
        for (i = 0; i < n; i++) {
            check("enum in range succeeds",
                  intvsymtab_enum(t, i, &eaddr, ename, sizeof(ename), enote,
                                  sizeof(enote)) == 1);
            if (have_prev)
                check("enum is address-ascending", eaddr >= prev);
            prev = eaddr;
            have_prev = 1;
            if (eaddr == 0xa000 && strcmp(ename, "_GFX") == 0)
                saw_gfx = i;
            if (eaddr == 0xa000 && strcmp(ename, "TOPMOUNT") == 0)
                saw_topmount = i;
        }
        check("enum sees both tied $a000 entries",
              saw_gfx >= 0 && saw_topmount >= 0);
        check("enum lists the tie oldest-first (_GFX before TOPMOUNT)",
              saw_gfx < saw_topmount);
        check("enum out of range low fails",
              intvsymtab_enum(t, -1, &eaddr, ename, sizeof(ename), enote,
                              sizeof(enote)) == 0);
        check("enum out of range high fails",
              intvsymtab_enum(t, n, &eaddr, ename, sizeof(ename), enote,
                              sizeof(enote)) == 0);
    }

    /* ---- generation: bumped by add/load/clear_user, stable otherwise ---- */
    {
        unsigned g0 = intvsymtab_generation(t);
        check("generation bumps on add",
              intvsymtab_add(t, 0x3000, "GEN_BUMP") == 1 &&
              intvsymtab_generation(t) != g0);
    }

    /* ---- intvsym_parse_addr: name-or-hex resolution ---- */
    {
        uint16_t a = 0;
        check("parse: known symbol name",
              intvsym_parse_addr(t, "GAME_START", &a) == 1 && a == 0x1014);
        check("parse: unknown name is not silently hex",
              intvsym_parse_addr(t, "NOPE_NOT_A_SYMBOL", &a) == 0);
        check("parse: bare hex",
              intvsym_parse_addr(t, "1000", &a) == 1 && a == 0x1000);
        check("parse: $-prefixed hex",
              intvsym_parse_addr(t, "$1000", &a) == 1 && a == 0x1000);
        check("parse: 0x-prefixed hex",
              intvsym_parse_addr(t, "0x1000", &a) == 1 && a == 0x1000);
        check("parse: 0X-prefixed hex",
              intvsym_parse_addr(t, "0X1000", &a) == 1 && a == 0x1000);
        check("parse: surrounding whitespace ignored",
              intvsym_parse_addr(t, "  1000  ", &a) == 1 && a == 0x1000);
        check("parse: trailing garbage rejected",
              intvsym_parse_addr(t, "1000xyz", &a) == 0);
        check("parse: above $FFFF rejected",
              intvsym_parse_addr(t, "10000", &a) == 0);
        check("parse: empty text rejected",
              intvsym_parse_addr(t, "", &a) == 0);
        check("parse: NULL table falls back to hex-only",
              intvsym_parse_addr(NULL, "$1014", &a) == 1 && a == 0x1014);
    }

    /* ---- intvsym_dirname ---- */
    {
        char d[256];
        check("dirname: unix path",
              intvsym_dirname("/a/b/c.sym", d, sizeof(d)) > 0 &&
              strcmp(d, "/a/b") == 0);
        check("dirname: windows path",
              intvsym_dirname("a\\b\\c.sym", d, sizeof(d)) > 0 &&
              strcmp(d, "a\\b") == 0);
        check("dirname: no separator yields empty",
              intvsym_dirname("c.sym", d, sizeof(d)) == 0 && d[0] == 0);
        check("dirname: mixed separators uses the rightmost",
              intvsym_dirname("/a\\b/c.sym", d, sizeof(d)) > 0 &&
              strcmp(d, "/a\\b") == 0);
    }

    /* ---- clear_user drops everything but the built-ins ---- */
    intvsymtab_clear_user(t);
    check("user symbol gone after clear",
          intvsymtab_lookup_name(t, "GAME_START", &addr) == 0);
    check("built-in survives clear",
          intvsymtab_lookup_addr(t, 0x1000, name, sizeof(name)) == 1 &&
          strcmp(name, "EXEC_ENTRY") == 0);

    intvsymtab_destroy(t);

    /* ---- intvsymtab_shared: process-wide singleton, same contract as
     * intvdebug_get() -- two calls return the same table, and a symbol
     * added through one handle is visible through the other. */
    {
        intvsymtab *s1 = intvsymtab_shared();
        intvsymtab *s2 = intvsymtab_shared();
        uint16_t a = 0;
        check("shared table is a singleton", s1 != NULL && s1 == s2);
        check("shared table starts with built-ins",
              intvsymtab_lookup_addr(s1, 0x1000, name, sizeof(name)) == 1);
        check("symbol added via one handle visible via the other",
              intvsymtab_add(s1, 0x7000, "SHARED_SYM") == 1 &&
              intvsymtab_lookup_name(s2, "SHARED_SYM", &a) == 1 &&
              a == 0x7000);
    }

    if (failed) {
        fprintf(stderr, "symtab_test: FAILED\n");
        return 1;
    }
    printf("symtab_test: OK\n");
    return 0;
}
