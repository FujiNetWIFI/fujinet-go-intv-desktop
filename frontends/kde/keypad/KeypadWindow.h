/*
 * KeypadWindow: the clickable keypad sub-window, showing both hand
 * controllers side by side so a mouse alone can drive every button and
 * direction disc on the Intellivision controller.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QWidget>

#include "intvsession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. */
class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    static void showFor(QWidget *parent, intvsession *session);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    explicit KeypadWindow(QWidget *parent, intvsession *session);

    QWidget *buildController(intvsession_pad_side side, const QString &title);
    void forwardKey(const QKeyEvent *event, int down);

    intvsession *m_session;
};
