/*
 * ============================================================================
 *  Title:    Graphics Interface Routines -- desktop (frame-capturing) backend
 * ============================================================================
 *  Takes the place of jzIntv's own gfx_sdl1.c/gfx_sdl2.c in the object list
 *  (see core/CMakeLists.txt's header comment on how backend selection works)
 *  -- and of gfx_null.c, which this is a close copy of. The only real change
 *  from gfx_null.c is in gfx_refresh: instead of doing nothing, it publishes
 *  the completed frame into intv_frame.h's store for a UI thread to pull.
 *
 *  Everything else (gfx_check/gfx_init/gfx_dtor/gfx_toggle_windowed/
 *  gfx_force_windowed/gfx_set_title/gfx_stic_tick/gfx_vid_enable/
 *  gfx_set_bord) is gfx_null.c's own logic, verbatim -- there is no window to
 *  manage, no SDL surface to touch, and the movie/AVI/screenshot bookkeeping
 *  in gfx_stic_tick is toolkit-agnostic already (gfx_movieupd/gfx_aviupd/
 *  gfx_scrshot live in the shared src/gfx/gfx.c, not any backend).
 * ============================================================================
 */

#include "config.h"
#include "periph/periph.h"
#include "gfx/gfx.h"
#include "gfx/palette.h"
#include "mvi/mvi.h"
#include "avi/avi.h"
#include "lzoe/lzoe.h"
#include "file/file.h"

#include "../intv_frame.h"

/* The Video tab's per-frame capture (intvvidcap.h) reads the STIC through
 * the machine struct, whose header chain is the same one the debugger uses. */
#include <pthread.h>
#include <stdatomic.h>
#include "cp1600/cp1600.h"
#include "mem/mem.h"
#include "ecs/ecs.h"
#include "icart/icart.h"
#include "bincfg/bincfg.h"
#include "bincfg/legacy.h"
#include "pads/pads.h"
#include "pads/pads_cgc.h"
#include "pads/pads_intv2pc.h"
#include "snd/snd.h"
#include "ay8910/ay8910.h"
#include "demo/demo.h"
#include "stic/stic.h"
#include "speed/speed.h"
#include "debug/debug_.h"
#include "debug/debug_if.h"
#include "event/event.h"
#include "ivoice/ivoice.h"
#include "jlp/jlp.h"
#include "fujinet/fujinet.h"
#include "locutus/locutus_adapt.h"
#include "cheat/cheat.h"
#include "cfg/mapping.h"
#include "cfg/cfg.h"
#include "../../include/intvvidcap.h"

typedef struct gfx_pvt_t
{
    int         vid_enable;         /*  Video enable flag.                  */
} gfx_pvt_t;

LOCAL void gfx_dtor(periph_t *const p);

int gfx_check(int desire_x, int desire_y, int desire_bpp, int prescaler)
{
    UNUSED(desire_x);
    UNUSED(desire_y);
    UNUSED(desire_bpp);
    UNUSED(prescaler);
    return 0;
}

int gfx_init(gfx_t *gfx, int desire_x, int desire_y, int desire_bpp,
                         int flags,    int verbose,  int prescaler,
                         int border_x, int border_y, int pal_mode,
                         struct avi_writer_t *const avi, int audio_rate,
                         const palette_t *const palette)
{
    UNUSED(desire_x);
    UNUSED(desire_y);
    UNUSED(desire_bpp);
    UNUSED(flags);
    UNUSED(verbose);
    UNUSED(prescaler);
    UNUSED(border_x);
    UNUSED(border_y);
    UNUSED(pal_mode);

    assert(gfx);
    memset((void*)gfx, 0, sizeof(gfx_t));

    gfx->vid = CALLOC(uint8_t,   160 * 200);
    gfx->pvt = CALLOC(gfx_pvt_t, 1);

    if (!gfx->vid || !gfx->pvt)
    {
        fprintf(stderr, "gfx:  Panic:  Could not allocate memory.\n");
        goto die;
    }

    gfx->pvt->vid_enable = 0;
    gfx->dirty      = 3;
    gfx->b_dirty    = 3;
    gfx->palette    = *palette;

    gfx->fps        = pal_mode ? 50 : 60;
    gfx->avi        = avi;
    gfx->audio_rate = audio_rate;

    gfx->hidden     = false; /* unlike gfx_null.c: a real frontend is
                              * (eventually) watching, so frames should not
                              * be dropped for being "iconified". */

    gfx->periph.read        = NULL;
    gfx->periph.write       = NULL;
    gfx->periph.peek        = NULL;
    gfx->periph.poke        = NULL;
    gfx->periph.tick        = NULL;
    gfx->periph.min_tick    = 0;
    gfx->periph.max_tick    = INT_MAX;
    gfx->periph.addr_base   = 0;
    gfx->periph.addr_mask   = 0;
    gfx->periph.dtor        = gfx_dtor;

    return 0;

die:
    CONDFREE(gfx->pvt);
    CONDFREE(gfx->vid);
    return -1;
}

