/*
 * no_sdl_link_test -- proves libjzintv_core links standalone, with none of
 * jzIntv's SDL-backed gfx/snd/event/plat/joy sources pulled in.
 *
 * Doesn't call jzintv_entry_point() (that needs ROM images and would spin up
 * the full simulator loop); taking its address is enough to force the linker
 * to resolve every symbol the null-backend jzintv_core exports, which is what
 * this test is actually checking.
 */
#include <stdio.h>

extern int jzintv_entry_point(int argc, char *argv[]);

int main(void)
{
    void *volatile entry = (void *)&jzintv_entry_point;
    if (!entry)
    {
        fprintf(stderr, "no_sdl_link_test: jzintv_entry_point missing\n");
        return 1;
    }
    printf("no_sdl_link_test: OK\n");
    return 0;
}
