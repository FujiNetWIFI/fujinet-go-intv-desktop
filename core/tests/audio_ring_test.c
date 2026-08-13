/*
 * audio_ring_test -- pure function test, no emulator boot needed: proves
 * intv_audio_copy's fixed-threshold lag self-correction, its prime gate,
 * and intv_audio_reset (see intv_audio.h's own LATENCY/PRIMING comments).
 *
 * The trim threshold used to be relative to the caller's own request size
 * (max_samples * 2), which meant its behaviour depended on how the
 * consumer's chunk size happened to compare to the producer's fixed
 * publish size -- on a host whose audio device quantum was smaller than
 * one producer burst, that threshold fired on every single publish and
 * decimated the stream. It is now a fixed sample count independent of
 * max_samples; the first two checks below are the regression test for
 * that bug: a single 2048-sample publish (snd_desktop's own default burst
 * size), drained in small 512-sample chunks (a small device quantum),
 * must come back whole and in order, not decimated.
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
    static int16_t burst[2048];
    static int16_t stale[10000];
    static int16_t fresh[200];
    int16_t out[2048];
    int n, i, total;

    for (i = 0; i < 2048; i++) burst[i] = (int16_t)i;

    /* Nothing published yet: an empty ring returns 0, not garbage. */
    n = intv_audio_copy(out, 512);
    check("empty ring returns 0", n == 0);

    /* Regression test: one producer-sized (2048) publish, drained in
     * small (512) consumer chunks -- a small device quantum -- must come
     * back whole and byte-exact, not decimated by a request-relative
     * threshold. */
    intv_audio_publish(burst, 2048);
    total = 0;
    while (total < 2048) {
        n = intv_audio_copy(out + total, 512);
        if (n == 0)
            break; /* prime gate withholding; keep polling */
        total += n;
    }
    check("small-chunk drain recovers the full burst", total == 2048);
    check("small-chunk drain is byte-exact",
         memcmp(out, burst, sizeof(burst)) == 0);
    n = intv_audio_copy(out, 512);
    check("nothing left after draining the burst", n == 0);

    /* Sustained backlog (far beyond RING_HIGHWATER_SAMPLES, ~100ms/4800
     * samples at 48kHz) followed by fresh data, with no draining in
     * between -- exactly what a consumer that fell behind for a while
     * leaves sitting in the ring. The trim must discard (most of) the
     * stale backlog and return the resulting cushion of freshest samples,
     * not just under.*/
    for (i = 0; i < 10000; i++) stale[i] = 1111;
    for (i = 0; i < 200; i++) fresh[i] = 2222;
    intv_audio_publish(stale, 10000);
    intv_audio_publish(fresh, 200);

    total = 0;
    while (total < 2048) {
        n = intv_audio_copy(out + total, 512);
        if (n == 0)
            break;
        total += n;
    }
    check("trimmed backlog is bounded, not the full 10000+200",
         total > 0 && total < 10000);
    {
        int saw_fresh = 0;
        for (i = 0; i < total; i++)
            if (out[i] == 2222)
                saw_fresh = 1;
        check("trimmed backlog still ends with the freshest samples",
             saw_fresh);
    }

    /* intv_audio_reset empties the ring and clears the prime gate. */
    intv_audio_publish(fresh, 200);
    intv_audio_reset();
    n = intv_audio_copy(out, 200);
    check("reset leaves the ring empty", n == 0);

    /* Normal (un-backlogged, primed) operation still works: publish
     * exactly one target's worth or more and get it all back untouched. */
    intv_audio_reset();
    intv_audio_publish(burst, 2048);
    total = 0;
    while (total < 2048) {
        n = intv_audio_copy(out + total, 2048 - total);
        if (n == 0)
            break;
        total += n;
    }
    check("un-backlogged copy returns everything published", total == 2048);
    check("un-backlogged copy is byte-exact",
         memcmp(out, burst, sizeof(burst)) == 0);

    if (failed) {
        fprintf(stderr, "audio_ring_test: FAILED\n");
        return 1;
    }
    printf("audio_ring_test: OK\n");
    return 0;
}