LOCAL void gfx_dtor(periph_t *const p)
{
    gfx_t *const gfx = PERIPH_AS(gfx_t, p);

    if (gfx->movie)
    {
        if (gfx->movie->f)
            fclose(gfx->movie->f);

        CONDFREE(gfx->movie);
    }

    if (avi_is_active(gfx->avi))
        avi_end_video(gfx->avi);

    CONDFREE(gfx->pvt);
    CONDFREE(gfx->vid);
}

bool gfx_toggle_windowed(gfx_t *gfx, int quiet)
{
    UNUSED(gfx);
    UNUSED(quiet);
    return false;
}

int gfx_force_windowed(gfx_t *gfx, int quiet)
{
    UNUSED(gfx);
    UNUSED(quiet);
    return 0;
}

int gfx_set_title(gfx_t *gfx, const char *title)
{
    UNUSED(gfx);
    UNUSED(title);
    return 0;
}

/* ======================================================================== */
/*  GFX_REFRESH -- publish the completed frame for a UI thread to pull.     */
/* ======================================================================== */
void gfx_refresh(gfx_t *const gfx)
{
    if ((gfx->tot_frames++ & 31) == 0)
    {
        gfx->dirty |= 3;
        gfx->b_dirty |= 3;
    }

    if (!gfx->scrshot && (!gfx->dirty || gfx->hidden))
    {
        return;
    }

    if (gfx->dropped_frames)
    {
        gfx->tot_dropped_frames += gfx->dropped_frames;
        gfx->dropped_frames = 0;
    }

    /* gfx_vid_enable() (below) is a two-phase commit: on a real state
     * change it sets the pending bit (bit 1) but deliberately leaves bit 0
     * -- the value actually read here -- at its OLD setting, because the
     * bit-0 flip is meant to happen exactly once, at the next real
     * redraw. gfx_null.c (this file started as a close copy of it, per
     * this file's own header) never had to finish that contract because
     * it never draws anything and never reads vid_enable for real; we DO
     * read it (intv_frame_publish below halves brightness when it is 0,
     * mirroring gfx_sdl2.c's own pal_off), so we have to do what
     * gfx_sdl2.c's own refresh path does -- commit the pending toggle
     * right here, once, before reading it. Without this, bit 0 never
     * moves off its startup value of 0 once the STIC turns the display
     * on: every frame calls gfx_vid_enable(gfx, 1), sees bit 0 still 0,
     * re-arms the pending bit, and bit 0 is never actually flipped --
     * a permanent, session-long "blanked" (half brightness) read. */
    if (gfx->pvt->vid_enable & 2)
    {
        gfx->pvt->vid_enable &= 1;
        gfx->pvt->vid_enable ^= 1;
    }

    intv_frame_publish(gfx->vid, gfx->palette.color, gfx->pvt->vid_enable & 1);

    gfx->dirty = 0;
    gfx->b_dirty = 0;
}

/* ======================================================================== */
/*  The Video tab's capture: one copy of the STIC per published frame, made */
/*  on the emulator thread right after stic_update, only while a tab wants  */
/*  it. The gfx pvt is private to this file, so the fill lives here too.    */
/* ======================================================================== */
static pthread_mutex_t s_vidcap_lock = PTHREAD_MUTEX_INITIALIZER;
static intvvid_capture s_vidcap;
static atomic_int s_vidcap_users = 0;

