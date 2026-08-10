/*
 * ============================================================================
 *  Title:    Sound Interface Abstraction -- desktop (audio-capturing) backend
 * ============================================================================
 *  Takes the place of jzIntv's own snd_sdl.c in the object list (see
 *  core/CMakeLists.txt's header comment on how backend selection works) --
 *  and of snd_null.c, which this is a close copy of, with two changes:
 *
 *   1. snd_null.c's snd_tick(), when neither dumping to a raw audio file nor
 *      an AVI, sets try_drop = min_num_dirty and dly_drop = 0 -- which means
 *      the mixing loop below (`for (i = try_drop; i < min_num_dirty; i++)`)
 *      never actually runs: every PSG buffer is silently discarded without
 *      ever touching mixbuf. That is exactly right for a backend with no
 *      consumer at all, but wrong for us, so this forces the same path
 *      snd_null.c takes only when raw_file/avi_active is set: dly_drop =
 *      try_drop, try_drop = 0, unconditionally. Mixing always runs.
 *
 *   2. Once a mix buffer is filled, gfx_sdl2.c's counterpart (snd_fill, the
 *      SDL audio callback) is what actually plays it -- applying
 *      snd->atten there, not in the mixer, because that only runs once
 *      per *device* buffer, not once per emulator-generated one. We have no
 *      device callback, so that step happens here instead, synchronously,
 *      right after mixing: apply the same attenuation shift snd_fill does,
 *      then intv_audio_publish() the result. The buffer is then returned to
 *      snd->mixbuf.clean immediately after (same as snd_null.c's own
 *      delayed-drop path) -- there is no need for our own mixbuf.dirty list
 *      the way SDL's separate-thread callback needs one.
 * ============================================================================
 */

#include "config.h"
#include "periph/periph.h"
#include "snd/snd.h"
#include "avi/avi.h"

#include "../intv_audio.h"

LOCAL int32_t *mixbuf = NULL;
LOCAL uint32_t snd_tick(periph_t *const periph, uint32_t len);

typedef struct snd_pvt_t
{
    avi_writer_t    *avi;
} snd_pvt_t;

LOCAL const uint8_t snd_wav_hdr[44] =
{
    0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00,
    0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20,
    0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61,
    0x00, 0x00, 0x00, 0x00
};

LOCAL void snd_update_wav_hdr(FILE *f, int rate)
{
    long     filepos;
    uint32_t tot_smp_bytes, tot_file_bytes;
    uint8_t  upd_wav_hdr[44];

    memcpy(upd_wav_hdr, snd_wav_hdr, 44);

    filepos = ftell(f);
    if (filepos < 0)
        return;

    tot_smp_bytes  = filepos > 44 ? filepos - 44 : 0;
    tot_file_bytes = filepos >  8 ? filepos -  8 : 0;

    upd_wav_hdr[ 4] = (tot_file_bytes >>  0) & 0xFF;
    upd_wav_hdr[ 5] = (tot_file_bytes >>  8) & 0xFF;
    upd_wav_hdr[ 6] = (tot_file_bytes >> 16) & 0xFF;
    upd_wav_hdr[ 7] = (tot_file_bytes >> 24) & 0xFF;

    upd_wav_hdr[40] = (tot_smp_bytes  >>  0) & 0xFF;
    upd_wav_hdr[41] = (tot_smp_bytes  >>  8) & 0xFF;
    upd_wav_hdr[42] = (tot_smp_bytes  >> 16) & 0xFF;
    upd_wav_hdr[43] = (tot_smp_bytes  >> 24) & 0xFF;

    upd_wav_hdr[24] = (rate           >>  0) & 0xFF;
    upd_wav_hdr[25] = (rate           >>  8) & 0xFF;
    upd_wav_hdr[26] = (rate           >> 16) & 0xFF;
    upd_wav_hdr[27] = (rate           >> 24) & 0xFF;
    upd_wav_hdr[28] = (rate * 2       >>  0) & 0xFF;
    upd_wav_hdr[29] = (rate * 2       >>  8) & 0xFF;
    upd_wav_hdr[30] = (rate * 2       >> 16) & 0xFF;
    upd_wav_hdr[31] = (rate * 2       >> 24) & 0xFF;

    if (fseek(f, 0, SEEK_SET) == 0)
    {
        fwrite(upd_wav_hdr, 44, 1, f);
        fseek(f,  0, SEEK_END);
    }
}

/* Applies snd->atten the same way snd_sdl.c's snd_fill does, in place. */
LOCAL void apply_attenuation(snd_t *const snd, int16_t *samples)
{
    if (snd->atten > 0)
    {
        const int a = (snd->atten + 1) >> 1;
        for (int i = 0; i < snd->buf_size; i++)
            samples[i] >>= a;
    }
    if (snd->atten & 1)
        for (int i = 0; i < snd->buf_size; i++)
            samples[i] += samples[i] >> 1;
}

