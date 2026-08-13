/*
 * DisplayWidget: the emulator video widget for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QWidget>

#include <vector>

#include "intvsession.h"

class QTimer;

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(intvsession *session, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;

private:
    void forwardKey(const QKeyEvent *event, int down);
    QRectF destRect() const;

    intvsession *m_session;
    /* The session writes tightly-packed XRGB8888 rows, so the frame is kept
     * in a plain buffer and QImage is only ever a view over it. On a
     * little-endian host XRGB8888 is exactly QImage::Format_RGB32. Fixed
     * size (INTVSESSION_FB_WIDTH x _HEIGHT) -- the STIC's output does not
     * vary at runtime, unlike the CoCo/MSX targets' own DisplayWidgets.
     *
     * Plain QWidget + a QTimer poll, not QOpenGLWidget's frameSwapped --
     * unlike CoCo/MSX, intvsession has no notify_vsync to feed (jzIntv's
     * own paced thread already governs itself to NTSC/PAL speed
     * independent of the frontend; see frontends/gnome/display.c's own
     * comment on this), so there is nothing an OpenGL swap signal would
     * buy here, and one fewer Qt component (OpenGLWidgets) to depend on. */
    std::vector<uint32_t> m_frame;
    uint64_t m_serial = 0;
    QTimer *m_tick;
};
