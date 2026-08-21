/*
 * reset_test -- starts intv_host with a fresh ROM directory, waits for it
 * to boot (same recipe as boot_smoke), then exercises intv_host_reset():
 * a held pad key doesn't wedge it, the emulator thread survives the
 * one-shot RESET (intv.do_reset = 2, see intv_host.c's own comment) rather
 * than exiting or hanging, and frames keep flowing afterward -- proof
 * periph_reset() actually ran and the main loop kept going. Also checks
 * that a reset with nothing running is a safe no-op, same convention
 * intv_host_stop() already has.
 *
 * Needs exec.bin/grom.bin available to provision, either embedded
 * (-DWITH_INTV_ROMS=ON) or already present in the ROM dir; skipped (exit 77)
 * otherwise, same convention boot_smoke uses.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv_frame.h"
#include "intv_host.h"
#include "roms_embedded.h"
#include "test_tmpdir.h"

/* Polls intv_frame_copy until a NEW serial arrives (proving the emulator
 * thread is actively publishing again) or max_ms elapses. Returns 1 on a
 * fresh frame, 0 on timeout. */
static int wait_for_new_frame(uint64_t *serial, int max_ms)
{
    uint32_t pixels[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
    int waited;

    for (waited = 0; waited < max_ms; waited += 10)
    {
        if (intv_frame_copy(pixels, serial))
            return 1;
        usleep(10000); /* 10ms */
    }
    return 0;
}

int main(void)
{
    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "reset_test: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    /* A reset with nothing running must be a harmless no-op -- same
     * contract intv_host_stop() already documents. */
    intv_host_reset();

    char rom_dir[1024];
    test_tmp_template(rom_dir, sizeof(rom_dir), "intv-reset-test-");
    if (!mkdtemp(rom_dir))
    {
        perror("mkdtemp");
        return 1;
    }

    intv_host_opts opts = {
        .rom_dir = rom_dir,
        .fujinet_host = "127.0.0.1",
        .fujinet_port = 65503,
    };

    if (intv_host_start(&opts) != 0)
    {
        fprintf(stderr, "reset_test: intv_host_start failed\n");
        return 1;
    }

    /* Wait for the machine to actually boot before resetting it -- resetting
     * mid-boot would prove nothing about periph_reset() specifically. */
    uint64_t serial = 0;
    if (!wait_for_new_frame(&serial, 3000))
    {
        fprintf(stderr, "reset_test: no frame published before reset\n");
        intv_host_stop();
        return 1;
    }

    /* Hold a key across the reset -- intv_host_reset()'s pad_reset_inputs
     * call is what stops this from staying latched down in the machine
     * afterward. No public way to inspect pad_t directly from outside
     * intv_host (it's cfg_t intv, private to the staged jzIntv tree), so
     * this only exercises the code path rather than asserting on it -- the
     * assertions below (thread survives, frames resume) are the actual
     * pass/fail signal. */
    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_5, 1);

    intv_host_reset();

    if (!intv_host_is_running())
    {
        fprintf(stderr,
                "reset_test: emulator thread exited across the reset\n");
        return 1;
    }

    /* Frames should keep flowing after the reset -- proof the one-shot
     * RESET didn't hang jzintv.c's main loop (see intv_host_reset's own
     * comment: do_reset == 2 clears itself, periph_reset() runs exactly
     * once, one iteration later). */
    if (!wait_for_new_frame(&serial, 3000))
    {
        fprintf(stderr, "reset_test: no frame published after reset\n");
        intv_host_stop();
        return 1;
    }

    intv_host_pad_key(INTV_PAD_LEFT, INTV_PAD_KEY_5, 0);

    const int running = intv_host_is_running();
    intv_host_stop();

    if (!running)
    {
        fprintf(stderr, "reset_test: emulator thread exited early\n");
        return 1;
    }

    printf("reset_test: OK (survived reset, frames resumed)\n");
    return 0;
}
