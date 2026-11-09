/*
 * Debugger window (Qt6): disassembly with click-to-toggle breakpoints,
 * editable registers, a memory view, and an STIC tab (BACKTAB, all 8 MOBs,
 * the GRAM/GROM card sheet with pager, palette, decoded register/mode
 * text) -- the same content as the GNOME debugger window
 * (frontends/gnome/debugger/dbg_window.c), built from plain Qt6 Widgets.
 *
 * A FujiNet tab, last, shows every FujiBus packet between the machine and
 * FujiNet (intvdebug_fujinet_trace, always on) beside the mailbox's state;
 * the timer also refreshes it twice a second while it shows, since it is
 * the one view that changes while the machine runs.
 *
 * Refreshing is a POLL (a QTimer watching intvdebug_stop_serial), not a
 * callback: intvdebug.h has no stop-callback mechanism at all (unlike the
 * MSX port's msxdebug_set_stop_callback) -- the engine stops the machine
 * synchronously inside cp1600_instr_tick on the emulator thread, and a UI
 * watches intvdebug_stop_serial()/intvdebug_is_paused() on its own timer
 * instead. Same reasoning the GNOME port's own header documents.
 *
 * The symbol table (core/debugger/symbols.h) is not owned by intvdebug --
 * unlike msxdebug's own -- so this window uses the process-wide
 * intvsymtab_shared() instead of its own instance: a cart arrives through
 * FujiNet CONFIG with no path a .sym could ever be derived from, so a user
 * loading one by hand is the only way symbols get here at all, and that
 * work should survive closing this window, not be thrown away with it. It
 * ships pre-seeded with EXEC ROM/RAM and cart-header symbols (see
 * symbols.c); "Load Symbols..." on the toolbar loads a per-game as1600
 * .sym file on top (e.g. one of the FujiNet Intellivision ports'
 * build/GAME_net.sym), matching the GNOME port's own UI.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "../help/HelpWindow.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "srcmap.h"
#include "symbols.h"

namespace {
constexpr int kDisasmLines = 32;
constexpr int kMemWordsPerRow = 8;
constexpr int kMemRows = 16;
constexpr int kCardRowsPerPage = 8; /* 128 cards/page at CARDS_PER_ROW=16 */
constexpr int kFujiTicks = 15;      /* the FujiNet tab, 500 ms at 33 ms */

/* The tabs, in order; INTV_DEBUGGER_TAB picks one by number or by the start
 * of its name (any case), for tests and captures. */
const char *const kTabNames[] = {"CPU", "BASIC", "STIC", "Symbols", "FujiNet"};
enum { kTabCpu, kTabBasic, kTabStic, kTabSymbols, kTabFujinet };
enum { kPickFile, kPickFolder, kPickMap };
constexpr int kTabCount = int(sizeof(kTabNames) / sizeof(kTabNames[0]));

int tabFromEnv()
{
    const char *tab = std::getenv("INTV_DEBUGGER_TAB");
    if (!tab || !*tab)
        return -1;
    char *end;
    const long n = std::strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < kTabCount ? int(n) : -1;
    for (int i = 0; i < kTabCount; i++)
        if (qstrnicmp(tab, kTabNames[i], uint(std::strlen(tab))) == 0)
            return i;
    return -1;
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

QLabel *picLabel(int w, int h)
{
    auto *l = new QLabel;
    l->setFixedSize(w, h);
    return l;
}

/* All intvstic_render_* outputs are RGBA8888, alpha always 0xFF except
 * intvstic_render_mob (which uses alpha for transparency) -- both are the
 * same memory layout QImage::Format_RGBA8888 expects either way. */
void setRgba(QLabel *label, const uint8_t *rgba, int w, int h)
{
    label->setPixmap(QPixmap::fromImage(
        QImage(rgba, w, h, w * 4, QImage::Format_RGBA8888)));
}

QPointer<DebuggerWindow> g_singleton;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, intvsession *session)
{
    if (g_singleton) {
        g_singleton->raise();
        g_singleton->activateWindow();
        return;
    }
    g_singleton = new DebuggerWindow(parent, session);
    g_singleton->show();
}

DebuggerWindow::DebuggerWindow(QWidget *parent, intvsession *session)
    : QMainWindow(parent), m_session(session)
{
    m_dbg = intvdebug_get();
    m_symtab = intvsymtab_shared();
    m_srcmap = intvsrcmap_shared();
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Intellivision Debugger"));
    resize(1080, 780);
    buildUi();
    if (tabFromEnv() >= 0)
        m_tabs->setCurrentIndex(tabFromEnv());

    /* For captures and tests, like INTV_DEBUGGER_TAB: INTV_BASIC_IMPORT=<.bas
     * file or folder> imports it as Import File / Import Folder would, and
     * INTV_BASIC_BREAK=<name.bas>:<line> sets a breakpoint on that line. */
    if (const char *imp = std::getenv("INTV_BASIC_IMPORT"); imp && *imp)
        basicImported(QString::fromUtf8(imp), kPickFile);
    if (const char *brk = std::getenv("INTV_BASIC_BREAK"); brk && std::strchr(brk, ':')) {
        const QString spec = QString::fromUtf8(brk);
        const QString name = spec.section(QLatin1Char(':'), 0, 0);
        const int line = spec.section(QLatin1Char(':'), 1).toInt();
        for (int f = 0; f < intvsrcmap_file_count(m_srcmap); f++) {
            const int addr = intvsrcmap_first_addr(m_srcmap, f, line);
            if (QString::fromUtf8(intvsrcmap_file_name(m_srcmap, f)) == name && addr >= 0 &&
                !intvdebug_breakpoint_is_set(m_dbg, uint16_t(addr)))
                intvdebug_breakpoint_toggle(m_dbg, uint16_t(addr));
        }
    }

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(m_dbg, 1);
    intvdebug_pause(m_dbg);

    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] {
        const uint64_t serial = intvdebug_stop_serial(m_dbg);
        if (serial != m_seenSerial ||
            intvdebug_is_paused(m_dbg) != m_wasPaused) {
            m_seenSerial = serial;
            refreshAll();
        } else if (fujiShowing() && ++m_fujiTicks >= kFujiTicks) {
            /* the one view that moves while the machine runs */
            refreshFujinet(false);
        }
    });
    refreshAll();   /* the poll itself starts in showEvent */
}

