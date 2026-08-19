/*
 * disc_test -- exercises intvsession_disc_from_point, the pointer hit test
 * every frontend's keypad-window disc widget shares. Pure arithmetic, so
 * this needs neither a session nor a toolkit nor a mouse.
 *
 * The interesting property is the one the four frontends kept getting
 * wrong on their own: which sector a given screen offset belongs to, in a
 * y-DOWN coordinate system, across all 16 positions rather than just the 8
 * even ones.
 */
#include <math.h>
#include <stdio.h>

#include "intvsession.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "disc_test: FAILED: %s\n", what);
        failed = 1;
    }
}

/* A point on the rim at `deg` in ordinary compass/math degrees (0 = East,
 * counter-clockwise positive), converted to the screen convention the
 * function takes -- i.e. sin negated, exactly as each frontend's own
 * drawing code converts back the other way. */
static int dir_at(double deg, double radius)
{
    const double rad = deg * M_PI / 180.0;
    return intvsession_disc_from_point(radius * cos(rad), -radius * sin(rad),
                                       radius);
}

int main(void)
{
    const double r = 100.0;

    /* ---- deadzone ---- */
    check("dead centre", intvsession_disc_from_point(0.0, 0.0, r) == -1);
    check("inside the deadzone",
          intvsession_disc_from_point(r * INTVSESSION_DISC_DEADZONE_FRAC * 0.5,
                                      0.0, r) == -1);
    check("just outside the deadzone",
          intvsession_disc_from_point(r * INTVSESSION_DISC_DEADZONE_FRAC * 1.1,
                                      0.0, r) == 0);
    check("degenerate radius", intvsession_disc_from_point(5.0, 5.0, 0.0) == -1);

    /* ---- the 8 compass positions, in SCREEN coordinates ----
     * North is a NEGATIVE dy (up the screen) and must not come out as
     * South: that inversion was a real bug in three of the four frontends'
     * private copies of this arithmetic. */
    check("east", intvsession_disc_from_point(r, 0.0, r) == 0);
    check("north (dy negative)", intvsession_disc_from_point(0.0, -r, r) == 4);
    check("west", intvsession_disc_from_point(-r, 0.0, r) == 8);
    check("south (dy positive)", intvsession_disc_from_point(0.0, r, r) == 12);
    check("north-east", dir_at(45.0, r) == 2);
    check("north-west", dir_at(135.0, r) == 6);
    check("south-west", dir_at(225.0, r) == 10);
    check("south-east", dir_at(315.0, r) == 14);

    /* ---- the 8 odd half-steps, which the old 8-way snap could never
     * reach at all (intv_host.c's disc_codes: ENE = E|NE, and so on) ---- */
    check("east-north-east", dir_at(22.5, r) == 1);
    check("north-north-east", dir_at(67.5, r) == 3);
    check("north-north-west", dir_at(112.5, r) == 5);
    check("west-north-west", dir_at(157.5, r) == 7);
    check("west-south-west", dir_at(202.5, r) == 9);
    check("south-south-west", dir_at(247.5, r) == 11);
    check("south-south-east", dir_at(292.5, r) == 13);
    check("east-south-east", dir_at(337.5, r) == 15);

    /* ---- sector boundaries ----
     * Each position owns exactly the 22.5 degrees centred on it, so East's
     * sector wraps across 0 and its upper neighbour starts at 11.25. */
    check("just below the E/ENE boundary", dir_at(11.0, r) == 0);
    check("just above the E/ENE boundary", dir_at(11.5, r) == 1);
    check("just below 360 wraps to east", dir_at(355.0, r) == 0);
    check("just above the ESE/E boundary", dir_at(349.0, r) == 0);
    check("just below the ESE/E boundary", dir_at(348.0, r) == 15);

    /* ---- every angle lands in the sector it points at ---- */
    {
        int all_within = 1;
        for (int tenths = 0; tenths < 3600; tenths++) {
            const double deg = tenths / 10.0;
            const int dir = dir_at(deg, r);
            double delta;
            if (dir < 0 || dir >= INTVSESSION_DISC_POSITIONS) {
                all_within = 0;
                break;
            }
            delta = fabs(deg - dir * INTVSESSION_DISC_SECTOR_DEG);
            if (delta > 180.0)
                delta = 360.0 - delta; /* the wrap across East */
            if (delta > INTVSESSION_DISC_SECTOR_DEG / 2.0 + 1e-9) {
                fprintf(stderr,
                        "disc_test: %.1f degrees resolved to %d (%.2f off)\n",
                        deg, dir, delta);
                all_within = 0;
                break;
            }
        }
        check("every angle resolves to the nearest sector centre",
              all_within);
    }

    /* ---- all 16 positions are reachable ---- */
    {
        int seen[INTVSESSION_DISC_POSITIONS] = {0};
        int missing = 0;
        for (int tenths = 0; tenths < 3600; tenths++) {
            const int dir = dir_at(tenths / 10.0, r);
            if (dir >= 0)
                seen[dir] = 1;
        }
        for (int i = 0; i < INTVSESSION_DISC_POSITIONS; i++)
            if (!seen[i])
                missing = 1;
        check("all 16 disc positions are reachable from the widget",
              !missing);
    }

    if (!failed)
        printf("disc_test: OK\n");
    return failed;
}
