/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * plain QWidget, letterboxed to 4:3 -- NOT the frame buffer's own raw pixel
 * ratio (160/200 = 0.8, portrait): the STIC's pixels are not square, and
 * real Intellivision video (like jzIntv's own default display) fills a
 * standard NTSC 4:3 picture. Matches the reasoning in frontends/gnome/
 * display.c exactly. A QTimer poll drives the repaint (there is no
 * intvsession_notify_vsync to feed -- unlike CoCo/MSX, intv_host.c's own
 * paced thread already governs itself to NTSC/PAL speed independent of any
 * frontend, see frontends/gnome/display.c's own comment on this), so
 * there is nothing a QOpenGLWidget's frameSwapped signal would buy here --
 * plain QWidget avoids depending on the separate OpenGLWidgets Qt module
 * for no benefit.
 *
 * Key events are translated to intvsession.h's own private
 * INTVSESSION_KEYSYM_* numbering, NOT an X11/GDK keysym -- see
 * frontends/gnome/keysym_map.h's header comment for why that distinction
 * matters (a real bug was caught there: passing a toolkit's native keyval
 * straight through silently drops every arrow/numpad/modifier key while
 * ASCII letters keep working by coincidence). Both press and release are
 * forwarded: jzIntv's pad_t tracks held keys via direct bit injection, so
 * a missed release leaves that key down in the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include "KeyForward.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>

DisplayWidget::DisplayWidget(intvsession *session, QWidget *parent)
    : QWidget(parent),
      m_session(session),
      m_frame((size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT, 0)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT);
    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] { update(); });
    m_tick->start(16); /* ~60Hz repaint poll */
}

QRectF DisplayWidget::destRect() const
{
    const qreal w = width(), h = height();
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

void DisplayWidget::paintEvent(QPaintEvent *)
{
    uint64_t serial = m_serial;

    if (intvsession_copy_frame(m_session, m_frame.data(), &serial))
        m_serial = serial;

    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    const QImage frame((const uchar *)m_frame.data(), INTVSESSION_FB_WIDTH,
                       INTVSESSION_FB_HEIGHT, INTVSESSION_FB_WIDTH * 4,
                       QImage::Format_RGB32);
    p.drawImage(destRect(), frame);
}

bool DisplayWidget::event(QEvent *event)
{
    /* Claim every key except the reserved chords (F9 keypad, F11
     * fullscreen, F12 debugger) while focused, so menu accelerators
     * cannot shadow what the machine should receive. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F9 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void DisplayWidget::forwardKey(const QKeyEvent *event, int down)
{
    intvForwardKey(m_session, event, down);
}

void DisplayWidget::focusOutEvent(QFocusEvent *event)
{
    /* Losing keyboard focus shouldn't leave a key stuck down in whichever
     * matrix was active -- see intvsession_ecs_keys_clear's own comment. */
    intvsession_ecs_keys_clear(m_session);
    QWidget::focusOutEvent(event);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 1);
    event->accept();
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 0);
    event->accept();
}
