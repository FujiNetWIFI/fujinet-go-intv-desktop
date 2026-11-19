/*
 * controls -- the layout of the on-screen controllers and of the ECS keyboard,
 * for every toolkit.
 *
 * One table (intvsession_controls_layout) says what the Controllers window
 * draws and where, in layout units (DESIGN-intv.md's u) from the top-left; the
 * toolkits draw the art from it and ask intvsession_controls_hit what a click
 * landed on, so the shapes cannot drift apart between GTK, Qt, AppKit and
 * Win32. It has two Intellivision II handsets (four when the ECS is on), over
 * the console strip with its RESET. The height is the same either way, so the
 * window never changes height when the ECS option does.
 *
 * The second table (intvsession_ecs_layout) is the ECS keyboard: the 48
 * physical keys laid out as on the real one, the second SHIFT cap included,
 * with the legends the frontends used to carry tables of their own for.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "intvsession.h"

/* ---- the handsets (layout units) ---------------------------------------------- */

#define CAPTION_H    2.0f    /* the caption row: text 1.2, the cable 0.8 */
#define BODY_W      10.0f
#define BODY_H      20.0f
#define SLOT_W      11.0f    /* a handset with its side tabs and margin */
#define SLOT_PITCH  12.0f
#define CONSOLE_GAP  0.8f
#define CONSOLE_H    4.5f
#define LAYOUT_H    (CAPTION_H + BODY_H + CONSOLE_GAP + CONSOLE_H)

#define KEY_R   0.4f   /* corner radius of a keypad key */
#define TAB_R   0.3f
#define BTN_R   0.3f

#define MAX_CONTROLS 128

typedef struct {
    intvsession_control ctl[MAX_CONTROLS];
    int count;
    float width;
} layout;

static layout g_layout[2];
static int g_built[2];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void add(layout *lay, int kind, const char *label, const char *legend,
                int side, int target, float x, float y, float w, float h)
{
    intvsession_control *ctl;
    if (lay->count >= MAX_CONTROLS)
        return;
    ctl = &lay->ctl[lay->count++];
    ctl->kind = kind;
    ctl->label = label;
    ctl->legend = legend;
    ctl->side = side;
    ctl->target = target;
    ctl->x = x; ctl->y = y; ctl->w = w; ctl->h = h;
}

/* One Intellivision II handset, its body's top-left at (body_x, CAPTION_H). In
 * body-local units: the keypad plate 1.2..8.8 x 2.4..11.6 holding 12 keys of
 * 2.2 x 1.9 on a 2.45 x 2.2 pitch; the disc centred (5.0, 16.0), black pad
 * radius 2.8 in a cream ring of radius 3.3; the side tabs 1.8 u tall, upper at
 * y 6.0 and lower at y 8.8. */
