/*
 * FujiNet Go Intv -- KDE/Qt frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

#include <cstdio>
#include <cstdlib>

#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"
#include "intvsession.h"
#include "keypad/KeypadWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FujiNet Go Intv"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.intv.kde"));
#ifdef INTV_ICON_FILE
    /* Dev-tree fallback; installed runs resolve the themed icon. */
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("online.fujinet.go.intv.kde"),
        QIcon(QStringLiteral(INTV_ICON_FILE))));
#endif

    intvsession *session = intvsession_new(nullptr);
    if (!session) {
        fprintf(stderr, "fatal: could not create the session\n");
        return 1;
    }
    intvsession_start_opts opts;
    intvsession_default_opts(session, &opts);
    if (intvsession_start(session, &opts) != 0)
        fprintf(stderr, "session start: %s\n",
                intvsession_last_error(session));

    MainWindow win(session);
    win.show();

    /* Developer affordances shared with the GNOME frontend. */
    if (std::getenv("INTV_OPEN_DEBUGGER"))
        DebuggerWindow::showFor(&win, session);
    if (std::getenv("INTV_OPEN_KEYPAD"))
        KeypadWindow::showFor(&win, session);

    const int rc = app.exec();

    intvsession_free(session);
    return rc;
}
