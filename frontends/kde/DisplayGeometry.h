/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display: letterboxed to 4:3 and centred. NOT the frame buffer's
 * raw pixel ratio (160/200 = 0.8, portrait) -- the STIC's pixels are not
 * square, and real Intellivision video (like jzIntv's own default display)
 * fills a standard NTSC 4:3 picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QEvent>
#include <QKeyEvent>
#include <QRectF>
#include <QSizeF>
#include <Qt>

inline QRectF displayRect(QSizeF avail)
{
    const qreal w = avail.width(), h = avail.height();
    constexpr qreal aspect = 4.0 / 3.0;
    qreal dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

/* Claim every key except the reserved chords (F9 keypad, F11 fullscreen, F12
 * debugger) while focused, so menu accelerators cannot shadow what the
 * machine should receive. */
inline bool claimShortcut(QEvent *event)
{
    if (event->type() != QEvent::ShortcutOverride)
        return false;
    const int k = static_cast<QKeyEvent *>(event)->key();
    if (k == Qt::Key_F9 || k == Qt::Key_F11 || k == Qt::Key_F12)
        return false;
    event->accept();
    return true;
}
