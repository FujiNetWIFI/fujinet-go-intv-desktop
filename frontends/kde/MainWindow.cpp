/*
 * MainWindow: menu bar over the emulator display. Plain Qt6 Widgets, no KDE
 * Frameworks -- Breeze arrives through the platform theme, and staying
 * framework-free keeps this frontend reusable elsewhere.
 *
 * No machine-selection menu (unlike the MSX port's MainWindow): the
 * Intellivision has one configuration, matching the GNOME frontend's own
 * window.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"
#include "ecskbd/EcsKeyboardWindow.h"
#include "keypad/KeypadWindow.h"

#ifndef INTV_VERSION_STRING
#define INTV_VERSION_STRING "0.0.0"
#endif

MainWindow::MainWindow(intvsession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Intv"));
    resize(800, 650);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    statusBar();

    buildMenus();
    m_display->setFocus();

    /* ~100ms: fast enough that a gamepad press feels immediate, cheap
     * enough to run for the app's whole lifetime. */
    m_sysactTimer = new QTimer(this);
    connect(m_sysactTimer, &QTimer::timeout, this,
           &MainWindow::drainSysactions);
    m_sysactTimer->start(100);
}

void MainWindow::drainSysactions()
{
    intvsession_sysaction a;
    while (intvsession_sysaction_take(m_session, &a)) {
        if (intvsession_sysaction_fire(m_session, a) != 0)
            statusBar()->showMessage(
                QString::fromUtf8(intvsession_last_error(m_session)), 5000);
    }
}

void MainWindow::showDebugger()
{
    DebuggerWindow::showFor(this, m_session);
}

void MainWindow::showKeypad()
{
    KeypadWindow::showFor(this, m_session);
}

void MainWindow::showEcsKeyboard()
{
    EcsKeyboardWindow::showFor(this, m_session);
}

void MainWindow::restartSession()
{
    intvsession_start_opts opts;
    intvsession_stop(m_session);
    intvsession_default_opts(m_session, &opts);
    if (intvsession_start(m_session, &opts) != 0)
        statusBar()->showMessage(
            QString::fromUtf8(intvsession_last_error(m_session)), 5000);
}

void MainWindow::resetToConfig()
{
    if (intvsession_reset_to_config(m_session) != 0)
        statusBar()->showMessage(
            QString::fromUtf8(intvsession_last_error(m_session)), 5000);
    else
        statusBar()->showMessage(QStringLiteral("Reset to CONFIG"), 5000);
    m_display->setFocus();
}

void MainWindow::resetGame()
{
    if (intvsession_reset_game(m_session) != 0)
        statusBar()->showMessage(
            QString::fromUtf8(intvsession_last_error(m_session)), 5000);
    else
        statusBar()->showMessage(QStringLiteral("Reset Game"), 5000);
    m_display->setFocus();
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(m_session, this);
    dlg.exec();
    if (dlg.sessionDirty()) {
        restartSession();
        statusBar()->showMessage(
            QStringLiteral("Machine options applied (session restarted)"),
            5000);
    }
    m_display->setFocus();
}

void MainWindow::buildMenus()
{
    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Keypad"), QKeySequence(Qt::Key_F9), this,
                    &MainWindow::showKeypad);
    view->addAction(QStringLiteral("ECS &Keyboard"), QKeySequence(Qt::Key_F10),
                    this, &MainWindow::showEcsKeyboard);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, &MainWindow::showDebugger);

    QMenu *fujinet = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    /* No QKeySequence here, deliberately: Backspace is a *remappable*
     * default (core/src/bindings.c), not a fixed accelerator like Ctrl+R
     * below. Binding one here would fire Reset Game on plain Backspace
     * forever, even after a keypad window Map-mode remap moved that action
     * to a different key -- DisplayWidget claims the ShortcutOverride for
     * everything except F9/F11/F12 specifically so menu accelerators can't
     * shadow what the machine should receive (see its own header), but a
     * KeypadWindow with focus doesn't, so this would have raced the
     * bindings table there. The mouse-clickable menu entry is enough; the
     * real trigger is intvForwardKey's MAP_SYSACT dispatch (KeyForward.cpp),
     * which honours whatever the user has actually bound. */
    fujinet->addAction(QStringLiteral("Reset &Game (Backspace)"), this,
                       &MainWindow::resetGame);
    fujinet->addAction(QStringLiteral("&Reset to CONFIG"),
                       QKeySequence(Qt::CTRL | Qt::Key_R), this,
                       &MainWindow::resetToConfig);
    fujinet->addAction(QStringLiteral("&Configuration…"), this,
                       [this] { fujinet_config_show(this, m_session); });
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       [this] { fujinet_log_show(this, m_session); });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("&Settings"));
    settings->addAction(QStringLiteral("&Settings…"), this,
                        &MainWindow::showSettings);

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp,
                    &QApplication::quit);
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("FujiNet Go Intv"),
            QStringLiteral(
                "<b>FujiNet Go Intv</b> %1<br><br>"
                "Self-contained Mattel Intellivision with built-in "
                "FujiNet.<br>"
                "Emulation by jzIntv; FujiNet joins over Bus-Over-IP "
                "(BoIP).<br><br>"
                "Copyright © 2026 Thomas Cherryhomes<br>"
                "GPL-3.0-or-later &middot; "
                "<a href=\"https://fujinet.online/\">fujinet.online</a>")
                .arg(QStringLiteral(INTV_VERSION_STRING)));
    });
}
