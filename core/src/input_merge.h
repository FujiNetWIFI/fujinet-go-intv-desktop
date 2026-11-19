/*
 * input_merge -- every way of holding a hand-controller input is a separate
 * SOURCE, and what the machine sees is their merge. So letting go of one
 * source (a key release, the Controllers window's pointer, a pad that is
 * unplugged) can never cancel another's hold.
 *
 * Sources: the host keyboard, the Controllers window's pointer, and for each
 * connected pad (by its stable owner number, 0 to INTV_MAX_PADS - 1) its
 * buttons / D-pad / hat and its analog stick separately.
 *
 * Per side the 15 keys (keypad and action buttons) merge as a plain OR. The
 * disc holds ONE position, so its sources are ranked: pointer, then a pad's
 * buttons / D-pad / hat, then a pad's stick, then the keyboard; among sources
 * of one rank the one that set its position last wins. A source that lets go
 * falls back to the next one holding a position. The merged state is handed to
 * intv_host_pad_key / intv_host_pad_disc only when it changes.
 *
 * All of it is process-global under one leaf mutex (jzIntv's machine is a
 * process singleton). This header is not input.h on purpose: macOS file
 * systems ignore case.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTV_INPUT_MERGE_H
#define INTV_INPUT_MERGE_H

#include "intvsession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INTV_MAX_PADS 8
#define INTV_SIDES 4

#define INTV_SRC_KEYBOARD 0
#define INTV_SRC_POINTER  1
#define INTV_SRC_PAD0     2                                  /* + owner: buttons, D-pad, hat */
#define INTV_SRC_STICK0   (INTV_SRC_PAD0 + INTV_MAX_PADS)    /* + owner: the analog stick */
#define INTV_SRC_COUNT    (INTV_SRC_STICK0 + INTV_MAX_PADS)
#define INTV_SRC_PAD(owner)   (INTV_SRC_PAD0 + (owner))
#define INTV_SRC_STICK(owner) (INTV_SRC_STICK0 + (owner))

/* Hold or release one key (an intvsession_key) on a side for a source. */
void input_key(int src, int side, int key, int down);

/* Set a source's whole key mask (1 << intvsession_key) on a side at once. */
void input_keys_set(int src, int side, unsigned mask);

/* Set a source's disc position on a side: 0-15, or -1 for none. Setting a
 * position stamps the source, so it wins over an equal-rank source set
 * earlier. */
void input_disc(int src, int side, int dir);

/* Let go of everything one source holds, on every side. */
void input_clear_source(int src);

/* Let go of everything a connected pad holds (its buttons' and its stick's
 * sources). Called when a pad goes away or changes side. */
void input_pad_clear(int owner);

/* Let go of every source, and forget what was forwarded (the machine has just
 * had its pads reset under us: session start / stop / reset game). */
void input_clear_all(void);

/* The merge. */
unsigned input_keys_merged(int side);
int      input_disc_merged(int side);

/* The ECS keyboard matrix's held keys (intvsession_ecs_key values). */
void input_ecs_set(int key, int down);
int  input_ecs_held(int key);
void input_ecs_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* INTV_INPUT_MERGE_H */