void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_tick->start(33);
    refreshAll();   /* whatever moved while it was hidden */
}

void DebuggerWindow::hideEvent(QHideEvent *e)
{
    QMainWindow::hideEvent(e);
    m_tick->stop();
}

DebuggerWindow::~DebuggerWindow()
{
    /* Releasing costs the machine nothing further (see intvdebug.h) and
     * leaving it engaged+paused would look like the emulator had hung. */
    intvdebug_set_engaged(m_dbg, 0);
    /* m_symtab is intvsymtab_shared() -- process-wide, not owned by this
     * window -- so it is deliberately NOT destroyed here; see this file's
     * header comment. */
}

void DebuggerWindow::buildUi()
{
    auto *tb = addToolBar(QStringLiteral("Control"));
    tb->setMovable(false);

    m_pauseBtn = new QPushButton(QStringLiteral("Pause (F5)"));
    connect(m_pauseBtn, &QPushButton::clicked, this,
            &DebuggerWindow::pauseContinue);
    tb->addWidget(m_pauseBtn);

    /* On the BASIC tab the three steppers go by BASIC line. */
    auto *step = new QPushButton(QStringLiteral("Step (F7)"));
    step->setToolTip(QStringLiteral("One instruction; on the BASIC tab, one BASIC line"));
    connect(step, &QPushButton::clicked, this, [this] { doStep(); });
    tb->addWidget(step);
    auto *stepOver = new QPushButton(QStringLiteral("Step Over (F8)"));
    stepOver->setToolTip(QStringLiteral("Over a subroutine call or GOSUB"));
    connect(stepOver, &QPushButton::clicked, this, [this] { doStepOver(); });
    tb->addWidget(stepOver);
    auto *stepOut = new QPushButton(QStringLiteral("Step Out (Shift+F8)"));
    stepOut->setToolTip(QStringLiteral("Until this routine returns"));
    connect(stepOut, &QPushButton::clicked, this, [this] { doStepOut(); });
    tb->addWidget(stepOut);
    auto *clearBp = new QPushButton(QStringLiteral("Clear Breakpoints"));
    connect(clearBp, &QPushButton::clicked, this, [this] {
        intvdebug_breakpoint_clear_all(m_dbg);
        m_seenSerial = 0;
    });
    tb->addWidget(clearBp);

    tb->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAddr = new QLineEdit;
    m_breakAddr->setMaximumWidth(120);
    m_breakAddr->setToolTip(
        QStringLiteral("Symbol name or hex address, then Enter"));
    connect(m_breakAddr, &QLineEdit::returnPressed, this,
            &DebuggerWindow::breakAtAddr);
    tb->addWidget(m_breakAddr);

    auto *loadSyms = new QPushButton(QStringLiteral("Load Symbols…"));
    connect(loadSyms, &QPushButton::clicked, this,
            &DebuggerWindow::loadSymbols);
    tb->addWidget(loadSyms);
    auto *clearSyms = new QPushButton(QStringLiteral("Clear Symbols"));
    connect(clearSyms, &QPushButton::clicked, this,
            &DebuggerWindow::clearSymbols);
    tb->addWidget(clearSyms);

    m_status = new QLabel(QStringLiteral("Running"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(m_status);
    tb->addWidget(intvHelpButton(QString()));

    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { intvHelpFor(this); });
    new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { pauseContinue(); });
    new QShortcut(QKeySequence(Qt::Key_F7), this, [this] { doStep(); });
    new QShortcut(QKeySequence(Qt::Key_F8), this, [this] { doStepOver(); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this, [this] { doStepOut(); });

    auto *tabs = new QTabWidget;
    m_tabs = tabs;
    setCentralWidget(tabs);

    /* ---- CPU tab ---- */
    auto *cpu = new QWidget;
    auto *cpuLayout = new QHBoxLayout(cpu);

    m_disasm = monoView();
    connect(m_disasm, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const QString line = m_disasm->textCursor().block().text();
        bool ok = false;
        const uint16_t addr = uint16_t(line.mid(2, 4).toUInt(&ok, 16));
        if (ok && m_disasm->hasFocus()) {
            intvdebug_breakpoint_toggle(m_dbg, addr);
            m_seenSerial = 0;
        }
    });
    cpuLayout->addWidget(m_disasm, 1);

    /* Read-only: intvdebug has no register-WRITE call (only intvdebug_write,
     * a bus-level poke -- R0-R7 are not bus-addressable that way), unlike
     * the MSX port's msxdebug_set_regs. Matches the GNOME debugger's own
     * scope, which does not offer editable registers either. */
    auto *regBox = new QGroupBox(QStringLiteral("Registers"));
    auto *regGrid = new QGridLayout(regBox);
    static const char *const kRegNames[8] = {"R0", "R1", "R2", "R3",
                                             "R4", "R5", "R6(SP)", "R7(PC)"};
    for (int i = 0; i < 8; i++) {
        regGrid->addWidget(new QLabel(QLatin1String(kRegNames[i])), i / 4,
                           (i % 4) * 2);
        m_regEdit[i] = new QLineEdit;
        m_regEdit[i]->setReadOnly(true);
        m_regEdit[i]->setMaximumWidth(64);
        regGrid->addWidget(m_regEdit[i], i / 4, (i % 4) * 2 + 1);
    }
    m_flags = new QLabel;
    regGrid->addWidget(m_flags, 2, 0, 1, 8);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->addWidget(regBox);

    auto *memBox = new QGroupBox(QStringLiteral("Memory"));
    auto *memLayout = new QVBoxLayout(memBox);
    m_memAddr = new QLineEdit;
    m_memAddr->setText(QStringLiteral("0000"));
    connect(m_memAddr, &QLineEdit::returnPressed, this,
            &DebuggerWindow::goToAddr);
    memLayout->addWidget(m_memAddr);
    m_memView = monoView();
    memLayout->addWidget(m_memView);
    rightLayout->addWidget(memBox, 1);

    cpuLayout->addWidget(right);
    tabs->addTab(cpu, QLatin1String(kTabNames[kTabCpu]));
    tabs->addTab(buildBasic(), QLatin1String(kTabNames[kTabBasic]));

    /* ---- STIC tab ---- */
    auto *stic = new QWidget;
    auto *sticGrid = new QGridLayout(stic);

    m_backtab = picLabel(INTVSTIC_BACKTAB_W * 2, INTVSTIC_BACKTAB_H * 2);
    m_backtab->setScaledContents(true);
    sticGrid->addWidget(new QLabel(QStringLiteral("BACKTAB")), 0, 0);
    sticGrid->addWidget(m_backtab, 1, 0);

    {
        auto *mobBox = new QWidget;
        auto *mobGrid = new QGridLayout(mobBox);
        for (int i = 0; i < INTVSTIC_MOB_COUNT; i++) {
            m_mob[i] = picLabel(32, 32);
            m_mob[i]->setScaledContents(true);
            mobGrid->addWidget(m_mob[i], i / 4, i % 4);
        }
        m_mobInfo = monoView();
        auto *mobCol = new QVBoxLayout;
        mobCol->addWidget(mobBox);
        mobCol->addWidget(m_mobInfo);
        auto *mobWrap = new QWidget;
        mobWrap->setLayout(mobCol);
        sticGrid->addWidget(new QLabel(QStringLiteral("MOBs")), 0, 1);
        sticGrid->addWidget(mobWrap, 1, 1);
    }

    {
        m_cards = picLabel(INTVSTIC_CARDS_PER_ROW * 8 * 2,
                           kCardRowsPerPage * 8 * 2);
        m_cards->setScaledContents(true);
        auto *bar = new QWidget;
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(0, 0, 0, 0);
        auto *prev = new QPushButton(QStringLiteral("◀ Prev"));
        auto *next = new QPushButton(QStringLiteral("Next ▶"));
        connect(prev, &QPushButton::clicked, this, [this] { cardsPage(-1); });
        connect(next, &QPushButton::clicked, this, [this] { cardsPage(1); });
        m_cardsRange = new QLabel;
        barLayout->addWidget(prev);
        barLayout->addWidget(next);
        barLayout->addWidget(m_cardsRange);
        barLayout->addStretch();
        auto *col = new QVBoxLayout;
        col->addWidget(m_cards);
        col->addWidget(bar);
        auto *wrap = new QWidget;
        wrap->setLayout(col);
        sticGrid->addWidget(new QLabel(QStringLiteral("GRAM/GROM cards")), 2,
                            0);
        sticGrid->addWidget(wrap, 3, 0);
    }

    m_palette = picLabel(INTVSTIC_PAL_CELL * 16, INTVSTIC_PAL_CELL);
    sticGrid->addWidget(new QLabel(QStringLiteral("Palette")), 2, 1);
    sticGrid->addWidget(m_palette, 3, 1, Qt::AlignTop | Qt::AlignLeft);

    m_sticState = monoView();
    m_sticState->setMinimumSize(480, 220);
    sticGrid->addWidget(new QLabel(QStringLiteral("Registers & mode")), 4, 0);
    sticGrid->addWidget(m_sticState, 5, 0, 1, 2);

    auto *sticScroll = new QScrollArea;
    sticScroll->setWidgetResizable(true);
    sticScroll->setWidget(stic);
    tabs->addTab(sticScroll, QLatin1String(kTabNames[kTabStic]));

    /* ---- Symbols tab ---- */
    auto *symTab = new QWidget;
    auto *symLayout = new QVBoxLayout(symTab);
    auto *symFilterBar = new QHBoxLayout;
    symFilterBar->addWidget(new QLabel(QStringLiteral("Filter:")));
    m_symFilter = new QLineEdit;
    connect(m_symFilter, &QLineEdit::textChanged, this,
            &DebuggerWindow::refreshSymbols);
    symFilterBar->addWidget(m_symFilter);
    m_symCount = new QLabel;
    symFilterBar->addWidget(m_symCount);
    symLayout->addLayout(symFilterBar);

    m_symList = new QListWidget;
    QFont symFont = m_symList->font();
    symFont.setFamily(QStringLiteral("monospace"));
    symFont.setStyleHint(QFont::TypeWriter);
    m_symList->setFont(symFont);
    connect(m_symList, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
                m_memBase = uint16_t(item->data(Qt::UserRole).toUInt());
                m_memAddr->clear();
                m_seenSerial = 0;
                if (intvdebug_is_paused(m_dbg))
                    refreshMem();
            });
    symLayout->addWidget(m_symList, 1);

    tabs->addTab(symTab, QLatin1String(kTabNames[kTabSymbols]));

    m_fujiPage = buildFujinet();
    tabs->addTab(m_fujiPage, QLatin1String(kTabNames[kTabFujinet]));
    /* Help's contexts (intvhelp.h), one per tab */
    {
        static const char *const kHelpTabs[] = { "cpu", "basic", "stic", "symbols", "fujinet" };
        auto setHelpContext = [this](int page) {
            if (page >= 0 && page < kTabCount)
                setProperty("intvHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kHelpTabs[page])));
        };
        setHelpContext(tabs->currentIndex());
        connect(tabs, &QTabWidget::currentChanged, this, setHelpContext);
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        if (fujiShowing())
            refreshFujinet(false);
        if (basicTab())
            basicScrollToPc();   /* the view has no size until it shows */
    });
}

