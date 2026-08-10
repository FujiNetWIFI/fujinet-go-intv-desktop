/*
 * symtab_test -- pure function test, no emulator boot needed: built-in
 * entries, add/lookup both directions, loading a real as1600-format .sym
 * file (including its undefined-symbol and out-of-range skip rules), tie-
 * breaking (newest wins), and clear_user leaving the built-ins alone.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symbols.h"

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

    /* ---- built-ins (see symbols.c's header for why these two exactly) */
    char name[64];
    check("builtin EXEC_ENTRY",
          intvsymtab_lookup_addr(t, 0x1000, name, sizeof(name)) == 1 &&
          strcmp(name, "EXEC_ENTRY") == 0);
    check("builtin FUJINET_MAILBOX",
          intvsymtab_lookup_addr(t, 0x9C00, name, sizeof(name)) == 1 &&
          strcmp(name, "FUJINET_MAILBOX") == 0);
    check("no symbol at an untouched address",
          intvsymtab_lookup_addr(t, 0x2222, name, sizeof(name)) == 0);

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
    char path[] = "/tmp/intv-symtab-test-XXXXXX";
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

    /* ---- clear_user drops everything but the built-ins ---- */
    intvsymtab_clear_user(t);
    check("user symbol gone after clear",
          intvsymtab_lookup_name(t, "GAME_START", &addr) == 0);
    check("built-in survives clear",
          intvsymtab_lookup_addr(t, 0x1000, name, sizeof(name)) == 1 &&
          strcmp(name, "EXEC_ENTRY") == 0);

    intvsymtab_destroy(t);

    if (failed) {
        fprintf(stderr, "symtab_test: FAILED\n");
        return 1;
    }
    printf("symtab_test: OK\n");
    return 0;
}
