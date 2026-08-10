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

    intv_frame_publish(gfx->vid, gfx->palette.color, gfx->pvt->vid_enable & 1);

    gfx->dirty = 0;
    gfx->b_dirty = 0;
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