/* ---- BASIC tab ----------------------------------------------------------- */

bool DebuggerWindow::basicTab() const
{
    return m_tabs->currentIndex() == kTabBasic;
}

/* On the BASIC tab the three steppers go by BASIC line. */
void DebuggerWindow::doStep()
{
    if (basicTab()) intvdebug_step_line(m_dbg); else intvdebug_step(m_dbg);
}

void DebuggerWindow::doStepOver()
{
    if (basicTab()) intvdebug_step_line_over(m_dbg); else intvdebug_step_over(m_dbg);
}

void DebuggerWindow::doStepOut()
{
    if (basicTab()) intvdebug_step_line_out(m_dbg); else intvdebug_step_out(m_dbg);
}

QWidget *DebuggerWindow::buildBasic()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    auto addButton = [&](const QString &label, const QString &tip, int kind) {
        auto *b = new QPushButton(label);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, kind] { basicPick(kind); });
        bar->addWidget(b);
    };
    addButton(QStringLiteral("Import File…"),
              QStringLiteral("Load a .bas file and everything it INCLUDEs"), kPickFile);
    addButton(QStringLiteral("Import Folder…"),
              QStringLiteral("Load every .bas file under a folder"), kPickFolder);
    addButton(QStringLiteral("Load Map…"),
              QStringLiteral("Load an as1600 -j source map, or a -l listing"), kPickMap);

    m_basFiles = new QComboBox;
    m_basFiles->setToolTip(QStringLiteral("The source file shown"));
    m_basFiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_basFiles, QOverload<int>::of(&QComboBox::activated), this,
            [this](int i) { basicShowFile(i); });
    bar->addWidget(m_basFiles);
    bar->addStretch();
    layout->addLayout(bar);

    m_basNote = new QLabel;
    m_basNote->setWordWrap(true);
    m_basNote->setEnabled(false);   /* the dim, secondary text colour */
    layout->addWidget(m_basNote);

    m_basView = monoView();
    connect(m_basView, &QPlainTextEdit::cursorPositionChanged, this,
            [this] { basicToggleAtCursor(); });
    layout->addWidget(m_basView, 1);
    return page;
}

