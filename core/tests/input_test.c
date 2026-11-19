/*
 * input_test -- the hand controllers' input sources (core/src/input.c):
 *  - every source is its own hold: a keyboard release never drops a direction
 *    or key a pad is holding, nor the other way round;
 *  - the disc holds ONE position, so its sources are ranked: pointer, then a
 *    pad's buttons / D-pad / hat, then a pad's stick, then the keyboard, the
 *    latest writer winning among equals, with the next source taking over when
 *    the winner lets go;
 *  - a keypad press lights its key and not the disc (held state comes from the
 *    source table, never from the aliased pad byte);
 *  - release_all lets go of the keyboard and the pointer only; a pad's hold
 *    survives, and an unplugged pad lets go of exactly its own;
 *  - the merged state reaches the machine's pad_t, and only on a change.
 * No session and no machine: the sources write the pad_t global directly.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "lzoe/lzoe.h"
#include "file/file.h"
#include "periph/periph.h"
#include "cp1600/cp1600.h"
#include "mem/mem.h"
#include "ecs/ecs.h"
#include "icart/icart.h"
#include "bincfg/bincfg.h"
#include "bincfg/legacy.h"
#include "pads/pads.h"
#include "pads/pads_cgc.h"
#include "pads/pads_intv2pc.h"
#include "avi/avi.h"
#include "gfx/gfx.h"
#include "gfx/palette.h"
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

#include "input_merge.h"
#include "intv_host.h"
#include "intvsession.h"

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass)
        failures++;
}

#define LEFT  INTVSESSION_PAD_LEFT
#define RIGHT INTVSESSION_PAD_RIGHT
#define KEYBIT(key) (1u << (key))

/* the disc codes of intv_host.c: E = 1, N = 4, W = 16, S = 64 */
#define DISC_E 1u
#define DISC_N 4u
#define DISC_W 16u
#define DISC_S 64u

