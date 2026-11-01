/*
 * The software display: the fallback for a machine with no usable GPU
 * device. A QWidget that scales with QPainter, fed by FrameFeed like the GPU
 * one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFocusEvent>
#include <QKeyEvent>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "intvsession.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(intvsession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    intvsession *m_session;
    FrameFeed *m_feed;
    /* XRGB8888 rows written by the session; paintEvent views them as
     * Format_RGB32 (which ignores the X byte when drawn opaque). */
    std::vector<uint32_t> m_frame;
    uint64_t m_serial = 0;
};