/* Clicking a line that has code toggles a breakpoint on it, as on the CPU
 * tab. Only a real click counts: we move the cursor ourselves too. */
void DebuggerWindow::basicToggleAtCursor()
{
    if (m_basGuard || m_basFile < 0 || !m_basView->hasFocus())
        return;
    const int line = m_basView->textCursor().blockNumber() + 1;
    const int addr = intvsrcmap_first_addr(m_srcmap, m_basFile, line);
    if (addr < 0) {
        m_status->setText(QStringLiteral("No code on that line"));
        return;
    }
    intvdebug_breakpoint_toggle(m_dbg, uint16_t(addr));
    m_seenSerial = 0;   /* redraw the marker immediately */
}

void DebuggerWindow::basicScrollToPc()
{
    if (m_basPcLine < 1)
        return;
    const QTextBlock block = m_basView->document()->findBlockByNumber(m_basPcLine - 1);
    if (!block.isValid())
        return;
    m_basGuard = true;
    m_basView->setTextCursor(QTextCursor(block));
    const QRect r = m_basView->cursorRect();
    if (r.top() < 0 || r.bottom() > m_basView->viewport()->height())
        m_basView->centerCursor();
    m_basGuard = false;
}

/* The map changed (an import, a Load Map): the file list. */
void DebuggerWindow::basicReload()
{
    const int n = intvsrcmap_file_count(m_srcmap);
    m_basFiles->clear();
    for (int i = 0; i < n; i++)
        m_basFiles->addItem(QString::fromUtf8(intvsrcmap_file_name(m_srcmap, i)));
    if (m_basFile >= n)
        m_basFile = -1;
    if (m_basFile < 0 && n > 0) {
        m_basFile = 0;   /* the first file with any code on it */
        bool found = false;
        for (int i = 0; i < n && !found; i++) {
            const int lines = intvsrcmap_line_count(m_srcmap, i);
            for (int l = 1; l <= lines && !found; l++)
                if (intvsrcmap_first_addr(m_srcmap, i, l) >= 0) {
                    m_basFile = i;
                    found = true;
                }
        }
    }
    m_basFiles->setCurrentIndex(m_basFile);
    m_basPcLine = 0;
    if (n == 0)
        m_basNote->setText(QStringLiteral(
            "Import a .bas file, or a folder of them. The assembler's map "
            "(as1600 -j game.smap) puts each line at its address."));
}