int main(void)
{
    intvsession *none = NULL;

    input_clear_all();

    /* keys: each source its own hold, the merge an OR */
    intvsession_pad_key(none, LEFT, INTVSESSION_KEY_5, 1);
    check(intvsession_buttons_held(none, LEFT) == KEYBIT(INTVSESSION_KEY_5), "keyboard holds 5");
    check(intv.pad0.l[5] == 0x42, "and the machine sees it");
    input_key(INTV_SRC_PAD(0), (int)LEFT, INTVSESSION_KEY_5, 1);
    intvsession_pad_key(none, LEFT, INTVSESSION_KEY_5, 0);
    check(intvsession_buttons_held(none, LEFT) == KEYBIT(INTVSESSION_KEY_5),
          "a key release leaves the pad's hold on the same key");
    check(intv.pad0.l[5] == 0x42, "the machine still sees it");
    input_key(INTV_SRC_PAD(0), (int)LEFT, INTVSESSION_KEY_5, 0);
    check(intvsession_buttons_held(none, LEFT) == 0 && intv.pad0.l[5] == 0, "the pad lets go: released");
    check(intvsession_buttons_held(none, RIGHT) == 0, "the other side never held it");

    /* the disc winner rule: pointer > pad buttons > stick > keyboard */
    check(intvsession_disc_held(none, LEFT) == -1, "no disc held");
    intvsession_pad_disc(none, LEFT, 4);
    check(intvsession_disc_held(none, LEFT) == 4 && intv.pad0.l[15] == DISC_N, "keyboard N");
    input_disc(INTV_SRC_STICK(0), (int)LEFT, 8);
    check(intvsession_disc_held(none, LEFT) == 8 && intv.pad0.l[15] == DISC_W, "the stick beats the keyboard");
    input_disc(INTV_SRC_PAD(0), (int)LEFT, 12);
    check(intvsession_disc_held(none, LEFT) == 12 && intv.pad0.l[15] == DISC_S, "pad buttons beat the stick");
    intvsession_pointer_set(none, LEFT, -1, 0);
    check(intvsession_disc_held(none, LEFT) == 0 && intv.pad0.l[15] == DISC_E, "the pointer beats the pad");
    intvsession_pad_disc(none, LEFT, 2);
    check(intvsession_disc_held(none, LEFT) == 0, "a later keyboard write does not beat the pointer");
    intvsession_release_pointer(none);
    check(intvsession_disc_held(none, LEFT) == 12, "the pointer lets go: the pad takes over");
    input_disc(INTV_SRC_PAD(0), (int)LEFT, -1);
    check(intvsession_disc_held(none, LEFT) == 8, "the pad lets go: the stick takes over");
    input_disc(INTV_SRC_STICK(0), (int)LEFT, -1);
    check(intvsession_disc_held(none, LEFT) == 2, "the stick lets go: the keyboard (its last write) is back");
    intvsession_pad_disc(none, LEFT, -1);
    check(intvsession_disc_held(none, LEFT) == -1 && intv.pad0.l[15] == 0, "all let go: centred");

    /* equals: the latest writer wins; an older one is back when it lets go */
    input_disc(INTV_SRC_PAD(0), (int)LEFT, 4);
    input_disc(INTV_SRC_PAD(1), (int)LEFT, 12);
    check(intvsession_disc_held(none, LEFT) == 12, "two pads: the later write wins");
    input_disc(INTV_SRC_PAD(1), (int)LEFT, -1);
    check(intvsession_disc_held(none, LEFT) == 4, "and the other is back when it lets go");
    input_disc(INTV_SRC_PAD(0), (int)LEFT, -1);

    /* the keyboard's own disc: last write wins inside the source */
    intvsession_pad_disc(none, LEFT, 0);
    intvsession_pad_disc(none, LEFT, 8);
    check(intvsession_disc_held(none, LEFT) == 8, "inside a source the last write wins");
    intvsession_pad_disc(none, LEFT, -1);

    /* a keypad press lights its key and not the disc, whatever the byte says */
    intvsession_pointer_set(none, LEFT, INTVSESSION_KEY_8, -1);
    check(intvsession_buttons_held(none, LEFT) == KEYBIT(INTVSESSION_KEY_8), "pointer holds 8");
    check(intvsession_disc_held(none, LEFT) == -1, "a keypad press does not light the disc");
    check(intv.pad0.l[15] == 0, "nor reach it");
    check(intvsession_target_held(none, intvsession_target_key(LEFT, INTVSESSION_KEY_8)) == 1, "target_held: the key");
    check(intvsession_target_held(none, intvsession_target_key(RIGHT, INTVSESSION_KEY_8)) == 0, "target_held: not the other side");
    /* the pointer is one gesture: it moves to another side's disc */
    intvsession_pointer_set(none, RIGHT, -1, 4);
    check(intvsession_buttons_held(none, LEFT) == 0, "dragging to another handset lets go of the key");
    check(intvsession_disc_held(none, RIGHT) == 4 && intv.pad0.r[15] == DISC_N, "and holds the new side's disc");
    check(intvsession_target_held(none, intvsession_target_disc(RIGHT, 4)) == 1, "target_held: the disc position");
    check(intvsession_target_held(none, intvsession_target_disc(RIGHT, 5)) == 0, "target_held: another position");
    check(intvsession_target_held(none, intvsession_target_sysaction(INTVSESSION_SYSACT_RESET_GAME)) == 0,
          "target_held: a system action is a click, not a hold");
    intvsession_pointer_set(none, RIGHT, -1, -1);
    check(intvsession_disc_held(none, RIGHT) == -1 && intv.pad0.r[15] == 0, "(side, -1, -1) lets go");

    /* release_all: keyboard + pointer only */
    intvsession_pad_key(none, LEFT, INTVSESSION_ACTION_TOP, 1);
    intvsession_pad_disc(none, LEFT, 4);
    intvsession_pointer_set(none, RIGHT, INTVSESSION_KEY_1, 8);
    input_key(INTV_SRC_PAD(2), (int)LEFT, INTVSESSION_KEY_3, 1);
    input_disc(INTV_SRC_STICK(2), (int)LEFT, 0);
    intvsession_release_all(none);
    check(intvsession_buttons_held(none, LEFT) == KEYBIT(INTVSESSION_KEY_3), "release_all: the pad's key stays, the keyboard's is gone");
    check(intvsession_buttons_held(none, RIGHT) == 0 && intvsession_disc_held(none, RIGHT) == -1, "the pointer's holds are gone");
    check(intvsession_disc_held(none, LEFT) == 0, "the keyboard's disc is gone, the pad's stick stays");
    intvsession_pad_key(none, LEFT, INTVSESSION_KEY_1, 1);
    intvsession_pads_clear(none);
    check((intvsession_buttons_held(none, LEFT) & KEYBIT(INTVSESSION_KEY_1)) == 0 &&
          (intvsession_buttons_held(none, LEFT) & KEYBIT(INTVSESSION_KEY_3)) != 0,
          "intvsession_pads_clear is release_all");
    check(intv.pad0.l[3] == 0x21, "the machine kept the pad's key");

    /* an unplugged pad lets go of exactly its own */
    input_pad_clear(2);
    check(intvsession_buttons_held(none, LEFT) == 0 && intvsession_disc_held(none, LEFT) == -1,
          "pad_clear lets go of the pad's buttons and its stick");

    /* only changes reach the machine: a hand-written value is not overwritten by an unchanged merge */
    intv.pad0.l[2] = 0x99;
    input_key(INTV_SRC_PAD(0), (int)LEFT, INTVSESSION_KEY_5, 1);
    check(intv.pad0.l[2] == 0x99, "an unrelated key's state is not rewritten");
    input_key(INTV_SRC_PAD(0), (int)LEFT, INTVSESSION_KEY_5, 0);
    intv.pad0.l[2] = 0;

    /* the ECS pair is two more sides */
    intvsession_pad_key(none, INTVSESSION_PAD_ECS_LEFT, INTVSESSION_KEY_5, 1);
    check(intv.pad1.l[5] == 0x42 && intvsession_buttons_held(none, INTVSESSION_PAD_ECS_LEFT) == KEYBIT(INTVSESSION_KEY_5),
          "side 2 is the ECS pair's left");
    input_clear_all();
    check(intvsession_buttons_held(none, INTVSESSION_PAD_ECS_LEFT) == 0 && intv.pad1.l[5] == 0,
          "clear_all lets go of everything");

    /* bad arguments are ignored */
    intvsession_pad_key(none, (intvsession_pad_side)7, INTVSESSION_KEY_5, 1);
    intvsession_pad_key(none, LEFT, (intvsession_key)99, 1);
    intvsession_pointer_set(none, (intvsession_pad_side)-1, 1, 1);
    check(intvsession_buttons_held(none, (intvsession_pad_side)9) == 0 && intvsession_disc_held(none, (intvsession_pad_side)-3) == -1,
          "out-of-range sides and keys are ignored");

    /* the ECS keyboard's held keys */
    intvsession_ecs_key_set(none, INTVSESSION_ECS_KEY_SHIFT, 1);
    check(intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_SHIFT) == 1 &&
          intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_A) == 0, "a latched ECS SHIFT reads as held");
    intvsession_ecs_key_set(none, INTVSESSION_ECS_KEY_A, 1);
    intvsession_ecs_key_set(none, INTVSESSION_ECS_KEY_A, 0);
    check(intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_A) == 0 &&
          intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_SHIFT) == 1, "a key up leaves the latch");
    intvsession_ecs_keys_clear(none);
    check(intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_SHIFT) == 0, "ecs_keys_clear unlatches");
    check(intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_NONE) == 0, "NONE is never held");
    intvsession_ecs_key_set(none, INTVSESSION_ECS_KEY_SHIFT, 1);
    intvsession_reset_game(none);
    check(intvsession_ecs_key_held(none, INTVSESSION_ECS_KEY_SHIFT) == 0, "reset_game drops a latched ECS key");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("input_test: OK\n");
    return 0;
}