static void add_handset(layout *lay, int side, float slot_x)
{
    static const char *const k_caption[4] = {
        "Player 1", "Player 2", "ECS 1", "ECS 2"
    };
    static const struct { const char *label; int key; } k_keys[4][3] = {
        { { "1", INTVSESSION_KEY_1 }, { "2", INTVSESSION_KEY_2 }, { "3", INTVSESSION_KEY_3 } },
        { { "4", INTVSESSION_KEY_4 }, { "5", INTVSESSION_KEY_5 }, { "6", INTVSESSION_KEY_6 } },
        { { "7", INTVSESSION_KEY_7 }, { "8", INTVSESSION_KEY_8 }, { "9", INTVSESSION_KEY_9 } },
        { { "clear", INTVSESSION_KEY_CLEAR }, { "0", INTVSESSION_KEY_0 },
          { "enter", INTVSESSION_KEY_ENTER } },
    };
    const float body_x = slot_x + 0.5f;     /* the body; the tabs reach 0.4 u past it */
    const float body_y = CAPTION_H;
    int row, col;

    add(lay, INTVSESSION_CTL_CAPTION, k_caption[side], NULL, side, -1,
        slot_x, 0.0f, SLOT_W, 1.2f);
    add(lay, INTVSESSION_CTL_BODY, "Cable", NULL, side, -1,
        body_x + 4.5f, body_y - 0.8f, 1.0f, 0.9f);
    add(lay, INTVSESSION_CTL_BODY, "Handset", NULL, side, -1, body_x, body_y, BODY_W, BODY_H);
    add(lay, INTVSESSION_CTL_BODY, "Keypad", NULL, side, -1,
        body_x + 1.2f, body_y + 2.4f, 7.6f, 9.2f);
    add(lay, INTVSESSION_CTL_BODY, "Card", "overlay card edge", side, -1,
        body_x + 1.0f, body_y + 2.2f, 8.0f, 9.6f);
    for (row = 0; row < 4; row++)
        for (col = 0; col < 3; col++)
            add(lay, INTVSESSION_CTL_KEY, k_keys[row][col].label, NULL, side,
                k_keys[row][col].key,
                body_x + 1.45f + (float)col * 2.45f, body_y + 2.75f + (float)row * 2.2f,
                2.2f, 1.9f);
    add(lay, INTVSESSION_CTL_BODY, "Ring", NULL, side, -1,
        body_x + 1.7f, body_y + 12.7f, 6.6f, 6.6f);
    add(lay, INTVSESSION_CTL_DISC, "DISC", "16 directions", side, -1,
        body_x + 2.2f, body_y + 13.2f, 5.6f, 5.6f);
    /* the side tabs: the two upper ones are the same Top signal */
    add(lay, INTVSESSION_CTL_ACTION, "T", "top button", side,
        INTVSESSION_ACTION_TOP, body_x - 0.4f, body_y + 6.0f, 1.2f, 1.8f);
    add(lay, INTVSESSION_CTL_ACTION, "T", "top button", side,
        INTVSESSION_ACTION_TOP, body_x + BODY_W - 0.8f, body_y + 6.0f, 1.2f, 1.8f);
    add(lay, INTVSESSION_CTL_ACTION, "L", "lower left button", side,
        INTVSESSION_ACTION_LOWER_LEFT, body_x - 0.4f, body_y + 8.8f, 1.2f, 1.8f);
    add(lay, INTVSESSION_CTL_ACTION, "R", "lower right button", side,
        INTVSESSION_ACTION_LOWER_RIGHT, body_x + BODY_W - 0.8f, body_y + 8.8f, 1.2f, 1.8f);
}

/* The console strip under the handsets, `width` wide: the black label band
 * with its two wordmarks and the thin red line under it, and below them from
 * the left the speaker grille, the black block with ON/OFF (dimmed, no target)
 * and RESET (= Reset Game), and the LED. */
static void add_console(layout *lay, float width)
{
    const float strip_y = CAPTION_H + BODY_H + CONSOLE_GAP;
    add(lay, INTVSESSION_CTL_BODY, "Console", NULL, -1, -1, 0.0f, strip_y, width, CONSOLE_H);
    add(lay, INTVSESSION_CTL_BODY, "Band", NULL, -1, -1, 0.4f, strip_y + 0.4f, width - 0.8f, 1.3f);
    add(lay, INTVSESSION_CTL_CAPTION, "INTELLIVISION II", NULL, -1, -1,
        0.9f, strip_y + 0.5f, 10.0f, 1.1f);
    add(lay, INTVSESSION_CTL_CAPTION, "MATTEL ELECTRONICS", NULL, -1, -1,
        width - 10.5f, strip_y + 0.5f, 10.0f, 1.1f);
    add(lay, INTVSESSION_CTL_BODY, "RedLine", NULL, -1, -1,
        0.4f, strip_y + 1.75f, width - 0.8f, 0.12f);
    add(lay, INTVSESSION_CTL_BODY, "Grille", NULL, -1, -1, 0.8f, strip_y + 2.2f, 3.4f, 1.8f);
    add(lay, INTVSESSION_CTL_BODY, "Block", NULL, -1, -1, 5.0f, strip_y + 2.2f, 7.0f, 1.8f);
    add(lay, INTVSESSION_CTL_BODY, "OnOff", "not interactive", -1, -1,
        5.2f, strip_y + 2.3f, 3.2f, 1.6f);
    add(lay, INTVSESSION_CTL_BUTTON, "RESET", "reset the game", -1,
        INTVSESSION_SYSACT_RESET_GAME, 8.6f, strip_y + 2.3f, 3.2f, 1.6f);
    add(lay, INTVSESSION_CTL_BODY, "Led", NULL, -1, -1, 12.6f, strip_y + 2.8f, 0.6f, 0.6f);
}