void DebuggerWindow::basicShowFile(int file)
{
    m_basFile = file;
    m_basPcLine = 0;
    refreshBasic(intvdebug_is_paused(m_dbg));
}

void DebuggerWindow::refreshBasic(bool paused)
{
    const unsigned gen = intvsrcmap_generation(m_srcmap);
    int pf = -1, pl = 0;
    intvdebug_regs r;

    if (gen != m_basGen) {
        m_basGen = gen;
        basicReload();
    }
    if (paused && intvdebug_regs_get(m_dbg, &r)) {
        int f, l;
        if (intvsrcmap_lookup(m_srcmap, r.pc, &f, &l)) {
            pf = f;
            pl = l;
        }
    }
    if (pf >= 0 && pf != m_basFile) {   /* followed into another file */
        m_basFile = pf;
        m_basFiles->setCurrentIndex(pf);
    }

    QString text;
    if (m_basFile >= 0) {
        const int n = intvsrcmap_line_count(m_srcmap, m_basFile);
        text.reserve(n * 48);
        for (int i = 1; i <= n; i++) {
            const int addr = intvsrcmap_first_addr(m_srcmap, m_basFile, i);
            const char *src = intvsrcmap_line_text(m_srcmap, m_basFile, i);
            text += QStringLiteral("%1%2 %3  %4\n")
                        .arg(pf == m_basFile && i == pl ? QLatin1Char('>') : QLatin1Char(' '))
                        .arg(addr >= 0 && intvdebug_breakpoint_is_set(m_dbg, uint16_t(addr))
                                 ? QLatin1Char('*') : QLatin1Char(' '))
                        .arg(i, 5)
                        .arg(QString::fromUtf8(src ? src : ""));
        }
    }
    m_basGuard = true;   /* setPlainText moves the cursor: not a click */
    setText(m_basView, text);
    m_basGuard = false;

    const int line = pf == m_basFile ? pl : 0;
    if (line != m_basPcLine) {
        m_basPcLine = line;
        basicScrollToPc();
    }
}

void DebuggerWindow::basicImported(const QString &path, int kind)
{
    intvsrcmap_import_result r;
    const int n = intvsrcmap_import(m_srcmap, path.toUtf8().constData(), m_symtab, &r);
    QString msg;

    if (n < 0) {
        msg = kind == kPickFolder ? QStringLiteral("No .bas files in that folder.")
                                  : QStringLiteral("Could not read that file.");
    } else if (r.mapped > 0) {
        msg = QStringLiteral("%1 file%2, %3 address range%4 from %5%6.")
                  .arg(r.files).arg(r.files == 1 ? QString() : QStringLiteral("s"))
                  .arg(r.mapped).arg(r.mapped == 1 ? QString() : QStringLiteral("s"))
                  .arg(QFileInfo(QString::fromUtf8(r.map_path)).fileName())
                  .arg(r.symbols ? QStringLiteral(", with its symbols") : QString());
    } else {
        msg = QStringLiteral("%1 file%2 loaded, but no .smap or .lst was found beside them. "
                             "Build with as1600 -j game.smap, then Load Map.")
                  .arg(r.files).arg(r.files == 1 ? QString() : QStringLiteral("s"));
    }
    m_basNote->setText(msg);
    if (n >= 0) {
        m_basFile = -1;   /* basicReload picks the first file with code */
        m_seenSerial = 0;
        if (intvdebug_is_paused(m_dbg))
            refreshAll();
    }
}

