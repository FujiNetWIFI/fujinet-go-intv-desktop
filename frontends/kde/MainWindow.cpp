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
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
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
    m_noBios = buildNoBiosPage();
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_display);
    m_stack->addWidget(m_noBios);
    setCentralWidget(m_stack);
    statusBar();

    buildMenus();
    showPage();

    /* ~100ms: fast enough that a gamepad press feels immediate, cheap
     * enough to run for the app's whole lifetime. */
    m_sysactTimer = new QTimer(this);
    connect(m_sysactTimer, &QTimer::timeout, this,
           &MainWindow::drainSysactions);
    m_sysactTimer->start(100);

    /* For the smoke tests: picture every open window, then quit. */
    const QString shot = qEnvironmentVariable("INTV_SCREENSHOT");
    if (!shot.isEmpty()) {
        bool ok = false;
        int delay = qEnvironmentVariableIntValue("INTV_SCREENSHOT_DELAY_MS", &ok);
        if (!ok)
            delay = 3000;
        QTimer::singleShot(delay, this,
                           [this, shot] { screenshotAndQuit(shot); });
    }
}

/* INTV_SCREENSHOT=<file.png>: this window to <file.png>, every other visible
 * top-level window to <stem>-<title>.png, then quit. The app's own widgets
 * are grabbed, never the screen. */
void MainWindow::screenshotAndQuit(const QString &path)
{
    grab().save(path);
    const QString base = path.endsWith(QStringLiteral(".png"),
                                       Qt::CaseInsensitive)
                             ? path.chopped(4) : path;
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == this || !w->isVisible())
            continue;
        QString name = w->windowTitle().isEmpty()
                           ? QStringLiteral("window%1").arg(++n)
                           : w->windowTitle();
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                     QStringLiteral("_"));
        w->grab().save(QStringLiteral("%1-%2.png").arg(base, name));
    }
    if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        modal->reject();
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

/* "BIOS images and ROM sets (*.zip *.ZIP ...);;All files (*)": either case. */
static QString romFilter()
{
    return QStringLiteral("BIOS images and ROM sets (*.zip *.ZIP *.rom *.ROM "
                          "*.bin *.BIN);;All files (*)");
}

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all, and the machine starts if it now can. */
void MainWindow::importBios(bool folder)
{
    QStringList paths;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Import BIOS from a Folder"));
        if (!dir.isEmpty())
            paths << dir;
    } else {
        paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Import BIOS"), QString(), romFilter());
    }
    QStringList log;
    int imported = 0;
    for (const QString &p : paths) {
        char msg[1024] = "";
        if (intvsession_rom_import(m_session, p.toUtf8().constData(), msg,
                                   sizeof msg) == 0)
            imported++;
        log << QString::fromUtf8(msg);
    }
    if (!log.isEmpty())
        QMessageBox::information(
            this,
            imported ? QStringLiteral("BIOS Imported")
                     : QStringLiteral("Nothing Imported"),
            log.join(QLatin1Char('\n')));
    startIfReady();
}

/* In place of the picture until the ROMs are there. */
QWidget *MainWindow::buildNoBiosPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addStretch(1);
    auto *title = new QLabel(QStringLiteral("No Intellivision BIOS"));
    QFont tf = title->font();
    tf.setPointSizeF((tf.pointSizeF() > 0 ? tf.pointSizeF() : 10) * 1.8);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    auto *body = new QLabel(QStringLiteral(
        "The Intellivision Executive and Graphics ROMs are not part of the "
        "app. Import the ones you have \u2014 exec.bin and grom.bin, a zip "
        "such as MAME's intv set, or a folder \u2014 and the Intellivision "
        "starts."));
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setFixedWidth(520);
    body->setMinimumHeight(body->heightForWidth(520));
    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import BIOS\u2026"));
    auto *folder = new QPushButton(QStringLiteral("Import a Folder\u2026"));
    files->setDefault(true);
    connect(files, &QPushButton::clicked, this, [this] { importBios(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importBios(true); });
    row->addStretch(1);
    row->addWidget(files);
    row->addWidget(folder);
    row->addStretch(1);
    v->addWidget(title);
    v->addSpacing(8);
    v->addWidget(body, 0, Qt::AlignHCenter);
    v->addSpacing(16);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

void MainWindow::showPage()
{
    const bool bios = intvsession_has_system_roms(m_session) != 0;
    m_stack->setCurrentWidget(bios ? static_cast<QWidget *>(m_display)
                                   : m_noBios);
    if (bios)
        m_display->setFocus();
}

void MainWindow::startIfReady()
{
    if (intvsession_has_system_roms(m_session))
        restartSession();
    showPage();
}

void MainWindow::importCartToSd()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import Cartridge to SD"), QString(),
        QStringLiteral("Intellivision cartridges (*.rom *.ROM *.bin *.BIN "
                       "*.int *.INT *.itv *.ITV *.cfg *.CFG *.zip *.ZIP);;"
                       "All files (*)"));
    int n = 0;
    QString first;
    for (const QString &f : files) {
        char dest[1024] = "";
        if (intvsession_import_cart_to_sd(m_session, f.toUtf8().constData(),
                                          dest, sizeof dest) == 0) {
            if (n++ == 0)
                first = QFileInfo(f).fileName();
        } else {
            QMessageBox::warning(
                this, QStringLiteral("Import failed"),
                QString::fromUtf8(intvsession_last_error(m_session)));
        }
    }
    if (n == 1)
        statusBar()->showMessage(
            QStringLiteral("%1 is on the SD host \u2014 boot it from the "
                           "CONFIG client").arg(first), 8000);
    else if (n > 1)
        statusBar()->showMessage(
            QStringLiteral("%1 files are on the SD host \u2014 boot them "
                           "from the CONFIG client").arg(n), 8000);
}

void MainWindow::openSdFolder()
{
    const QString dir = QString::fromUtf8(intvsession_sd_path(m_session));
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
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
    if (intvsession_has_system_roms(m_session) &&
        intvsession_start(m_session, &opts) != 0)
        statusBar()->showMessage(
            QString::fromUtf8(intvsession_last_error(m_session)), 5000);
    showPage();
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
    connect(&dlg, &SettingsDialog::romsImported, this,
            &MainWindow::startIfReady);
    dlg.exec();
    if (dlg.sessionDirty()) {
        restartSession();
        statusBar()->showMessage(
            QStringLiteral("Machine options applied (session restarted)"),
            5000);
    }
    showPage();
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Import BIOS…"), this,
                       [this] { importBios(false); });
    machine->addAction(QStringLiteral("Import BIOS &from a Folder…"), this,
                       [this] { importBios(true); });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Import Cartridge to SD…"), this,
                       &MainWindow::importCartToSd);
    machine->addAction(QStringLiteral("Open &SD Folder"), this,
                       &MainWindow::openSdFolder);

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
    fujinet->addAction(QStringLiteral("&Configuration…"), this, [this] {
        if (intvsession_fujinet_external(m_session)) {
            statusBar()->showMessage(
                QStringLiteral("FujiNet is external: use its own web UI"),
                5000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fujinet->addAction(QStringLiteral("Console &Log…"), this, [this] {
        if (intvsession_fujinet_external(m_session)) {
            statusBar()->showMessage(
                QStringLiteral("FujiNet is external: see its own console"),
                5000);
            return;
        }
        fujinet_log_show(this, m_session);
    });

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