static void vidcap_take(intvvid_capture *dst, const gfx_t *gfx)
{
    const stic_t *stic = &intv.stic;

    memcpy(dst->raw, stic->raw, sizeof dst->raw);
    memcpy(dst->gmem, stic->gmem, sizeof dst->gmem);
    memcpy(dst->mob_bmp, stic->mob_bmp, sizeof dst->mob_bmp);
    memcpy(dst->btab, stic->btab, sizeof dst->btab);
    memcpy(dst->btab_sr, stic->btab_sr, sizeof dst->btab_sr);
    memcpy(dst->vid, gfx->vid, sizeof dst->vid);
    memcpy(dst->palette, gfx->palette.color, sizeof dst->palette);
    dst->vid_enable = gfx->pvt->vid_enable & 1;
    dst->mode = stic->mode;
    dst->p_mode = stic->p_mode;
    dst->gram_size = stic->gram_size;
    dst->gram_mask = stic->gram_mask;
    dst->pal = stic->pal;
    dst->cycle = stic->eff_cycle;
    dst->last_intrq = stic->last_frame_intrq;
    dst->next_intrq = stic->next_frame_intrq;
    dst->busrq_count = stic->busrq_count;
    dst->fifo_wr_ptr = stic->fifo_wr_ptr;
}

void intv_host_capture_enable(int on)
{
    if (on)
    {
        atomic_fetch_add(&s_vidcap_users, 1);
    } else if (atomic_load(&s_vidcap_users) > 0)
    {
        atomic_fetch_sub(&s_vidcap_users, 1);
    }
}

int intv_host_capture_get(intvvid_capture *dst, uint64_t *serial_inout)
{
    int changed;

    pthread_mutex_lock(&s_vidcap_lock);
    changed = s_vidcap.serial != 0 && *serial_inout != s_vidcap.serial;
    if (changed)
    {
        memcpy(dst, &s_vidcap, sizeof *dst);
        *serial_inout = s_vidcap.serial;
    }
    pthread_mutex_unlock(&s_vidcap_lock);
    return changed;
}

void intv_host_capture_fill(intvvid_capture *dst)
{
    uint64_t serial;

    pthread_mutex_lock(&s_vidcap_lock);
    serial = s_vidcap.serial;
    dst->frame = s_vidcap.frame;
    pthread_mutex_unlock(&s_vidcap_lock);

    vidcap_take(dst, &intv.gfx);
    dst->cycle = intv.cp1600.periph.now;
    dst->serial = serial;
    dst->live = 1;
}

void gfx_stic_tick(gfx_t *const gfx)
{
    gfx->tot_frames++;

    if (gfx->scrshot & (GFX_MOVIE | GFX_MVTOG))
        gfx_movieupd(gfx);

    if (gfx->scrshot & (GFX_AVI | GFX_AVTOG))
        gfx_aviupd(gfx);

    if (gfx->drop_frame)
    {
        gfx->drop_frame--;
        gfx->tot_frames++;
        if (gfx->dirty) gfx->dropped_frames++;
        return;
    }

    gfx_refresh(gfx);

    if (atomic_load(&s_vidcap_users) > 0)
    {
        pthread_mutex_lock(&s_vidcap_lock);
        vidcap_take(&s_vidcap, gfx);
        s_vidcap.live = 0;
        s_vidcap.frame++;
        s_vidcap.serial++;
        pthread_mutex_unlock(&s_vidcap_lock);
    }

    if (gfx->scrshot & GFX_SHOT)
    {
        gfx_scrshot(gfx);
        gfx->scrshot &= ~GFX_SHOT;
    }

    return;
}

void gfx_vid_enable(gfx_t *gfx, int enabled)
{
    enabled = enabled == VID_ENABLED;

    if ((gfx->pvt->vid_enable ^ enabled) & 1)
    {
        gfx->pvt->vid_enable |= 2;
        gfx->dirty |= 2;
    } else
    {
        gfx->pvt->vid_enable = enabled;
    }
}

void gfx_set_bord
(
    gfx_t *gfx,
    int b_color
)
{
    int dirty = 0;

    if (gfx->b_color != b_color) { gfx->b_color = b_color; dirty = 3; }

    if (dirty)     { gfx->dirty   |= 1; }
    if (dirty & 2) { gfx->b_dirty |= 2; }
}