void DebuggerWindow::basicPick(int kind)
{
    const QString lastDir = m_session
                                ? QString::fromUtf8(intvsession_get_str(m_session, "bas_dir", ""))
                                : QString();
    QString path;
    if (kind == kPickFolder)
        path = QFileDialog::getExistingDirectory(this, QStringLiteral("Import Folder"), lastDir);
    else if (kind == kPickMap)
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Map"), lastDir,
            QStringLiteral("as1600 source maps and listings (*.smap *.lst);;All files (*)"));
    else
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import File"), lastDir,
            QStringLiteral("IntyBASIC source (*.bas);;All files (*)"));
    if (path.isEmpty())
        return;

    if (m_session) {   /* start the next picker where this one ended */
        const QString dir = kind == kPickFolder ? path : QFileInfo(path).absolutePath();
        intvsession_set_str(m_session, "bas_dir", dir.toUtf8().constData());
        intvsession_settings_flush(m_session);
    }
    if (kind == kPickMap) {
        const int n = intvsrcmap_load_map(m_srcmap, path.toUtf8().constData());
        if (n < 0) {
            m_basNote->setText(QStringLiteral("Could not read that map."));
            return;
        }
        const int syms = intvsrcmap_load_sibling_sym(path.toUtf8().constData(), m_symtab);
        m_basNote->setText(QStringLiteral("%1 address range%2 from %3%4.")
                               .arg(n).arg(n == 1 ? QString() : QStringLiteral("s"))
                               .arg(QFileInfo(path).fileName())
                               .arg(syms ? QStringLiteral(", with its symbols") : QString()));
        m_seenSerial = 0;
        if (intvdebug_is_paused(m_dbg))
            refreshAll();
    } else {
        basicImported(path, kind);
    }
}

QWidget *DebuggerWindow::buildFujinet()
{
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);

    auto *logBox = new QGroupBox(QStringLiteral("FujiBus packets (SLIP-decoded)"));
    auto *logLayout = new QVBoxLayout(logBox);
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    clear->setToolTip(QStringLiteral("Forget the packets so far"));
    connect(clear, &QPushButton::clicked, this, [this] {
        intvdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    logLayout->addLayout(row);
    m_fujiText = monoView();
    logLayout->addWidget(m_fujiText, 1);
    logBox->setMinimumWidth(560);
    layout->addWidget(logBox, 3);

    auto *stateBox = new QGroupBox(QStringLiteral("FujiNet's mailbox"));
    auto *stateLayout = new QVBoxLayout(stateBox);
    m_fujiState = monoView();
    stateLayout->addWidget(m_fujiState);
    stateBox->setMinimumWidth(320);
    layout->addWidget(stateBox, 2);
    return page;
}

bool DebuggerWindow::fujiShowing() const
{
    return m_tabs->currentWidget() == m_fujiPage;
}

void DebuggerWindow::refreshFujinet(bool force)
{
    static intvdebug_fuji_packet p[INTVDEBUG_FUJI_TRACE_MAX];
    static char state[2048];
    const int n = intvdebug_fujinet_trace(m_dbg, p, INTVDEBUG_FUJI_TRACE_MAX);

    m_fujiTicks = 0;
    if (!m_session || (!intvsession_fujinet_running(m_session) && !intvsession_fujinet_external(m_session)))
        m_fujiNote->setText(QStringLiteral("FujiNet is not running."));
    else
        m_fujiNote->setText(QStringLiteral("%1 FujiNet at %2, cartridge %3.")
                                .arg(intvsession_fujinet_external(m_session) ? QStringLiteral("External")
                                                                             : QStringLiteral("Built-in"))
                                .arg(QString::fromUtf8(intvsession_boip_endpoint(m_session)))
                                .arg(intvsession_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                         : QStringLiteral("not connected")));
    intvdebug_fujinet_state(m_dbg, state, int(sizeof(state)));
    setText(m_fujiState, QString::fromUtf8(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast))
        return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        char hex[sizeof(p[i].data) * 3 + 1];
        int len = 0;
        for (int k = 0; k < p[i].nbytes && k < int(sizeof(p[i].data)); k++)
            len += std::snprintf(hex + len, sizeof(hex) - size_t(len), "%02X ",
                                 p[i].data[k]);
        hex[len] = '\0';
        if (i)
            t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        t += QStringLiteral("\n     ");
        t += QLatin1String(hex);
        if (p[i].nbytes > int(sizeof(p[i].data)))
            t += QStringLiteral("…");
    }
    if (n == 0)
        t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, force || atEnd(m_fujiText));
}

void DebuggerWindow::goToAddr()
{
    uint16_t addr;
    if (intvsym_parse_addr(m_symtab, m_memAddr->text().toUtf8().constData(),
                           &addr)) {
        m_memBase = addr;
        m_seenSerial = 0;
    } else {
        m_status->setText(
            QStringLiteral("No such symbol or address: %1").arg(m_memAddr->text()));
    }
}

void DebuggerWindow::breakAtAddr()
{
    uint16_t addr;
    if (intvsym_parse_addr(m_symtab, m_breakAddr->text().toUtf8().constData(),
                           &addr)) {
        intvdebug_breakpoint_toggle(m_dbg, addr);
        m_seenSerial = 0; /* redraw the marker immediately */
    } else {
        m_status->setText(QStringLiteral("No such symbol or address: %1")
                              .arg(m_breakAddr->text()));
    }
}

/* Only rebuilds the list when the table's generation or the filter text has
 * changed since the last call -- a real game .sym can be a few hundred
 * entries, no reason to re-walk and re-widget it on every timer tick. */