LOCAL uint32_t snd_tick(periph_t *const periph, uint32_t len)
{
    snd_t *const snd = PERIPH_AS(snd_t, periph);
    int min_num_dirty;
    int i, j, k, mix;
    int try_drop = 0, did_drop = 0, dly_drop = 0;
    int16_t *clean;
    uint64_t new_now;
    int not_silent = snd->raw_start;
    const int avi_active = avi_is_active(snd->pvt->avi);

    if (snd->change_vol == 1) snd->atten -= (snd->atten > 0);
    if (snd->change_vol == 2) snd->atten += (snd->atten < 32);
    snd->change_vol = 0;

    if (snd->src_cnt == 0)
        return len;

    if (snd->mixbuf.num_clean == 0)
        return 0;

    min_num_dirty = snd->src[0]->num_dirty;
    for (i = 1; i < snd->src_cnt; i++)
    {
        if (min_num_dirty > snd->src[i]->num_dirty)
            min_num_dirty = snd->src[i]->num_dirty;
    }

    /* Unlike snd_null.c: always take the "delayed drop" path, so mixing
     * always runs and every buffer reaches intv_audio_publish below (see
     * this file's header, change 1). */
    try_drop = min_num_dirty;
    dly_drop = try_drop;
    try_drop = 0;

    did_drop = try_drop;

    assert(try_drop == 0 || dly_drop == 0);
    for (i = try_drop; i < min_num_dirty; i++)
    {
        int clean_idx;
        clean_idx = --snd->mixbuf.num_clean;
        clean = snd->mixbuf.clean[clean_idx];
        snd->mixbuf.clean[clean_idx] = NULL;

        if (snd->src_cnt == 1)
        {
            int16_t *tmp;

            tmp = snd->src[0]->dirty[i];
            snd->src[0]->dirty[i] = clean;
            clean = tmp;

            if (snd->raw_file || !snd->raw_start)
                for (j = 0; j < snd->buf_size && !not_silent; j++)
                    not_silent = clean[j];

            goto one_source;
        }

        memset(mixbuf, 0, snd->buf_size * sizeof(int));
        for (j = 0; j < snd->src_cnt; j++)
        {
            for (k = 0; k < snd->buf_size; k++)
                mixbuf[k] += snd->src[j]->dirty[i][k];
        }

        for (j = 0; j < snd->buf_size; j++)
        {
            mix = mixbuf[j];
            if (mix >  0x7FFF) mix =  0x7FFF;
            if (mix < -0x8000) mix = -0x8000;
            clean[j] = mix;
            not_silent |= mix;
        }

one_source:

        /* Change 2 (see file header): apply attenuation and publish the
         * mixed buffer here, synchronously -- this is our "device". */
        apply_attenuation(snd, clean);
        intv_audio_publish(clean, snd->buf_size);

        if (avi_active && avi_start_time(snd->pvt->avi) < snd->periph.now)
            avi_record_audio(snd->pvt->avi, clean, snd->buf_size,
                             !not_silent);

        if (snd->raw_file && not_silent)
        {
            fwrite(clean, sizeof(int16_t), snd->buf_size,
                    snd->raw_file);
            snd->raw_start = 1;
        }

        assert(dly_drop > 0);
        if (dly_drop > 0)
        {
            snd->mixbuf.clean[snd->mixbuf.num_clean++] = clean;
            dly_drop--;
            did_drop++;
        }
    }

    for (i = 0; i < snd->src_cnt; i++)
    {
        snd->src[i]->tot_drop += did_drop;
        if (snd->src[i]->drop >= did_drop)
            snd->src[i]->drop -= did_drop;
        else
            snd->src[i]->drop = 0;
    }

    for (i = 0; i < snd->src_cnt; i++)
    {
        for (j = 0; j < min_num_dirty; j++)
            snd->src[i]->clean[snd->src[i]->num_clean++] =
                snd->src[i]->dirty[j];

        snd->src[i]->num_dirty -= min_num_dirty;

        if (min_num_dirty > 0)
        {
            for (j = 0; j < snd->src[i]->num_dirty; j++)
            {
                snd->src[i]->dirty[j] = snd->src[i]->dirty[min_num_dirty + j];
                snd->src[i]->dirty[min_num_dirty + j] = NULL;
            }
        }
    }

    snd->samples += min_num_dirty * snd->buf_size;
    if (snd->time_scale > 0)
    {
        new_now = (double)snd->samples * snd->cyc_per_sec * snd->time_scale
                                                                  / snd->rate;
        if (new_now >= snd->periph.now)
            len = new_now - snd->periph.now;
    }

    if (snd->raw_file && not_silent)
        snd_update_wav_hdr(snd->raw_file, snd->rate);

    return len;
}

