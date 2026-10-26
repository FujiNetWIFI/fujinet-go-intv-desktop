/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>
#include <QTimer>

#include "intvsession.h"

class DisplayWidget;
class QStackedWidget;

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
    void resetGame();
    /* Drains intvsession_sysaction_take -- the one caller that can't fire a
     * sysaction directly is gamepad_sdl.c's SDL thread (queuing RESET_CONFIG
     * so it never joins itself); this, the UI thread, fires each on
     * m_sysactTimer's tick. Lives here rather than on KeypadWindow: that
     * window hides rather than closes but is hidden far more often than
     * shown, and a gamepad-bound sysaction has to keep working regardless. */
    void drainSysactions();
    void importBios(bool folder);
    void importCartToSd();
    void openSdFolder();
    QWidget *buildNoBiosPage();
    /* The picture when the ROMs are there, else the No-BIOS page. */
    void showPage();
    /* (Re)starts the session once its ROMs exist (after an import). */
    void startIfReady();
    void screenshotAndQuit(const QString &path);

    intvsession *m_session;
    DisplayWidget *m_display;
    QStackedWidget *m_stack;
    QWidget *m_noBios;
    QTimer *m_sysactTimer;
};
