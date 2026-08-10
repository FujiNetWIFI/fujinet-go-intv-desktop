/*
 * session_test -- exercises intvsession's public API: settings persistence,
 * path layout, lifecycle, frame copy and pad injection, through the wrapper
 * rather than intv_host directly (pad_test and boot_smoke already cover the
 * lower layer).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intvsession.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "session_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    char config_dir[] = "/tmp/intv-session-test-cfg-XXXXXX";
    char data_dir[]   = "/tmp/intv-session-test-data-XXXXXX";
    if (!mkdtemp(config_dir) || !mkdtemp(data_dir)) {
        perror("mkdtemp");
        return 1;
    }

    intvsession_paths paths = {
        .config_dir = config_dir,
        .data_dir = data_dir,
    };
    intvsession *s = intvsession_new(&paths);
    check("intvsession_new", s != NULL);
    if (!s)
        return 1;

    /* ---- settings ---- */
    check("default int", intvsession_get_int(s, "nope", 42) == 42);
    intvsession_set_int(s, "volume", 7);
    check("get after set", intvsession_get_int(s, "volume", -1) == 7);
    intvsession_set_str(s, "greeting", "hello");
    check("string round-trip",
          strcmp(intvsession_get_str(s, "greeting", ""), "hello") == 0);
    intvsession_settings_flush(s);

    char settings_path[512];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.ini",
             config_dir);
    FILE *f = fopen(settings_path, "r");
    check("settings.ini exists after flush", f != NULL);
    if (f) fclose(f);

    /* A second session pointed at the same config dir should see the
     * persisted setting. */
    intvsession_free(s);
    s = intvsession_new(&paths);
    check("re-open", s != NULL);
    if (s)
        check("setting persisted", intvsession_get_int(s, "volume", -1) == 7);

    /* ---- paths ---- */
    check("roms_path under data_dir",
          strncmp(intvsession_roms_path(s), data_dir, strlen(data_dir)) == 0);
    check("config_path matches", strcmp(intvsession_config_path(s),
                                        config_dir) == 0);
    check("data_path matches", strcmp(intvsession_data_path(s),
                                      data_dir) == 0);

    /* ---- lifecycle + frame + pad injection, only if ROMs are available -- */
    if (!intvsession_has_system_roms(s)) {
        fprintf(stderr, "session_test: no system ROMs in %s -- lifecycle "
                        "checks SKIPPED (build -DWITH_INTV_ROMS=ON to run "
                        "them)\n", intvsession_roms_path(s));
    } else {
        check("start", intvsession_start(s) == 0);
        check("is_running", intvsession_is_running(s));

        uint32_t pixels[INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT];
        uint64_t serial = 0;
        int got = 0;
        for (int i = 0; i < 300 && !got; i++) {
            if (intvsession_copy_frame(s, pixels, &serial))
                got = 1;
            usleep(10000);
        }
        check("frame published", got);

        intvsession_pad_key(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1, 1);
        intvsession_pad_key(s, INTVSESSION_PAD_LEFT, INTVSESSION_KEY_1, 0);
        intvsession_pad_disc(s, INTVSESSION_PAD_RIGHT, 4);
        intvsession_pad_disc(s, INTVSESSION_PAD_RIGHT, -1);

        intvsession_stop(s);
        check("stopped", !intvsession_is_running(s));
    }

    intvsession_free(s);

    if (failed) {
        fprintf(stderr, "session_test: FAILED\n");
        return 1;
    }
    printf("session_test: OK\n");
    return 0;
}
