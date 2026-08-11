/*
 * audio_ring_test -- pure function test, no emulator boot needed: proves
 * intv_audio_copy's lag self-correction (see intv_audio.h's own comment).
 * Publishes a large "stale" batch, then a small "fresh" batch, without
 * draining in between -- a stand-in for a consumer (an SDL audio callback)
 * that fell behind for a while. A copy small enough that both batches
 * together exceed its own 2x threshold must return the FRESH data, not
 * play through the stale backlog first.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "intv_audio.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "audio_ring_test: FAILED: %s\n", what);
        failed = 1;
    }
}

int main(void)
{
    static int16_t stale[4000];
    static int16_t fresh[200];
    int16_t out[200];
    int n, i;

    /* A distinctive, easy-to-recognise value for each batch. */
    for (i = 0; i < 4000; i++) stale[i] = 1111;
    for (i = 0; i < 200; i++) fresh[i] = 2222;

    /* Nothing published yet: an empty ring returns 0, not garbage. */
    n = intv_audio_copy(out, 200);
    check("empty ring returns 0", n == 0);

    /* Publish a large stale batch (far more than the 2x-of-a-200-sample-
     * request threshold intv_audio_copy enforces), then a small fresh
     * batch, with no draining in between -- exactly what a briefly-behind
     * consumer leaves sitting in the ring. */
    intv_audio_publish(stale, 4000);
    intv_audio_publish(fresh, 200);

    n = intv_audio_copy(out, 200);
    check("returns a full request's worth", n == 200);
    {
        int all_fresh = 1;
        for (i = 0; i < n; i++)
            if (out[i] != 2222)
                all_fresh = 0;
        check("catches up over the stale backlog instead of playing it",
             all_fresh);
    }

    /* Draining again immediately should find nothing left -- the stale
     * backlog was discarded, not merely reordered behind the fresh data. */
    n = intv_audio_copy(out, 200);
    check("nothing left after catching up", n == 0);

    /* Normal (non-backlogged) operation still works: publish exactly what
     * is asked for and get it all back untouched. */
    intv_audio_publish(fresh, 200);
    n = intv_audio_copy(out, 200);
    check("un-backlogged copy returns everything published", n == 200);
    check("un-backlogged copy is byte-exact",
         memcmp(out, fresh, sizeof(fresh)) == 0);

    if (failed) {
        fprintf(stderr, "audio_ring_test: FAILED\n");
        return 1;
    }
    printf("audio_ring_test: OK\n");
    return 0;
}