int snd_register
(
    periph_t    *const per,
    snd_buf_t   *const src
)
{
    int i;
    snd_t *const snd = PERIPH_AS(snd_t, per);

    memset(src, 0, sizeof(snd_buf_t));
    src->snd       = snd;

    src->num_clean = snd->buf_cnt;
    src->tot_buf   = snd->buf_cnt;
    src->num_dirty = 0;

    src->buf   = CALLOC(int16_t,   snd->buf_size * src->num_clean);
    src->clean = CALLOC(int16_t *, src->num_clean);
    src->dirty = CALLOC(int16_t *, src->num_clean);

    if (!src->buf || !src->clean || !src->dirty)
    {
        fprintf(stderr, "snd_register: Out of memory allocating sndbuf.\n");
        return -1;
    }

    for (i = 0; i < src->num_clean; i++)
    {
        src->clean[i] = src->buf + i * snd->buf_size;
        src->dirty[i] = NULL;
    }

    snd->src_cnt++;
    snd->src = (snd_buf_t**) realloc(snd->src,
                                     snd->src_cnt * sizeof(snd_buf_t*));
    if (!snd->src)
    {
        fprintf(stderr, "Error:  Out of memory in snd_register()\n");
        return -1;
    }
    snd->src[snd->src_cnt - 1] = src;

    return 0;
}

LOCAL void snd_dtor(periph_t *const p);

int snd_init(snd_t *snd, int rate, char *raw_file,
             int user_snd_buf_size, int user_snd_buf_cnt,
             struct avi_writer_t *const avi, int pal_mode, double time_scale)
{
    int i;

    memset(snd, 0, sizeof(snd_t));
    if (!(snd->pvt = CALLOC(snd_pvt_t, 1)))
    {
        fprintf(stderr, "snd_init: Out of memory allocating snd_pvt_t.\n");
        goto fail;
    }

    snd->buf_size = user_snd_buf_size > 0 ? user_snd_buf_size
                  :                         SND_BUF_SIZE_DEFAULT;

    snd->buf_cnt  = user_snd_buf_cnt  > 0 ? user_snd_buf_cnt
                  :                         SND_BUF_CNT_DEFAULT;

    snd->pvt->avi         = avi;

    snd->rate             = rate;
    snd->atten            = 0;
    snd->cyc_per_sec      = pal_mode ? 1000000 : 894886;
    snd->time_scale       = time_scale;

    snd->periph.read      = NULL;
    snd->periph.write     = NULL;
    snd->periph.peek      = NULL;
    snd->periph.poke      = NULL;
    snd->periph.tick      = snd_tick;
    snd->periph.min_tick  = snd->buf_size * snd->cyc_per_sec / (2*rate);
    snd->periph.max_tick  = snd->periph.min_tick * 3;
    snd->periph.addr_base = ~0U;
    snd->periph.addr_mask = ~0U;
    snd->periph.dtor      = snd_dtor;

    snd->mixbuf.tot_buf   = snd->buf_cnt;
    snd->mixbuf.num_clean = snd->buf_cnt;
    snd->mixbuf.num_dirty = 0;
    snd->mixbuf.buf   = CALLOC(int16_t, snd->buf_size * snd->mixbuf.num_clean);
    snd->mixbuf.clean = CALLOC(int16_t *, snd->mixbuf.num_clean);
    snd->mixbuf.dirty = CALLOC(int16_t *, snd->mixbuf.num_clean);

    if (mixbuf) free(mixbuf);
    mixbuf            = CALLOC(int32_t,   snd->buf_size);

    if (!snd->mixbuf.buf || !snd->mixbuf.clean || !snd->mixbuf.dirty || !mixbuf)
    {
        fprintf(stderr, "snd_init: Out of memory allocating mixbuf.\n");
        goto fail;
    }

    for (i = 0; i < snd->mixbuf.num_clean; i++)
        snd->mixbuf.clean[i] = snd->mixbuf.buf + i * snd->buf_size;

    if (raw_file)
    {
        snd->raw_file = fopen(raw_file, "wb");
        if (!snd->raw_file)
        {
            fprintf(stderr,"snd:  Error opening '%s' for writing.\n",raw_file);
            perror("fopen");
            goto fail;
        }
        snd_update_wav_hdr(snd->raw_file, snd->rate);
        return 0;
    }

    return 0;


fail:
    CONDFREE(snd->pvt);
    CONDFREE(snd->mixbuf.buf);
    CONDFREE(snd->mixbuf.clean);
    CONDFREE(snd->mixbuf.dirty);
    CONDFREE(mixbuf);

    return -1;
}

LOCAL void snd_dtor(periph_t *const p)
{
    snd_t *const snd = PERIPH_AS(snd_t, p);
    int i;

    CONDFREE(mixbuf);
    CONDFREE(snd->mixbuf.buf);
    CONDFREE(snd->mixbuf.clean);
    CONDFREE(snd->mixbuf.dirty);

    CONDFREE(snd->pvt);

    for (i = 0; i < snd->src_cnt; i++)
        if (snd->src[i])
        {
            CONDFREE(snd->src[i]->buf);
            CONDFREE(snd->src[i]->clean);
            CONDFREE(snd->src[i]->dirty);
        }

    CONDFREE(snd->src);
}

void snd_play_silence(snd_t *const snd) { UNUSED(snd); }
void snd_play_static (snd_t *const snd) { UNUSED(snd); }