void DebuggerWindow::refreshSymbols()
{
    const unsigned gen = intvsymtab_generation(m_symtab);
    const QString filter = m_symFilter->text();
    if (gen == m_symSeenGen && filter == m_symFilterText)
        return;
    m_symSeenGen = gen;
    m_symFilterText = filter;

    m_symList->clear();
    const int n = intvsymtab_count(m_symtab);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        uint16_t addr;
        char name[64], note[64];
        if (!intvsymtab_enum(m_symtab, i, &addr, name, sizeof(name), note,
                             sizeof(note)))
            continue;
        const QString qname = QString::fromUtf8(name);
        if (!filter.isEmpty() &&
            !qname.contains(filter, Qt::CaseInsensitive))
            continue;
        /* Only the address is uppercased -- as1600 symbol names are
         * case-sensitive (see symbols.h), so unlike refreshDisasm's whole-
         * string .toUpper() (which only ever uppercases its own hex/
         * mnemonic text), the name/note here must be left exactly as
         * loaded. */
        QString text =
            QStringLiteral("%1  %2")
                .arg(QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0'))
                        .toUpper())
                .arg(qname.leftJustified(24));
        if (note[0])
            text += QStringLiteral("  ; %1").arg(QString::fromUtf8(note));
        auto *item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, addr);
        m_symList->addItem(item);
        shown++;
    }
    m_symCount->setText(QStringLiteral("%1 symbol%2%3")
                            .arg(shown)
                            .arg(shown == 1 ? QString() : QStringLiteral("s"))
                            .arg(filter.isEmpty() ? QString()
                                                  : QStringLiteral(" (filtered)")));
}

void DebuggerWindow::loadSymbols()
{
    /* Remember the directory (settings key "sym_dir") so re-opening the
     * picker -- expected to happen often, since loading is always a manual
     * step taken after CONFIG has already booted a cart, never automatic --
     * starts back where the user just was. */
    const QString lastDir = m_session
                                ? QString::fromUtf8(
                                      intvsession_get_str(m_session, "sym_dir", ""))
                                : QString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Symbols"), lastDir,
        QStringLiteral("as1600 symbol files (*.sym);;All files (*)"));
    if (path.isEmpty())
        return;
    const int n = intvsymtab_load(m_symtab, path.toUtf8().constData());
    if (n < 0) {
        QMessageBox::warning(this, QStringLiteral("Load Symbols"),
                             QStringLiteral("Could not open %1").arg(path));
        return;
    }
    if (m_session) {
        char dir[1024];
        if (intvsym_dirname(path.toUtf8().constData(), dir, sizeof(dir)) > 0) {
            intvsession_set_str(m_session, "sym_dir", dir);
            intvsession_settings_flush(m_session);
        }
    }
    m_seenSerial = 0; /* force a redraw with the new symbols applied */
    if (intvdebug_is_paused(m_dbg))
        refreshAll();
}

void DebuggerWindow::clearSymbols()
{
    intvsymtab_clear_user(m_symtab);
    m_seenSerial = 0;
    if (intvdebug_is_paused(m_dbg))
        refreshAll();
}

void DebuggerWindow::pauseContinue()
{
    if (intvdebug_is_paused(m_dbg)) {
        intvdebug_resume(m_dbg);
    } else {
        intvdebug_pause(m_dbg);
    }
}

void DebuggerWindow::refreshDisasm()
{
    intvdebug_dasm_line lines[kDisasmLines];
    intvdebug_regs r;
    if (!intvdebug_regs_get(m_dbg, &r))
        return;

    const int n = intvdebug_disassemble(m_dbg, r.pc, r.D, lines, kDisasmLines);
    QString text;
    for (int i = 0; i < n; i++) {
        char symsuffix[96];
        intvsym_annotate_line(m_symtab, &lines[i], symsuffix,
                              sizeof(symsuffix));
        text += QStringLiteral("%1%2 %3  %4%5\n")
                    .arg(lines[i].addr == r.pc ? QLatin1Char('>')
                                               : QLatin1Char(' '))
                    .arg(intvdebug_breakpoint_is_set(m_dbg, lines[i].addr)
                             ? QLatin1Char('*')
                             : QLatin1Char(' '))
                    .arg(lines[i].addr, 4, 16, QLatin1Char('0'))
                    .arg(QString::fromUtf8(lines[i].text), -24)
                    .arg(QString::fromUtf8(symsuffix));
    }
    m_disasm->setPlainText(text.toUpper());
}

void DebuggerWindow::refreshRegs()
{
    intvdebug_regs r;
    if (!intvdebug_regs_get(m_dbg, &r)) {
        for (auto &e : m_regEdit) e->setText(QString());
        m_flags->setText(QString());
        return;
    }
    for (int i = 0; i < 8; i++)
        m_regEdit[i]->setText(
            QStringLiteral("%1").arg(r.r[i], 4, 16, QLatin1Char('0'))
                .toUpper());
    m_flags->setText(QStringLiteral("S%1 C%2 O%3 Z%4 I%5   D%6")
                         .arg(r.S)
                         .arg(r.C)
                         .arg(r.O)
                         .arg(r.Z)
                         .arg(r.I)
                         .arg(r.D));
}

