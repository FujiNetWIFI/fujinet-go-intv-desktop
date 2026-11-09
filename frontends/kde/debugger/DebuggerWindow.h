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
struct intvsrcmap;

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows (creating on first use) the debugger for the session. */
    static void showFor(QWidget *parent, intvsession *session);

private:
    explicit DebuggerWindow(QWidget *parent, intvsession *session);
    ~DebuggerWindow() override;

    /* The 33 ms poll runs only while the window is shown. */
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

    void buildUi();
    void refreshAll();
    void refreshDisasm();
    void refreshRegs();
    void refreshMem();
    void refreshStic();
    void pauseContinue();
    void cardsPage(int delta);
    void loadSymbols();
    void clearSymbols();
    void goToAddr();
    void breakAtAddr();
    void refreshSymbols();
    QWidget *buildBasic();
    void refreshBasic(bool paused);
    void basicReload();
    void basicShowFile(int file);
    void basicScrollToPc();
    void basicPick(int kind);
    void basicImported(const QString &path, int kind);
    void basicToggleAtCursor();
    bool basicTab() const;
    void doStep();
    void doStepOver();
    void doStepOut();
    QWidget *buildFujinet();
    void refreshFujinet(bool force);
    bool fujiShowing() const;

    intvsession *m_session;
    intvdebug *m_dbg;
    intvsymtab *m_symtab;
    intvsrcmap *m_srcmap;

    QLabel *m_status;
    QPushButton *m_pauseBtn;
    QPlainTextEdit *m_disasm;
    int m_disasmBar = -1;     /* the block wearing the current-line bar */

    QLineEdit *m_regEdit[8]; /* R0-R7 */
    QLabel *m_flags;

    QLineEdit *m_memAddr;
    QPlainTextEdit *m_memView;
    uint16_t m_memBase = 0;
    QLineEdit *m_breakAddr;

    QListWidget *m_symList;
    QLineEdit *m_symFilter;
    QLabel *m_symCount;
    unsigned m_symSeenGen = 0;
    QString m_symFilterText;

    QLabel *m_backtab, *m_mob[INTVSTIC_MOB_COUNT], *m_cards, *m_palette;
    QLabel *m_cardsRange;
    QPlainTextEdit *m_mobInfo;
    QPlainTextEdit *m_sticState;
    int m_cardFirst = 0;
    int m_cardTotal = 0;

    /* BASIC tab: the imported IntyBASIC source (core/debugger/srcmap.h) */
    QLabel *m_basNote;
    QComboBox *m_basFiles;
    QPlainTextEdit *m_basView;
    int m_basFile = -1;       /* index into the map; -1 = nothing shown */
    unsigned m_basGen = 0;    /* intvsrcmap_generation() the view was built from */
    int m_basBar = -1;        /* the block wearing the current-line bar */
    int m_basPcLine = 0;      /* the machine's line in the file shown, 0 = none */
    bool m_basGuard = false;  /* we are moving the cursor, not the user */

    QTabWidget *m_tabs;
    QWidget *m_fujiPage;
    QLabel *m_fujiNote;
    QPlainTextEdit *m_fujiText, *m_fujiState;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
    int m_fujiTicks = 0;

    QTimer *m_tick;
    uint64_t m_seenSerial = 0;
    int m_wasPaused = 0;
};
