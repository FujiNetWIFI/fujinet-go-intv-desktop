/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "intvsession.h"

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(intvsession *session, QWidget *parent = nullptr);

private:
    void buildMenus();
    void showDebugger();
    void showKeypad();

    intvsession *m_session;
    DisplayWidget *m_display;
};