void DebuggerWindow::refreshMem()
{
    uint16_t words[kMemWordsPerRow * kMemRows];
    const int n = intvdebug_read(m_dbg, m_memBase, words,
                                 kMemWordsPerRow * kMemRows);
    QString text;
    const char *lastRegion = nullptr;
    for (int row = 0; row * kMemWordsPerRow < n; row++) {
        const uint16_t rowAddr = uint16_t(m_memBase + row * kMemWordsPerRow);
        text += QStringLiteral("%1 ").arg(rowAddr, 4, 16, QLatin1Char('0'))
                    .toUpper();
        for (int col = 0; col < kMemWordsPerRow; col++) {
            const int idx = row * kMemWordsPerRow + col;
            if (idx < n)
                text += QStringLiteral(" %1").arg(words[idx], 4, 16,
                                                  QLatin1Char('0')).toUpper();
        }
        /* Only label the first row of a run in the same region -- see
         * symbols.c's k_regions and the GNOME frontend's own refresh_mem. */
        const char *region = intvsym_region(rowAddr);
        if (region && region != lastRegion)
            text += QStringLiteral("  ; %1").arg(QString::fromUtf8(region));
        lastRegion = region;
        text += QLatin1Char('\n');
    }
    m_memView->setPlainText(text);
}

void DebuggerWindow::refreshStic()
{
    static intvstic_snapshot snap;
    static uint8_t backtabRgba[INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4];
    static uint8_t mobRgba[16 * 16 * 4];
    static uint8_t cardsRgba[INTVSTIC_CARDS_PER_ROW * 8 * kCardRowsPerPage *
                             8 * 4];
    static uint8_t palRgba[INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4];
    static char text[2048];

    intvstic_snapshot_get(m_dbg, &snap);

    intvstic_render_backtab(&snap, backtabRgba);
    setRgba(m_backtab, backtabRgba, INTVSTIC_BACKTAB_W, INTVSTIC_BACKTAB_H);

    QString mobText =
        QStringLiteral("## X    Y  CARD CLR SZx SZy VIS PRI GRAM COLL\n");
    for (int i = 0; i < INTVSTIC_MOB_COUNT; i++) {
        intvstic_mob mob;
        intvstic_mob_get(&snap, i, &mob);
        intvstic_render_mob(&snap, i, mobRgba);
        setRgba(m_mob[i], mobRgba, 16, 16);
        mobText += QStringLiteral("%1  %2  %3  %4  %5  %6x  %7x  %8   %9   "
                                  "%10    %11\n")
                       .arg(i)
                       .arg(mob.x, 3)
                       .arg(mob.y, 3)
                       .arg(mob.card, 3)
                       .arg(mob.color, 2)
                       .arg(mob.x_size)
                       .arg(mob.y_stretch)
                       .arg(mob.visible)
                       .arg(mob.priority)
                       .arg(mob.gram)
                       .arg(mob.collision, 3, 16, QLatin1Char('0')).toUpper();
    }
    m_mobInfo->setPlainText(mobText);

    const int total = intvstic_card_count(&snap);
    m_cardTotal = total;
    if (m_cardFirst >= total)
        m_cardFirst = 0;
    intvstic_render_cards(&snap, m_cardFirst, kCardRowsPerPage, cardsRgba);
    setRgba(m_cards, cardsRgba, INTVSTIC_CARDS_PER_ROW * 8,
           kCardRowsPerPage * 8);
    const int page = INTVSTIC_CARDS_PER_ROW * kCardRowsPerPage;
    const int lastShown = std::min(m_cardFirst + page, total) - 1;
    m_cardsRange->setText(QStringLiteral("cards %1–%2 of %3")
                              .arg(m_cardFirst)
                              .arg(lastShown)
                              .arg(total));

    intvstic_render_palette(m_dbg, palRgba);
    setRgba(m_palette, palRgba, INTVSTIC_PAL_CELL * 16, INTVSTIC_PAL_CELL);

    intvstic_format_state(&snap, text, int(sizeof(text)));
    m_sticState->setPlainText(QString::fromUtf8(text));
}

void DebuggerWindow::cardsPage(int delta)
{
    const int page = INTVSTIC_CARDS_PER_ROW * kCardRowsPerPage;
    const int lastPage =
        m_cardTotal > 0 ? ((m_cardTotal - 1) / page) * page : 0;
    int next = m_cardFirst + delta * page;
    if (next < 0)
        next = 0;
    if (next > lastPage)
        next = lastPage;
    m_cardFirst = next;
    m_seenSerial = 0;
    if (intvdebug_is_paused(m_dbg))
        refreshStic();
}

void DebuggerWindow::refreshAll()
{
    const int paused = intvdebug_is_paused(m_dbg);
    m_pauseBtn->setText(paused ? QStringLiteral("Continue (F5)")
                               : QStringLiteral("Pause (F5)"));
    m_status->setText(paused ? QStringLiteral("Paused")
                             : QStringLiteral("Running"));

    if (paused) {
        refreshRegs();
        refreshDisasm();
        refreshMem();
        refreshStic();
    } else {
        for (auto &e : m_regEdit) e->setText(QString());
        m_flags->setText(QString());
        m_disasm->setPlainText(QString());
        m_memView->setPlainText(QString());
    }
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refreshSymbols's own comment). */
    refreshSymbols();
    refreshBasic(paused);
    if (fujiShowing())
        refreshFujinet(false);
    m_wasPaused = paused;
}