static const layout *get_layout(int ecs_on)
{
    const int which = ecs_on ? 1 : 0;
    layout *lay = &g_layout[which];

    pthread_mutex_lock(&g_lock);
    if (!g_built[which]) {
        const int handsets = which ? 4 : 2;
        int side;
        memset(lay, 0, sizeof *lay);
        lay->width = (float)handsets * SLOT_PITCH - (SLOT_PITCH - SLOT_W);
        for (side = 0; side < handsets; side++)
            add_handset(lay, side, (float)side * SLOT_PITCH);
        add_console(lay, lay->width);
        g_built[which] = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return lay;
}

const intvsession_control *intvsession_controls_layout(int ecs_on, int *count,
                                                       float *width,
                                                       float *height)
{
    const layout *lay = get_layout(ecs_on);
    if (count)
        *count = lay->count;
    if (width)
        *width = lay->width;
    if (height)
        *height = LAYOUT_H;
    return lay->ctl;
}

/* ---- hit tests ------------------------------------------------------------------ */

/* Inside the rounded rectangle (x, y, w, h) with corner radius `rad`? */
static int in_round_rect(float pt_x, float pt_y, float x, float y, float w,
                         float h, float rad)
{
    float near_x, near_y, off_x, off_y;
    if (pt_x < x || pt_x >= x + w || pt_y < y || pt_y >= y + h)
        return 0;
    /* the corner square nearest the point, if the point is in one */
    near_x = pt_x < x + rad ? x + rad : (pt_x > x + w - rad ? x + w - rad : pt_x);
    near_y = pt_y < y + rad ? y + rad : (pt_y > y + h - rad ? y + h - rad : pt_y);
    off_x = pt_x - near_x;
    off_y = pt_y - near_y;
    return off_x * off_x + off_y * off_y <= rad * rad;
}

static int is_pressable(int kind)
{
    return kind == INTVSESSION_CTL_KEY || kind == INTVSESSION_CTL_ACTION ||
           kind == INTVSESSION_CTL_DISC || kind == INTVSESSION_CTL_BUTTON;
}

static float corner_radius(int kind)
{
    return kind == INTVSESSION_CTL_KEY ? KEY_R
         : kind == INTVSESSION_CTL_ACTION ? TAB_R : BTN_R;
}

int intvsession_controls_hit(int ecs_on, float x, float y)
{
    int count, idx;
    const intvsession_control *ctl =
        intvsession_controls_layout(ecs_on, &count, NULL, NULL);

    /* the topmost: later entries are drawn over earlier ones */
    for (idx = count - 1; idx >= 0; idx--) {
        const intvsession_control *cur = &ctl[idx];
        if (!is_pressable(cur->kind))
            continue;
        if (cur->kind == INTVSESSION_CTL_DISC) {
            const float rad = cur->w * 0.5f;
            const float off_x = x - (cur->x + rad), off_y = y - (cur->y + rad);
            if (off_x * off_x + off_y * off_y <= rad * rad)
                return idx;
            continue;
        }
        if (in_round_rect(x, y, cur->x, cur->y, cur->w, cur->h,
                          corner_radius(cur->kind)))
            return idx;
    }
    return -1;
}

intvsession_key_mapping intvsession_control_target(const intvsession_control *ctl)
{
    intvsession_key_mapping map = { .kind = INTVSESSION_MAP_NONE };
    if (!ctl)
        return map;
    if (ctl->kind == INTVSESSION_CTL_KEY || ctl->kind == INTVSESSION_CTL_ACTION)
        return intvsession_target_key((intvsession_pad_side)ctl->side,
                                      (intvsession_key)ctl->target);
    if (ctl->kind == INTVSESSION_CTL_BUTTON)
        return intvsession_target_sysaction((intvsession_sysaction)ctl->target);
    return map;
}

int intvsession_controls_disc_dir(const intvsession_control *ctl, float x, float y)
{
    float rad;
    if (!ctl || ctl->kind != INTVSESSION_CTL_DISC)
        return -1;
    rad = ctl->w * 0.5f;
    return intvsession_disc_from_point((double)(x - (ctl->x + rad)),
                                       (double)(y - (ctl->y + rad)), (double)rad);
}

/* ---- the ECS keyboard ----------------------------------------------------------- */

#define ECS_KEY_W    2.0f
#define ECS_PITCH    2.3f
#define ECS_X0       1.0f
#define ECS_Y0       3.4f
#define ECS_W       29.5f
#define ECS_H       15.8f
#define ECS_KEY_R    0.3f

#define MAX_ECS_CAPS 64

typedef struct {
    const char *label;
    const char *shifted;
    int key;
    int flags;
} ecs_spec;

#define KEYSPEC(lbl, sh, k) { (lbl), (sh), INTVSESSION_ECS_KEY_##k, 0 }

/* The shifted legends are the ECS's own layer (see intvsession.h's list of
 * shifted symbols): SHIFT+1 '=', +2 '"', ... +UP '^', +DOWN '?', +LEFT '%',
 * +RIGHT "'", +; ':', +. '>', +, '<'. */
static const ecs_spec row_digits[] = {
    KEYSPEC("1", "=", 1), KEYSPEC("2", "\"", 2), KEYSPEC("3", "#", 3),
    KEYSPEC("4", "$", 4), KEYSPEC("5", "+", 5), KEYSPEC("6", "-", 6),
    KEYSPEC("7", "/", 7), KEYSPEC("8", "*", 8), KEYSPEC("9", "(", 9),
    KEYSPEC("0", ")", 0), KEYSPEC("ESC", NULL, ESC),
};
static const ecs_spec row_qwerty[] = {
    { "CTL", NULL, INTVSESSION_ECS_KEY_CTRL, INTVSESSION_ECSCAP_LATCH },
    KEYSPEC("Q", NULL, Q), KEYSPEC("W", NULL, W), KEYSPEC("E", NULL, E),
    KEYSPEC("R", NULL, R), KEYSPEC("T", NULL, T), KEYSPEC("Y", NULL, Y),
    KEYSPEC("U", NULL, U), KEYSPEC("I", NULL, I), KEYSPEC("O", NULL, O),
    KEYSPEC("P", NULL, P),
};
static const ecs_spec row_asdf[] = {
    KEYSPEC("\xE2\x86\x91", "^", UP),
    KEYSPEC("A", NULL, A), KEYSPEC("S", NULL, S), KEYSPEC("D", NULL, D),
    KEYSPEC("F", NULL, F), KEYSPEC("G", NULL, G), KEYSPEC("H", NULL, H),
    KEYSPEC("J", NULL, J), KEYSPEC("K", NULL, K), KEYSPEC("L", NULL, L),
    KEYSPEC(";", ":", SEMI),
};
static const ecs_spec row_zxcv[] = {
    KEYSPEC("\xE2\x86\x90", "%", LEFT), KEYSPEC("\xE2\x86\x92", "'", RIGHT),
    KEYSPEC("Z", NULL, Z), KEYSPEC("X", NULL, X), KEYSPEC("C", NULL, C),
    KEYSPEC("V", NULL, V), KEYSPEC("B", NULL, B), KEYSPEC("N", NULL, N),
    KEYSPEC("M", NULL, M), KEYSPEC(",", "<", COMMA), KEYSPEC(".", ">", PERIOD),
    { "RTN", NULL, INTVSESSION_ECS_KEY_ENTER, INTVSESSION_ECSCAP_WHITE },
};

typedef struct {
    intvsession_ecs_cap cap[MAX_ECS_CAPS];
    int count;
} ecs_table;

static ecs_table g_ecs;
static int g_ecs_built;

static void ecs_add(ecs_table *tab, int kind, const char *label,
                    const char *shifted, int key, int flags,
                    float x, float y, float w, float h)
{
    intvsession_ecs_cap *cap;
    if (tab->count >= MAX_ECS_CAPS)
        return;
    cap = &tab->cap[tab->count++];
    cap->kind = kind;
    cap->label = label;
    cap->shifted = shifted;
    cap->key = key;
    cap->flags = flags;
    cap->x = x; cap->y = y; cap->w = w; cap->h = h;
}

static void ecs_row(ecs_table *tab, const ecs_spec *row, int cnt, float offset,
                    int line)
{
    int idx;
    for (idx = 0; idx < cnt; idx++)
        ecs_add(tab, INTVSESSION_ECS_KEYCAP, row[idx].label, row[idx].shifted,
                row[idx].key, row[idx].flags,
                ECS_X0 + offset + (float)idx * ECS_PITCH,
                ECS_Y0 + (float)line * ECS_PITCH, ECS_KEY_W, ECS_KEY_W);
}

static void ecs_build(ecs_table *tab)
{
    const float row5 = ECS_Y0 + 4.0f * ECS_PITCH;
    memset(tab, 0, sizeof *tab);
    /* the case: beige-grey with the inset key plate, the label plate at the top
     * left (white INTELLIVISION / COMPUTER KEYBOARD, red script MATTEL
     * ELECTRONICS to its right), ventilation ridges at the top right,
     * the red pinstripe along the lower front edge and the cable stub */
    ecs_add(tab, INTVSESSION_ECS_BODY, "Case", NULL, -1, 0, 0.0f, 0.0f, ECS_W, ECS_H);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Plate", NULL, -1, 0, 0.6f, 3.0f, ECS_W - 1.2f, 11.6f);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Label", "INTELLIVISION|COMPUTER KEYBOARD", -1, 0,
            0.8f, 0.6f, 10.0f, 2.0f);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Mattel", "MATTEL ELECTRONICS", -1, 0,
            11.2f, 1.3f, 5.6f, 0.8f);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Vents", NULL, -1, 0, 17.0f, 0.7f, 11.5f, 1.8f);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Pinstripe", NULL, -1, 0, 0.4f, 15.0f, ECS_W - 0.8f, 0.25f);
    ecs_add(tab, INTVSESSION_ECS_BODY, "Cable", NULL, -1, 0, 27.2f, 0.0f, 1.2f, 0.5f);

    ecs_row(tab, row_digits, (int)(sizeof row_digits / sizeof row_digits[0]), 0.0f, 0);
    ecs_row(tab, row_qwerty, (int)(sizeof row_qwerty / sizeof row_qwerty[0]), 0.5f, 1);
    ecs_row(tab, row_asdf, (int)(sizeof row_asdf / sizeof row_asdf[0]), 0.9f, 2);
    ecs_row(tab, row_zxcv, (int)(sizeof row_zxcv / sizeof row_zxcv[0]), 0.2f, 3);

    /* the bottom row: DOWN, a SHIFT, the long SPACE bar, the other SHIFT */
    ecs_add(tab, INTVSESSION_ECS_KEYCAP, "\xE2\x86\x93", "?", INTVSESSION_ECS_KEY_DOWN, 0,
            ECS_X0 + 1.4f, row5, ECS_KEY_W, ECS_KEY_W);
    ecs_add(tab, INTVSESSION_ECS_KEYCAP, "SHIFT", NULL, INTVSESSION_ECS_KEY_SHIFT,
            INTVSESSION_ECSCAP_LATCH, ECS_X0 + 3.7f, row5, 3.0f, ECS_KEY_W);
    ecs_add(tab, INTVSESSION_ECS_KEYCAP, "SPACE", NULL, INTVSESSION_ECS_KEY_SPACE, 0,
            ECS_X0 + 7.0f, row5, 14.5f, ECS_KEY_W);
    ecs_add(tab, INTVSESSION_ECS_KEYCAP, "SHIFT", NULL, INTVSESSION_ECS_KEY_SHIFT,
            INTVSESSION_ECSCAP_LATCH, ECS_X0 + 21.8f, row5, 3.0f, ECS_KEY_W);
}

const intvsession_ecs_cap *intvsession_ecs_layout(int *count, float *width,
                                                  float *height)
{
    pthread_mutex_lock(&g_lock);
    if (!g_ecs_built) {
        ecs_build(&g_ecs);
        g_ecs_built = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (count)
        *count = g_ecs.count;
    if (width)
        *width = ECS_W;
    if (height)
        *height = ECS_H;
    return g_ecs.cap;
}

int intvsession_ecs_hit(float x, float y)
{
    int count, idx;
    const intvsession_ecs_cap *cap = intvsession_ecs_layout(&count, NULL, NULL);

    for (idx = count - 1; idx >= 0; idx--)
        if (cap[idx].kind == INTVSESSION_ECS_KEYCAP &&
            in_round_rect(x, y, cap[idx].x, cap[idx].y, cap[idx].w, cap[idx].h,
                          ECS_KEY_R))
            return idx;
    return -1;
}
