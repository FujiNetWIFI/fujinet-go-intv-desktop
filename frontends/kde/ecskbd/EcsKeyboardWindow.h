/*
 * EcsKeyboardWindow: the clickable ECS keyboard sub-window, so a mouse
 * alone can drive every key of the ECS's own 7x8 scan-matrix keyboard --
 * the on-screen counterpart to the hand-controller KeypadWindow.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QWidget>

#include "intvsession.h"

class QLabel;

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. */
class EcsKeyboardWindow : public QWidget {
    Q_OBJECT
public:
    static void showFor(QWidget *parent, intvsession *session);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    explicit EcsKeyboardWindow(QWidget *parent, intvsession *session);

    QWidget *buildKeyboard();
    void forwardKey(const QKeyEvent *event, int down);
    void releaseAll();

    intvsession *m_session;
    QLabel *m_notice;
};
