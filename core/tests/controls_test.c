/*
 * controls_test -- the Controllers window's layout and hit tests
 * (core/src/controls.c), and the ECS keyboard's. Pure: no session, no machine.
 * Every part is reachable at its centre and drives the right Map target, the
 * layout with the ECS on is as tall as without it, the disc is a circle with
 * the shared dead centre and 16 sectors, and the ECS layout holds the 48
 * physical keys once each plus the second SHIFT cap.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "intvsession.h"

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass)
        failures++;
}

static const intvsession_control *find(const intvsession_control *ctl, int count,
                                       int kind, int side, int target, int nth)
{
    int idx, seen = 0;
    for (idx = 0; idx < count; idx++)
        if (ctl[idx].kind == kind && ctl[idx].side == side && ctl[idx].target == target)
            if (seen++ == nth)
                return &ctl[idx];
    return NULL;
}

static int centre_hit(int ecs_on, const intvsession_control *ctl)
{
    return intvsession_controls_hit(ecs_on, ctl->x + ctl->w * 0.5f, ctl->y + ctl->h * 0.5f);
}

int main(void)
{
    int count2, count4, idx, side, key, ecs_on;
    float w2, h2, w4, h4;
    const intvsession_control *lay2 = intvsession_controls_layout(0, &count2, &w2, &h2);
    const intvsession_control *lay4 = intvsession_controls_layout(1, &count4, &w4, &h4);

    check(lay2 && lay4 && count2 > 40 && count4 > count2, "both layouts exist, ECS adds handsets");
    check(h2 == h4, "the height is the same with the ECS on and off");
    check(w4 > w2 && fabsf(w2 - 23.0f) < 0.01f && fabsf(w4 - 47.0f) < 0.01f, "two handsets 23 u wide, four 47 u");
    check(lay2 == intvsession_controls_layout(0, NULL, NULL, NULL), "the table is static");

    /* everything lies inside the extent */
    for (ecs_on = 0; ecs_on < 2; ecs_on++) {
        int count, inside = 1;
        float width, height;
        const intvsession_control *ctl = intvsession_controls_layout(ecs_on, &count, &width, &height);
        for (idx = 0; idx < count; idx++)
            if (ctl[idx].x < -0.001f || ctl[idx].y < -0.001f ||
                ctl[idx].x + ctl[idx].w > width + 0.001f || ctl[idx].y + ctl[idx].h > height + 0.001f)
                inside = 0;
        check(inside, ecs_on ? "four-handset parts stay inside the extent" : "two-handset parts stay inside the extent");
    }

    /* every key of every side is a pressable part that hits itself and maps to its target */
    for (ecs_on = 0; ecs_on < 2; ecs_on++) {
        int count, sides = ecs_on ? 4 : 2, ok = 1, keys_seen = 0;
        const intvsession_control *ctl = intvsession_controls_layout(ecs_on, &count, NULL, NULL);
        for (side = 0; side < sides; side++) {
            for (key = 0; key < INTVSESSION_KEY_COUNT; key++) {
                const int kind = key >= INTVSESSION_ACTION_TOP ? INTVSESSION_CTL_ACTION : INTVSESSION_CTL_KEY;
                const int expect = key == INTVSESSION_ACTION_TOP ? 2 : 1;
                int nth;
                for (nth = 0; nth < expect; nth++) {
                    const intvsession_control *ctl_found = find(ctl, count, kind, side, key, nth);
                    intvsession_key_mapping map;
                    if (!ctl_found) { ok = 0; continue; }
                    keys_seen++;
                    map = intvsession_control_target(ctl_found);
                    if (centre_hit(ecs_on, ctl_found) != (int)(ctl_found - ctl) ||
                        map.kind != INTVSESSION_MAP_KEY || (int)map.side != side || (int)map.key != key)
                        ok = 0;
                }
                if (find(ctl, count, kind, side, key, expect))
                    ok = 0;   /* no more than expected */
            }
        }
        check(ok, ecs_on ? "ECS on: every key and tab of 4 handsets is reachable and maps to its target"
                         : "ECS off: every key and tab of 2 handsets is reachable and maps to its target");
        check(keys_seen == sides * (INTVSESSION_KEY_COUNT + 1), "each handset has 12 keys and 4 tabs (Top twice)");
    }

    /* the upper tabs are both Top, one on each edge of the body */
    {
        const intvsession_control *left_tab = find(lay2, count2, INTVSESSION_CTL_ACTION, 0, INTVSESSION_ACTION_TOP, 0);
        const intvsession_control *right_tab = find(lay2, count2, INTVSESSION_CTL_ACTION, 0, INTVSESSION_ACTION_TOP, 1);
        const intvsession_control *body = NULL;
        for (idx = 0; idx < count2; idx++)
            if (lay2[idx].kind == INTVSESSION_CTL_BODY && !strcmp(lay2[idx].label, "Handset") && lay2[idx].side == 0)
                body = &lay2[idx];
        check(left_tab && right_tab && body && left_tab->x < body->x && left_tab->x > body->x - 0.5f &&
              right_tab->x + right_tab->w > body->x + body->w &&
              right_tab->x + right_tab->w < body->x + body->w + 0.5f,
              "the side tabs protrude 0.4 u past the body on both edges");
        check(body && fabsf(body->w - 10.0f) < 0.001f && fabsf(body->h - 20.0f) < 0.001f, "the body is 10 x 20 u");
    }

    /* the disc: a circle, 16 sectors, a dead centre */
    for (side = 0; side < 4; side++) {
        const intvsession_control *disc = find(lay4, count4, INTVSESSION_CTL_DISC, side, -1, 0);
        float ctr_x, ctr_y, rad;
        int dir, ok = 1;
        check(disc != NULL, "a disc on every handset");
        if (!disc)
            continue;
        ctr_x = disc->x + disc->w * 0.5f;
        ctr_y = disc->y + disc->h * 0.5f;
        rad = disc->w * 0.5f;
        check(fabsf(rad - 2.8f) < 0.001f && fabsf(disc->w - disc->h) < 0.001f, "the disc's hit radius is 2.8 u");
        check(intvsession_controls_hit(1, ctr_x, ctr_y) == (int)(disc - lay4), "the centre is the disc");
        check(intvsession_controls_disc_dir(disc, ctr_x, ctr_y) == -1, "the dead centre is no direction");
        check(intvsession_controls_disc_dir(disc, ctr_x + rad * 0.2f, ctr_y) == -1, "inside 22% is still the dead centre");
        for (dir = 0; dir < INTVSESSION_DISC_POSITIONS; dir++) {
            const double deg = dir * INTVSESSION_DISC_SECTOR_DEG;
            const float pt_x = ctr_x + (float)(rad * 0.8 * cos(deg * M_PI / 180.0));
            const float pt_y = ctr_y - (float)(rad * 0.8 * sin(deg * M_PI / 180.0));
            if (intvsession_controls_disc_dir(disc, pt_x, pt_y) != dir ||
                intvsession_controls_hit(1, pt_x, pt_y) != (int)(disc - lay4))
                ok = 0;
        }
        check(ok, "all 16 sectors resolve, East = 0 counter-clockwise");
        /* the square's corner is outside the circle */
        check(intvsession_controls_hit(1, disc->x + 0.05f, disc->y + 0.05f) != (int)(disc - lay4),
              "the disc is round: its bounding box corner misses");
        check(intvsession_controls_disc_dir(&lay4[0], 0, 0) == -1, "a non-disc has no sector");
    }

    /* the keys are rounded rectangles; their corner misses */
    {
        const intvsession_control *key5 = find(lay2, count2, INTVSESSION_CTL_KEY, 0, INTVSESSION_KEY_5, 0);
        check(key5 && intvsession_controls_hit(0, key5->x + 0.02f, key5->y + 0.02f) !=
                          (int)(key5 - lay2) &&
              intvsession_controls_hit(0, key5->x + 0.5f, key5->y + key5->h * 0.5f) == (int)(key5 - lay2),
              "a key's corner is rounded off, its edge middle is in");
        check(key5 && key5->w >= 2.1f && key5->h >= 1.8f, "key cells are 2.2 x 1.9 u");
    }

    /* the console RESET is a Reset Game button, the rest of the strip is not a target */
    {
        const intvsession_control *reset = find(lay2, count2, INTVSESSION_CTL_BUTTON, -1,
                                                INTVSESSION_SYSACT_RESET_GAME, 0);
        intvsession_key_mapping map;
        int buttons = 0, onoff_dim = 0;
        for (idx = 0; idx < count2; idx++) {
            if (lay2[idx].kind == INTVSESSION_CTL_BUTTON)
                buttons++;
            if (lay2[idx].kind == INTVSESSION_CTL_BODY && !strcmp(lay2[idx].label, "OnOff") &&
                intvsession_controls_hit(0, lay2[idx].x + 0.1f, lay2[idx].y + 0.5f) < 0)
                onoff_dim = 1;
        }
        check(reset != NULL && buttons == 1, "one console button");
        map = intvsession_control_target(reset);
        check(map.kind == INTVSESSION_MAP_SYSACT && map.sysact == INTVSESSION_SYSACT_RESET_GAME, "RESET is Reset Game");
        check(reset && centre_hit(0, reset) == (int)(reset - lay2), "RESET is reachable");
        check(onoff_dim, "ON/OFF is dimmed and not a target");
    }

    /* captions name the handsets */
    {
        static const char *const want[4] = { "Player 1", "Player 2", "ECS 1", "ECS 2" };
        int seen[4] = { 0, 0, 0, 0 }, saw;
        for (idx = 0; idx < count4; idx++)
            for (saw = 0; saw < 4; saw++)
                if (lay4[idx].kind == INTVSESSION_CTL_CAPTION && !strcmp(lay4[idx].label, want[saw]) &&
                    lay4[idx].side == saw)
                    seen[saw] = 1;
        check(seen[0] && seen[1] && seen[2] && seen[3], "captions Player 1, Player 2, ECS 1, ECS 2 on sides 0-3");
    }

    /* nothing hits outside the parts or on bodies and captions */
    check(intvsession_controls_hit(0, -1.0f, -1.0f) == -1 && intvsession_controls_hit(0, 500.0f, 500.0f) == -1,
          "outside the layout hits nothing");
    check(intvsession_controls_hit(0, 0.3f, 0.3f) == -1, "a caption is not pressable");
    check(intvsession_control_target(NULL).kind == INTVSESSION_MAP_NONE, "no control, no target");
    check(intvsession_control_target(&lay2[0]).kind == INTVSESSION_MAP_NONE, "a caption has no target");

    /* ---- the ECS keyboard ---- */
    {
        int count, caps = 0, shift_caps = 0, distinct = 0, owned[INTVSESSION_ECS_KEY_NONE], ok_inside = 1,
            ok_hit = 1, labels = 1, latches = 0;
        float width, height;
        const intvsession_ecs_cap *cap = intvsession_ecs_layout(&count, &width, &height);
        memset(owned, 0, sizeof owned);
        check(cap != NULL && width > 20.0f && height > 10.0f, "the ECS layout exists");
        for (idx = 0; idx < count; idx++) {
            if (cap[idx].kind != INTVSESSION_ECS_KEYCAP)
                continue;
            caps++;
            if (cap[idx].key < 0 || cap[idx].key >= INTVSESSION_ECS_KEY_PHYSICAL_COUNT) {
                labels = 0;
                continue;
            }
            if (!owned[cap[idx].key])
                distinct++;
            owned[cap[idx].key]++;
            if (cap[idx].key == INTVSESSION_ECS_KEY_SHIFT)
                shift_caps++;
            if (!cap[idx].label || !cap[idx].label[0])
                labels = 0;
            if (cap[idx].flags & INTVSESSION_ECSCAP_LATCH)
                latches++;
            if (cap[idx].x < 0 || cap[idx].y < 0 || cap[idx].x + cap[idx].w > width || cap[idx].y + cap[idx].h > height)
                ok_inside = 0;
            if (intvsession_ecs_hit(cap[idx].x + cap[idx].w * 0.5f, cap[idx].y + cap[idx].h * 0.5f) != idx)
                ok_hit = 0;
        }
        check(distinct == INTVSESSION_ECS_KEY_PHYSICAL_COUNT && distinct == 48, "the 48 physical keys each appear");
        check(caps == 49 && shift_caps == 2, "49 caps: the SHIFT key has two");
        {
            int once = 1, key;
            for (key = 0; key < INTVSESSION_ECS_KEY_PHYSICAL_COUNT; key++)
                if (owned[key] != (key == INTVSESSION_ECS_KEY_SHIFT ? 2 : 1))
                    once = 0;
            check(once, "every other key has exactly one cap");
        }
        check(labels, "every cap has a label");
        check(ok_inside, "caps stay inside the extent");
        check(ok_hit, "every cap is hit at its centre");
        check(latches == 3, "SHIFT (twice) and CTL latch");
        check(intvsession_ecs_hit(-1.0f, -1.0f) == -1, "outside hits nothing");
        for (idx = 0; idx < count; idx++)
            if (cap[idx].kind == INTVSESSION_ECS_BODY &&
                intvsession_ecs_hit(cap[idx].x + 0.01f, cap[idx].y + 0.01f) >= 0 && !strcmp(cap[idx].label, "Case"))
                check(0, "the case is not a key");
        {
            const intvsession_ecs_cap *one = NULL, *rtn = NULL, *space = NULL;
            for (idx = 0; idx < count; idx++) {
                if (cap[idx].kind != INTVSESSION_ECS_KEYCAP) continue;
                if (cap[idx].key == INTVSESSION_ECS_KEY_1) one = &cap[idx];
                if (cap[idx].key == INTVSESSION_ECS_KEY_ENTER) rtn = &cap[idx];
                if (cap[idx].key == INTVSESSION_ECS_KEY_SPACE) space = &cap[idx];
            }
            check(one && one->shifted && !strcmp(one->shifted, "=") && !strcmp(one->label, "1"), "1 carries '=' above it");
            check(rtn && (rtn->flags & INTVSESSION_ECSCAP_WHITE) && !strcmp(rtn->label, "RTN"), "RTN is the white cap");
            check(space && space->w > 10.0f, "SPACE is the long bar");
        }
    }

    /* the persisted/total target space the window's 'N in all' counts */
    check(INTVSESSION_TARGET_COUNT == 127 && intvsession_target_count() == 127, "127 Map targets in all");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("controls_test: OK\n");
    return 0;
}
