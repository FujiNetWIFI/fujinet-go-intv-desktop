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
