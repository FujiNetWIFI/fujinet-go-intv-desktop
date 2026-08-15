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
#include "test_tmpdir.h"

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
    char config_dir[1024], data_dir[1024];
    test_tmp_template(config_dir, sizeof(config_dir), "intv-session-test-cfg-");
    test_tmp_template(data_dir, sizeof(data_dir), "intv-session-test-data-");
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

    /* ---- machine options ---- */
    check("hw_mode_name(0) is Auto",
          strcmp(intvsession_hw_mode_name(0), "Auto") == 0);
    check("hw_mode_name table terminates",
          intvsession_hw_mode_name(3) == NULL);
    check("video_name(0) is NTSC",
          strstr(intvsession_video_name(0), "NTSC") != NULL);
    check("video_name table terminates",
          intvsession_video_name(2) == NULL);

    intvsession_start_opts opts;
    intvsession_default_opts(s, &opts);
    check("default ecs is Auto", opts.ecs == INTVSESSION_HW_AUTO);
    check("default ivoice is Auto", opts.ivoice == INTVSESSION_HW_AUTO);
    check("default video is NTSC", opts.video == INTVSESSION_VIDEO_NTSC);

    intvsession_set_int(s, "ecs", INTVSESSION_HW_ON);
    intvsession_set_int(s, "ivoice", INTVSESSION_HW_OFF);
    intvsession_set_int(s, "video_standard", INTVSESSION_VIDEO_PAL);
    intvsession_set_int(s, "keyboard_mode", 1);
    intvsession_default_opts(s, &opts);
    check("default_opts reflects ecs", opts.ecs == INTVSESSION_HW_ON);
    check("default_opts reflects ivoice", opts.ivoice == INTVSESSION_HW_OFF);
    check("default_opts reflects video", opts.video == INTVSESSION_VIDEO_PAL);
    check("keyboard_mode round-trips",
          intvsession_get_int(s, "keyboard_mode", 0) == 1);

    /* Restore Auto/Auto/NTSC/controllers for the lifecycle checks below --
     * a from-scratch session is what a real first run would see, and ECS
     * needing ecs.bin is exercised on its own below rather than here. */
    intvsession_set_int(s, "ecs", INTVSESSION_HW_AUTO);
    intvsession_set_int(s, "ivoice", INTVSESSION_HW_AUTO);
    intvsession_set_int(s, "video_standard", INTVSESSION_VIDEO_NTSC);
    intvsession_set_int(s, "keyboard_mode", 0);

    /* ---- reset to config ---- */
    intvsession_set_str(s, "cart", "/nonexistent/whatever.rom");
    check("cart_path reflects set",
          strcmp(intvsession_cart_path(s), "/nonexistent/whatever.rom") == 0);
    intvsession_default_opts(s, &opts);
    check("default_opts cart_path non-NULL when cart set",
          opts.cart_path != NULL);

    /* reset_to_config clears the persisted cart regardless of whether the
     * restart itself can succeed (no embedded ROMs -> intvsession_start
     * refuses, but the settings-clear side effect still happens first). */
    int reset_rc = intvsession_reset_to_config(s);
    check("cart_path cleared after reset_to_config",
          strcmp(intvsession_cart_path(s), "") == 0);
    intvsession_default_opts(s, &opts);
    check("default_opts cart_path NULL after reset_to_config",
          opts.cart_path == NULL);
    if (intvsession_has_system_roms(s)) {
        check("reset_to_config succeeds with system ROMs present",
              reset_rc == 0);
        check("running after reset_to_config", intvsession_is_running(s));
        /* Leave the session stopped for the lifecycle block below, which
         * calls intvsession_start(s, NULL) itself and would otherwise find
         * one already running. */
        intvsession_stop(s);
    }

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
        check("start", intvsession_start(s, NULL) == 0);
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

        /* ECS pair + keyboard through the public API, exercised only if
         * ecs.bin was embedded (WITH_INTV_ROMS=ON puts it in the same
         * embedded-ROM table as exec/grom -- see roms_embedded.h). */
        if (intvsession_has_ecs_rom(s)) {
            intvsession_pad_key(s, INTVSESSION_PAD_ECS_LEFT,
                               INTVSESSION_KEY_1, 1);
            intvsession_pad_key(s, INTVSESSION_PAD_ECS_LEFT,
                               INTVSESSION_KEY_1, 0);
            intvsession_ecs_key_set(s, INTVSESSION_ECS_KEY_A, 1);
            intvsession_ecs_keys_clear(s);
        } else {
            fprintf(stderr, "session_test: no ecs.bin in %s -- ECS checks "
                            "SKIPPED\n", intvsession_roms_path(s));
        }

        intvsession_stop(s);
        check("stopped", !intvsession_is_running(s));

        /* A user-forced ECS=On with no ecs.bin must refuse cleanly rather
         * than reach jzIntv's cfg_init() and exit(1) the process -- see
         * intv_host_start's own comment on why this is checked before the
         * emulator thread ever starts. Only meaningful when ecs.bin isn't
         * already there (a WITH_INTV_ROMS=ON build always has one). */
        if (!intvsession_has_ecs_rom(s)) {
            intvsession_start_opts ecs_opts = {
                .ecs = INTVSESSION_HW_ON,
                .ivoice = INTVSESSION_HW_AUTO,
                .video = INTVSESSION_VIDEO_NTSC,
            };
            check("forced ECS with no ecs.bin refuses",
                  intvsession_start(s, &ecs_opts) != 0);
            check("...and leaves an error message",
                  intvsession_last_error(s) != NULL &&
                      intvsession_last_error(s)[0] != '\0');
            check("...and does not leave the session running",
                  !intvsession_is_running(s));
        }
    }

    intvsession_free(s);

    if (failed) {
        fprintf(stderr, "session_test: FAILED\n");
        return 1;
    }
    printf("session_test: OK\n");
    return 0;
}
