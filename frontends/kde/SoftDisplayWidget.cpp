/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"
#include "KeyForward.h"

#include <QEvent>
#include <QImage>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(intvsession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_frame((size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT, 0)
{
    setMinimumSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

bool SoftDisplayWidget::event(QEvent *e)
{
    if (claimShortcut(e))
        return true;
    return QWidget::event(e);
}

void SoftDisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        intvForwardKey(m_session, event, 1);
    event->accept();
}

void SoftDisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        intvForwardKey(m_session, event, 0);
    event->accept();
}

void SoftDisplayWidget::focusOutEvent(QFocusEvent *event)
{
    intvsession_ecs_keys_clear(m_session);
    QWidget::focusOutEvent(event);
}

void SoftDisplayWidget::onFrame()
{
    if (!intvsession_copy_frame(m_session, m_frame.data(), &m_serial))
        return;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    /* 0x00RRGGBB from the core: draw as RGB32 (opaque, X ignored) */
    const QImage frame(reinterpret_cast<const uchar *>(m_frame.data()), INTVSESSION_FB_WIDTH,
                       INTVSESSION_FB_HEIGHT, INTVSESSION_FB_WIDTH * 4, QImage::Format_RGB32);
    p.drawImage(displayRect(QSizeF(size())), frame);
}
