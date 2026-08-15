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

    /* Stops and restarts the session with intvsession_default_opts()
     * re-read from the settings store -- the "apply" side of a Settings
     * dialog change to ECS/Intellivoice/video standard. Public: SettingsDialog
     * calls this after collecting changes, same shape as the GNOME
     * frontend's intv_window_restart_session. */
    void restartSession();

private:
    void buildMenus();
    void showDebugger();
    void showKeypad();
    void showEcsKeyboard();
    void showSettings();
    void resetToConfig();

    intvsession *m_session;
    DisplayWidget *m_display;
};
