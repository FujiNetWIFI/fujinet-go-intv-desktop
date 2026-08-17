/*
 * Debugger window for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "intvdebug.h"
#include "intvsession.h"
#include "intvstic.h"

struct intvsymtab;

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows (creating on first use) the debugger for the session. */
    static void showFor(QWidget *parent, intvsession *session);

private:
    explicit DebuggerWindow(QWidget *parent, intvsession *session);
    ~DebuggerWindow() override;

    void buildUi();
    void refreshAll();
    void refreshDisasm();
    void refreshRegs();
    void refreshMem();
    void refreshStic();
    void pauseContinue();
    void cardsPage(int delta);

    intvsession *m_session;
    intvdebug *m_dbg;
    intvsymtab *m_symtab;

    QLabel *m_status;
    QPushButton *m_pauseBtn;
    QPlainTextEdit *m_disasm;

    QLineEdit *m_regEdit[8]; /* R0-R7 */
    QLabel *m_flags;

    QLineEdit *m_memAddr;
    QPlainTextEdit *m_memView;
    uint16_t m_memBase = 0;

    QLabel *m_backtab, *m_mob[INTVSTIC_MOB_COUNT], *m_cards, *m_palette;
    QLabel *m_cardsRange;
    QPlainTextEdit *m_mobInfo;
    QPlainTextEdit *m_sticState;
    int m_cardFirst = 0;
    int m_cardTotal = 0;

    QTimer *m_tick;
    uint64_t m_seenSerial = 0;
    int m_wasPaused = 0;
};
