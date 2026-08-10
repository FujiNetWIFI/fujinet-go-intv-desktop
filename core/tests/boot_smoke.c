/*
 * boot_smoke -- starts intv_host with a fresh ROM directory and asserts a
 * non-blank frame appears, i.e. the emulator actually boots (the embedded
 * FujiNet config ROM, since no cartridge is given -- see intv_host.h) rather
 * than hanging or crashing.
 *
 * Needs exec.bin/grom.bin available to provision, either embedded
 * (-DWITH_INTV_ROMS=ON) or already present in the ROM dir; skipped (exit 77)
 * otherwise, same convention fujinet-go-coco-desktop's boot_smoke uses.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv_frame.h"
#include "intv_host.h"
#include "roms_embedded.h"

int main(void)
{
    if (intv_embedded_rom_count == 0)
    {
        fprintf(stderr, "boot_smoke: no embedded ROMs "
                        "(build -DWITH_INTV_ROMS=ON) -- SKIP\n");
        return 77;
    }

    char rom_dir[] = "/tmp/intv-boot-smoke-XXXXXX";
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
        fprintf(stderr, "boot_smoke: intv_host_start failed\n");
        return 1;
    }

    uint32_t pixels[INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT];
    uint64_t serial = 0;
    int got_frame = 0;
    int non_blank = 0;

    /* The STIC renders at ~60Hz; a few seconds is generous headroom on a
     * loaded CI machine without making a hung emulator take forever to
     * report as a failure. */
    for (int i = 0; i < 300 && !non_blank; i++)
    {
        if (intv_frame_copy(pixels, &serial))
        {
            got_frame = 1;
            for (int p = 0; p < INTV_FRAME_WIDTH * INTV_FRAME_HEIGHT; p++)
            {
                if (pixels[p] != pixels[0])
                {
                    non_blank = 1;
                    break;
                }
            }
        }
        usleep(10000); /* 10ms */
    }

    const int running = intv_host_is_running();
    intv_host_stop();

    if (!running)
    {
        fprintf(stderr, "boot_smoke: emulator thread exited early\n");
        return 1;
    }
    if (!got_frame)
    {
        fprintf(stderr, "boot_smoke: no frame published\n");
        return 1;
    }
    if (!non_blank)
    {
        fprintf(stderr, "boot_smoke: frame published but flat/blank\n");
        return 1;
    }

    printf("boot_smoke: OK (booted to a non-blank frame)\n");
    return 0;
}
