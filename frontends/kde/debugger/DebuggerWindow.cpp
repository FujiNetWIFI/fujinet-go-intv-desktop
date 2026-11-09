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
#include <QCheckBox>
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
#include <QTextEdit>
#include <QShortcut>
#include <QPointer>
#include <QStyle>
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
#include <functional>
#include <QCloseEvent>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "memtools.h"
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
const char *const kTabNames[] = {"CPU", "BASIC", "Memory", "Breakpoints", "Watches", "Cart", "Trace", "History", "STIC", "Symbols", "FujiNet"};
enum { kTabCpu, kTabBasic, kTabMemory, kTabBreaks, kTabWatches, kTabCart, kTabTrace, kTabHistory,
       kTabStic, kTabSymbols, kTabFujinet };
enum { kPickFile, kPickFolder, kPickMap };
constexpr int kTabCount = int(sizeof(kTabNames) / sizeof(kTabNames[0]));

int tabFromEnv(const char *var = "INTV_DEBUGGER_TAB")
{
    const char *tab = std::getenv(var);
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

/* The current line: a bar in the accent (the icon's green) and white text.
 * The bar is the block's own background: a full-width ExtraSelection would
 * repaint the gutter's red dot in its own colour. The white text is a
 * selection over everything but the gutter column. `prev` remembers which
 * block wears the bar, so a refresh that kept the text can move it. */
void pcBar(QList<QTextEdit::ExtraSelection> &out, QPlainTextEdit *v, int &prev, int block)
{
    QTextDocument *doc = v->document();
    auto setBg = [doc](int n, const QBrush &brush) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isValid())
            return;
        QTextCursor c(b);
        QTextBlockFormat f = c.blockFormat();
        f.setBackground(brush);
        c.setBlockFormat(f);
    };
    if (prev >= 0 && prev != block)
        setBg(prev, Qt::NoBrush);
    prev = -1;
    if (block < 0)
        return;
    setBg(block, QColor(0x1E, 0x49, 0x12));
    prev = block;

    QTextEdit::ExtraSelection text;
    const QTextBlock b = doc->findBlockByNumber(block);
    text.cursor = QTextCursor(b);
    text.cursor.setPosition(b.position() + 1);
    text.cursor.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    text.format.setForeground(Qt::white);
    out.append(text);
}

/* A breakpoint's gutter dot, red (drawn over the bar when both apply). */
QTextEdit::ExtraSelection bpDot(QPlainTextEdit *v, int block, bool enabled = true)
{
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(v->document()->findBlockByNumber(block));
    sel.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    sel.format.setForeground(enabled ? QColor(0xE0, 0x1B, 0x24) : QColor(0x8A, 0x8A, 0x8A));
    return sel;
}

const QString kBpDot = QStringLiteral("\u25CF");
const QString kBpDotOff = QStringLiteral("\u25CB");   /* disabled: a hollow grey dot */

/* The gutter's mark for an EXEC breakpoint's state (0 none, 1 on, 2 off). */
QString dotFor(int state)
{
    return state == 1 ? kBpDot : state == 2 ? kBpDotOff : QStringLiteral(" ");
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
    if (tabFromEnv("INTV_POPOUT") >= 0)   /* for captures: peel a tab off at the start */
        popOut(tabFromEnv("INTV_POPOUT"));

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

    /* More captures: INTV_WATCH="x" adds a watch, INTV_WATCH_BREAK="x" a
     * write-when-changed breakpoint on it, INTV_MEM_GOTO="x" shows it in Memory. */
    {
        const char *wt = std::getenv("INTV_WATCH"), *wb = std::getenv("INTV_WATCH_BREAK"),
                   *mg = std::getenv("INTV_MEM_GOTO");
        uint16_t lo, hi;
        if (wt && *wt)
            intvwatch_add(wt);
        if (wb && *wb && intvsym_parse_range(m_symtab, wb, &lo, &hi, nullptr)) {
            intvdebug_bp bp;
            std::memset(&bp, 0, sizeof(bp));
            bp.type = INTVDEBUG_BP_WRITE;
            bp.lo = lo;
            bp.hi = hi;
            bp.cond = INTVDEBUG_COND_CHANGED;
            bp.mask = 0xFF;
            intvdebug_bp_add(m_dbg, &bp);
        }
        if (mg && *mg && intvsym_parse_range(m_symtab, mg, &lo, &hi, nullptr))
            showInMemory(lo, hi);
    }
    if (std::getenv("INTV_HISTORY"))   /* record from the start, for captures */
        intvdebug_history_enable(m_dbg, 1);

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
        } else if (tabVisible(kTabTrace) && ++m_hTicks >= 12) {
            m_hTicks = 0;   /* the live view moves while the machine runs */
            refreshHist(0, false);
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
    m_closing = true;   /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
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
    auto *popOutBtn = new QPushButton(QStringLiteral("Pop Out"));
    popOutBtn->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(popOutBtn, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    tb->addWidget(popOutBtn);

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
    auto *regRow = new QHBoxLayout(regBox);
    static const char *const kRegNames[8] = {"R0", "R1", "R2", "R3",
                                             "R4", "R5", "R6(SP)", "R7(PC)"};
    for (int i = 0; i < 8; i++) {
        regRow->addWidget(new QLabel(QLatin1String(kRegNames[i])));
        m_regEdit[i] = new QLineEdit;
        m_regEdit[i]->setReadOnly(true);
        m_regEdit[i]->setMaximumWidth(64);
        regRow->addWidget(m_regEdit[i]);
    }
    m_flags = new QLabel;
    regRow->addWidget(m_flags);
    regRow->addStretch();

    /* The registers sit under every tab; the memory view has a tab of its own. */
    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->addWidget(tabs, 1);
    centralLayout->addWidget(regBox);
    setCentralWidget(central);

    auto *memTab = new QWidget;
    auto *memLayout = new QVBoxLayout(memTab);
    m_memAddr = new QLineEdit;
    m_memAddr->setText(QStringLiteral("0000"));
    m_memAddr->setPlaceholderText(QStringLiteral("Address, symbol or BASIC variable"));
    connect(m_memAddr, &QLineEdit::returnPressed, this,
            &DebuggerWindow::goToAddr);
    memLayout->addWidget(m_memAddr);
    memLayout->addWidget(buildSearchBar());
    m_memView = monoView();
    memLayout->addWidget(m_memView, 1);

    tabs->addTab(cpu, QLatin1String(kTabNames[kTabCpu]));
    tabs->addTab(buildBasic(), QLatin1String(kTabNames[kTabBasic]));
    tabs->addTab(memTab, QLatin1String(kTabNames[kTabMemory]));
    tabs->addTab(buildBreaks(), QLatin1String(kTabNames[kTabBreaks]));
    tabs->addTab(buildWatches(), QLatin1String(kTabNames[kTabWatches]));
    tabs->addTab(buildCart(), QLatin1String(kTabNames[kTabCart]));
    tabs->addTab(buildHist(0), QLatin1String(kTabNames[kTabTrace]));
    tabs->addTab(buildHist(1), QLatin1String(kTabNames[kTabHistory]));

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
    for (int i = 0; i < kTabCount; i++) {   /* each page keeps its identity */
        QWidget *pg = tabs->widget(i);
        pg->setProperty("tab", i);
        m_pages[i] = pg;
    }
    /* Help's contexts (intvhelp.h), one per tab */
    {
        static const char *const kHelpTabs[] = { "cpu", "basic", "memory", "breakpoints", "watches", "cart", "trace", "history", "stic", "symbols", "fujinet" };
        auto setHelpContext = [this](int page) {
            if (page >= 0 && page < kTabCount)
                setProperty("intvHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kHelpTabs[page])));
        };
        setHelpContext(currentTab());
        connect(tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) { setHelpContext(currentTab()); });
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        if (fujiShowing())
            refreshFujinet(false);
        if (basicTab())
            basicScrollToPc();   /* the view has no size until it shows */
    });
}

/* ---- BASIC tab ----------------------------------------------------------- */

/* A page's place in the tab widget is not its identity once tabs can be
 * moved into windows of their own: each carries its original index. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

bool DebuggerWindow::basicTab() const
{
    return currentTab() == kTabBasic ||
           (m_popped[kTabBasic] && m_popped[kTabBasic]->isActiveWindow());
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

/* ---- Breakpoints, Watches, memory search ------------------------------------ */

namespace {
/* One line on why the machine stopped. */
QString hex(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QString stopText(const intvdebug_stop_info &st)
{
    switch (st.reason) {
    case INTVDEBUG_STOP_WATCH_WRITE:
        return QStringLiteral("wrote $%1 = $%2 (was $%3), now at $%4")
            .arg(hex(st.addr, 4), hex(st.value & 0xFF, 2), hex(st.old_value & 0xFF, 2), hex(st.pc, 4));
    case INTVDEBUG_STOP_WATCH_READ:
        return QStringLiteral("read $%1, now at $%2").arg(hex(st.addr, 4), hex(st.pc, 4));
    case INTVDEBUG_STOP_BREAKPOINT:
        return QStringLiteral("breakpoint at $%1").arg(hex(st.pc, 4));
    }
    return QString();
}
} // namespace

QWidget *DebuggerWindow::buildBreaks()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;

    m_bpType = new QComboBox;
    m_bpType->addItems({QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                        QStringLiteral("Access")});
    m_bpType->setToolTip(QStringLiteral(
        "Execute stops before an instruction; Read, Write and Access stop after the instruction "
        "that touched the words (data accesses only, not code fetches)"));
    m_bpAddr = new QLineEdit;
    m_bpAddr->setPlaceholderText(QStringLiteral("address, lo-hi, symbol, x, #score, a(3)"));
    connect(m_bpAddr, &QLineEdit::returnPressed, this, &DebuggerWindow::addBreak);
    m_bpCond = new QComboBox;
    m_bpCond->addItems({QStringLiteral("any value"), QStringLiteral("changed"), QStringLiteral("equals"),
                        QStringLiteral("not equal")});
    m_bpCond->setToolTip(QStringLiteral("A condition on the value written (Write and Access)"));
    m_bpValue = new QLineEdit;
    m_bpValue->setPlaceholderText(QStringLiteral("value (hex)"));
    m_bpValue->setMaximumWidth(110);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, &DebuggerWindow::addBreak);
    form->addWidget(m_bpType);
    form->addWidget(m_bpAddr, 1);
    form->addWidget(m_bpCond);
    form->addWidget(m_bpValue);
    form->addWidget(add);
    layout->addLayout(form);

    m_bpList = new QListWidget;
    QFont f = m_bpList->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    m_bpList->setFont(f);
    connect(m_bpList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_bpBuilding)
            return;
        intvdebug_bp_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        m_seenSerial = 0;
    });
    layout->addWidget(m_bpList, 1);

    auto *foot = new QHBoxLayout;
    m_bpStop = new QLabel;
    m_bpStop->setEnabled(false);
    foot->addWidget(m_bpStop, 1);
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    remove->setToolTip(QStringLiteral("Remove the selected breakpoint"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *it = m_bpList->currentItem()) {
            intvdebug_bp_remove(m_dbg, it->data(Qt::UserRole).toInt());
            m_seenSerial = 0;
        }
    });
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        intvdebug_bp_clear_hits(m_dbg);
        m_seenSerial = 0;
    });
    auto *removeAll = new QPushButton(QStringLiteral("Remove All"));
    connect(removeAll, &QPushButton::clicked, this, [this] {
        intvdebug_breakpoint_clear_all(m_dbg);
        m_seenSerial = 0;
    });
    foot->addWidget(remove);
    foot->addWidget(clearHits);
    foot->addWidget(removeAll);
    layout->addLayout(foot);
    return page;
}

void DebuggerWindow::addBreak()
{
    const int type = m_bpType->currentIndex();
    const QByteArray text = m_bpAddr->text().toUtf8();
    uint16_t lo = 0, hi = 0;
    intvdebug_bp bp;
    std::memset(&bp, 0, sizeof(bp));

    if (type == 0 ? !intvsym_parse_addr(m_symtab, text.constData(), &lo)
                  : !intvsym_parse_range(m_symtab, text.constData(), &lo, &hi, nullptr)) {
        m_status->setText(QStringLiteral("No such symbol, variable or address: %1").arg(m_bpAddr->text()));
        return;
    }
    if (type == 0)
        hi = lo;
    bp.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE);
    bp.lo = lo;
    bp.hi = hi;
    bp.cond = m_bpCond->currentIndex();   /* any, changed, =, != in the combo's order */
    QString v = m_bpValue->text().trimmed();
    if (v.startsWith(QLatin1Char('$')))
        v.remove(0, 1);
    bp.value = uint16_t(v.toUInt(nullptr, 16));
    bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF;   /* the scratchpad is 8 bits */
    if (intvdebug_bp_add(m_dbg, &bp) < 0) {
        m_status->setText(QStringLiteral("The breakpoint table is full"));
        return;
    }
    m_bpAddr->clear();
    m_seenSerial = 0;
}

void DebuggerWindow::refreshBreaks()
{
    const unsigned gen = intvdebug_bp_generation(m_dbg);
    if (gen != m_bpGen) {
        m_bpGen = gen;
        intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
        const int n = intvdebug_bp_list(m_dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
        const int keep = m_bpList->currentRow();
        m_bpBuilding = true;
        m_bpList->clear();
        for (int i = 0; i < n; i++) {
            char desc[160];
            intvbp_describe(&bps[i], m_symtab, desc, sizeof(desc));
            auto *item = new QListWidgetItem(QStringLiteral("%1    hits %2")
                                                 .arg(QString::fromUtf8(desc)).arg(bps[i].hits));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, bps[i].id);
            m_bpList->addItem(item);
        }
        if (keep >= 0 && keep < n)
            m_bpList->setCurrentRow(keep);
        m_bpBuilding = false;
    }
    intvdebug_stop_info st;
    const bool paused = intvdebug_is_paused(m_dbg);
    if (paused && intvdebug_last_stop(m_dbg, &st)) {
        const QString why = stopText(st);
        m_bpStop->setText(QStringLiteral("Stopped: %1").arg(why.isEmpty() ? QStringLiteral("paused") : why));
    } else {
        m_bpStop->setText(paused ? QStringLiteral("Paused.") : QStringLiteral("Running."));
    }
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;
    form->addWidget(new QLabel(QStringLiteral("Watch:")));
    m_wtEntry = new QLineEdit;
    m_wtEntry->setPlaceholderText(QStringLiteral("address, symbol, x, #score, a(3), lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, &DebuggerWindow::addWatch);
    form->addWidget(m_wtEntry, 1);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, &DebuggerWindow::addWatch);
    form->addWidget(add);
    layout->addLayout(form);

    m_wtList = new QListWidget;
    QFont f = m_wtList->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    m_wtList->setFont(f);
    layout->addWidget(m_wtList, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *mem = new QPushButton(QStringLiteral("Show in Memory"));
    connect(mem, &QPushButton::clicked, this, [this] {
        uint16_t lo, hi;
        if (m_wtList->currentRow() >= 0 && intvwatch_range(m_symtab, m_wtList->currentRow(), &lo, &hi)) {
            if (m_popped[kTabMemory])
                m_popped[kTabMemory]->activateWindow();
            else
                m_tabs->setCurrentWidget(m_pages[kTabMemory]);
            showInMemory(lo, hi);
        }
    });
    auto *brk = new QPushButton(QStringLiteral("Break on Change"));
    connect(brk, &QPushButton::clicked, this, [this] {
        uint16_t lo, hi;
        if (m_wtList->currentRow() < 0 || !intvwatch_range(m_symtab, m_wtList->currentRow(), &lo, &hi))
            return;
        intvdebug_bp bp;
        std::memset(&bp, 0, sizeof(bp));
        bp.type = INTVDEBUG_BP_WRITE;
        bp.lo = lo;
        bp.hi = hi;
        bp.cond = INTVDEBUG_COND_CHANGED;
        bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF;
        if (intvdebug_bp_add(m_dbg, &bp) >= 0)
            m_status->setText(QStringLiteral("Breaking when it changes"));
        m_seenSerial = 0;
    });
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_wtList->currentRow() >= 0) {
            intvwatch_remove(m_wtList->currentRow());
            m_seenSerial = 0;
        }
    });
    foot->addWidget(mem);
    foot->addWidget(brk);
    foot->addWidget(remove);
    layout->addLayout(foot);
    return page;
}

void DebuggerWindow::addWatch()
{
    if (intvwatch_add(m_wtEntry->text().toUtf8().constData()) < 0) {
        m_status->setText(QStringLiteral("Type an address, symbol or BASIC variable (x, #score, a(3))"));
        return;
    }
    m_wtEntry->clear();
    m_seenSerial = 0;
}

void DebuggerWindow::refreshWatches()
{
    const int n = intvwatch_count();
    const int keep = m_wtList->currentRow();
    m_wtList->clear();
    for (int i = 0; i < n; i++) {
        char line[200];
        intvwatch_format(m_dbg, m_symtab, i, line, sizeof(line));
        m_wtList->addItem(QString::fromUtf8(line));
    }
    if (keep >= 0 && keep < n)
        m_wtList->setCurrentRow(keep);
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search:")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({QStringLiteral("Hex words"), QStringLiteral("Text")});
    m_srchKind->setToolTip(QStringLiteral("Hex words (02A0 0268) or text, one character per word"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { search(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    connect(next, &QPushButton::clicked, this, [this] { search(false); });
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(prev, &QPushButton::clicked, this, [this] { search(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setEnabled(false);
    row->addWidget(m_srchNote);
    return bar;
}

void DebuggerWindow::search(bool backward)
{
    uint16_t pat[64], mask, found;
    int flags;
    const int n = intvsearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(),
                                   pat, &mask, &flags);
    if (!n) {
        m_srchNote->setText(QStringLiteral("Type hex words (02A0 0268) or some text."));
        return;
    }
    if (!intvdebug_is_paused(m_dbg)) {
        m_srchNote->setText(QStringLiteral("Pause the machine to search its memory."));
        return;
    }
    const uint16_t from = m_memHl ? (backward ? m_memHlLo : uint16_t(m_memHlLo + 1)) : m_memBase;
    if (intvsearch_next(m_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found)) {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hex(found, 4)));
        showInMemory(found, uint16_t(found + n - 1));
    } else {
        m_srchNote->setText(QStringLiteral("Not found."));
    }
}
/* ---- Cart, Trace and History tabs, and peeling tabs off -------------------- */

QWidget *DebuggerWindow::buildCart()
{
    m_cartText = monoView();
    return m_cartText;
}

void DebuggerWindow::refreshCart()
{
    intvdebug_cart cart;
    intvdebug_region reg[64];
    QString t;

    intvdebug_cart_info(m_dbg, &cart);
    t += QStringLiteral("Cartridge\n");
    t += QStringLiteral("  File     %1\n").arg(cart.path[0] ? QString::fromUtf8(cart.path) : QStringLiteral("(none)"));
    if (cart.name[0])
        t += QStringLiteral("  Title    %1%2\n").arg(QString::fromUtf8(cart.name),
                                                      cart.year ? QStringLiteral(", %1").arg(cart.year) : QString());
    t += QStringLiteral("  FujiNet  %1\n").arg(cart.fujinet_mailbox ? QStringLiteral("mailbox mapped at $9C00")
                                                                    : QStringLiteral("no mailbox"));
    t += QStringLiteral("  ECS      %1\n").arg(cart.ecs ? QStringLiteral("present") : QStringLiteral("no"));
    t += QStringLiteral("  JLP      %1\n").arg(cart.jlp ? QStringLiteral("present") : QStringLiteral("no"));
    t += QStringLiteral("\nMemory map\n");
    const int n = intvdebug_memory_map(m_dbg, reg, 64);
    if (!n)
        t += QStringLiteral("  Pause the machine to read its memory map.\n");
    for (int i = 0; i < n; i++)
        t += QStringLiteral("  %1-%2  %3%4  %5\n")
                 .arg(hex(reg[i].lo, 4), hex(reg[i].hi, 4))
                 .arg(reg[i].readable ? QLatin1Char('r') : QLatin1Char('-'))
                 .arg(reg[i].writable ? QLatin1Char('w') : QLatin1Char('-'))
                 .arg(QString::fromUtf8(reg[i].name));
    setText(m_cartText, t);
}

QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);   /* the accent while on: the app's :checked style */
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync)
            return;
        intvdebug_history_enable(m_dbg, on);
        m_seenSerial = 0;
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                              QStringLiteral("In range")});
    m_hRange[idx] = new QLineEdit;
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a symbol"));
    auto changed = [this] {
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        m_seenSerial = 0;
    };
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this, changed);
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, changed);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        intvdebug_history_clear(m_dbg);
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        m_seenSerial = 0;
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setEnabled(false);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    layout->addLayout(bar);
    m_hText[idx] = monoView();
    layout->addWidget(m_hText[idx], 1);
    return page;
}

void DebuggerWindow::refreshHist(int idx, bool force)
{
    static intvdebug_hist buf[4000];
    constexpr int kShown = 400;
    const bool live = idx == 0;
    const bool paused = intvdebug_is_paused(m_dbg);
    const bool on = intvdebug_history_enabled(m_dbg);
    const uint64_t count = intvdebug_history_count(m_dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = 0;

    m_hSync = true;   /* the two Record buttons follow the engine */
    m_hRec[0]->setChecked(on);
    m_hRec[1]->setChecked(on);
    m_hSync = false;

    if (!force && count == m_hSeen[idx] && (live || paused))
        return;
    m_hSeen[idx] = count;
    if (!live && !paused) {
        m_hNote[idx]->setText(QStringLiteral("Pause the machine to read its history."));
        return;
    }
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = INTVDEBUG_HIST_BRANCHES; break;
    case 2:
        if (intvsym_parse_range(m_symtab, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi, nullptr))
            filter = INTVDEBUG_HIST_RANGE;
        break;
    }
    const int n = intvdebug_history_copy(m_dbg, buf, 4000);
    QList<int> hit;
    for (int i = n - 1; i >= 0 && hit.size() < kShown; i--)   /* the newest matches, scanning back */
        if (intvdebug_history_match(&buf[i], filter, lo, hi))
            hit.append(i);
    QString t;
    if (!on && n == 0)
        t = QStringLiteral("History is off. Press Record, then let the machine run.");
    for (int k = hit.size() - 1; k >= 0; k--) {   /* oldest first */
        char line[256];
        intvdebug_history_format(&buf[hit[k]], line, sizeof(line));
        t += QString::fromUtf8(line);
        t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(QStringLiteral("%1%2 instructions recorded, last %3 shown")
                              .arg(on ? QStringLiteral("Recording. ") : QString()).arg(count).arg(hit.size()));
}

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose)
            onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= kTabCount || m_popped[tab] || m_tabs->count() <= 1)
        return;   /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Intellivision Debugger – %1").arg(QLatin1String(kTabNames[tab])));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    win->onClose = [this, tab] {
        if (!m_closing)
            dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { pauseContinue(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { doStep(); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { doStepOver(); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { doStepOut(); });
    win->show();
    m_seenSerial = 0;
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win)
        return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; t++)   /* back where it was in the order */
        if (!m_popped[t])
            pos++;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QLatin1String(kTabNames[tab]));
    m_tabs->setCurrentIndex(pos);
    m_seenSerial = 0;
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
    QList<int> bpLines;
    int n = 0;
    if (m_basFile >= 0) {
        n = intvsrcmap_line_count(m_srcmap, m_basFile);
        text.reserve(n * 48);
        for (int i = 1; i <= n; i++) {
            const int addr = intvsrcmap_first_addr(m_srcmap, m_basFile, i);
            const char *src = intvsrcmap_line_text(m_srcmap, m_basFile, i);
            const int bp = addr >= 0 ? intvdebug_breakpoint_state(m_dbg, uint16_t(addr)) : 0;
            if (bp)
                bpLines.append(bp == 1 ? i - 1 : -(i - 1) - 1);   /* off: stored as -(line)-1 */
            text += QStringLiteral("%1 %2  %3\n")
                        .arg(dotFor(bp))
                        .arg(i, 5)
                        .arg(QString::fromUtf8(src ? src : ""));
        }
    }
    m_basGuard = true;   /* setPlainText moves the cursor: not a click */
    setText(m_basView, text);
    {
        QList<QTextEdit::ExtraSelection> sels;
        for (int b : bpLines)
            sels.append(b >= 0 ? bpDot(m_basView, b) : bpDot(m_basView, -b - 1, false));
        pcBar(sels, m_basView, m_basBar, pf == m_basFile && pl >= 1 && pl <= n ? pl - 1 : -1);
        m_basView->setExtraSelections(sels);
    }
    m_basGuard = false;

    const int line = pf == m_basFile ? pl : 0;
    /* Every new stop brings the current line into view, even when it is the
     * same line as last time and the reader has scrolled away. */
    const uint64_t ser = intvdebug_stop_serial(m_dbg);
    const bool newStop = paused && ser != m_basSerial;
    if (paused)
        m_basSerial = ser;
    if (line != m_basPcLine || (newStop && line)) {
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
    return tabVisible(kTabFujinet);
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

/* Shows a range in the Memory tab, highlighted, on a row boundary. */
void DebuggerWindow::showInMemory(uint16_t lo, uint16_t hi)
{
    m_memBase = uint16_t(lo & 0xFFF8);
    m_memHl = true;
    m_memHlLo = lo;
    m_memHlHi = hi;
    m_seenSerial = 0;
    if (intvdebug_is_paused(m_dbg))
        refreshMem();
}

void DebuggerWindow::goToAddr()
{
    uint16_t lo, hi;
    if (intvsym_parse_range(m_symtab, m_memAddr->text().toUtf8().constData(), &lo, &hi, nullptr))
        showInMemory(lo, hi);
    else
        m_status->setText(QStringLiteral("No such symbol, variable or address: %1").arg(m_memAddr->text()));
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
    QList<QTextEdit::ExtraSelection> sels;
    for (int i = 0; i < n; i++) {
        char symsuffix[96];
        intvsym_annotate_line(m_symtab, &lines[i], symsuffix,
                              sizeof(symsuffix));
        text += QStringLiteral("%1 %2  %3%4\n")
                    .arg(dotFor(intvdebug_breakpoint_state(m_dbg, lines[i].addr)))
                    .arg(lines[i].addr, 4, 16, QLatin1Char('0'))
                    .arg(QString::fromUtf8(lines[i].text), -24)
                    .arg(QString::fromUtf8(symsuffix));
    }
    m_disasm->setPlainText(text.toUpper());
    for (int i = 0; i < n; i++) {
        const int st = intvdebug_breakpoint_state(m_dbg, lines[i].addr);
        if (st)
            sels.append(bpDot(m_disasm, i, st == 1));
    }
    {
        int bar = -1;
        for (int i = 0; i < n; i++)
            if (lines[i].addr == r.pc)
                bar = i;
        pcBar(sels, m_disasm, m_disasmBar, bar);
    }
    m_disasm->setExtraSelections(sels);
    m_disasm->verticalScrollBar()->setValue(0);   /* it always starts at the PC */
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
    QList<QTextEdit::ExtraSelection> sels;
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
    if (m_memHl) {   /* the words of a variable, symbol or hit: the accent */
        for (int row = 0; row * kMemWordsPerRow < n; row++) {
            const QTextBlock b = m_memView->document()->findBlockByNumber(row);
            for (int col = 0; col < kMemWordsPerRow && row * kMemWordsPerRow + col < n; col++) {
                const uint16_t a = uint16_t(m_memBase + row * kMemWordsPerRow + col);
                if (a < m_memHlLo || a > m_memHlHi)
                    continue;
                QTextEdit::ExtraSelection sel;
                sel.cursor = QTextCursor(m_memView->document());
                sel.cursor.setPosition(b.position() + 6 + col * 5);
                sel.cursor.setPosition(b.position() + 6 + col * 5 + 4, QTextCursor::KeepAnchor);
                sel.format.setBackground(QColor(0x1E, 0x49, 0x12));
                sel.format.setForeground(Qt::white);
                sels.append(sel);
            }
        }
    }
    m_memView->setExtraSelections(sels);
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
    /* the action on offer wears the accent: Continue while stopped */
    m_pauseBtn->setProperty("accent", bool(paused));
    m_pauseBtn->style()->unpolish(m_pauseBtn);
    m_pauseBtn->style()->polish(m_pauseBtn);
    {
        intvdebug_stop_info st;
        QString why;
        if (paused && intvdebug_last_stop(m_dbg, &st) && st.reason != INTVDEBUG_STOP_BREAKPOINT)
            why = stopText(st);
        m_status->setText(!why.isEmpty() ? QStringLiteral("Paused: %1").arg(why)
                                         : paused ? QStringLiteral("Paused") : QStringLiteral("Running"));
    }

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
    refreshBreaks();
    refreshWatches();
    refreshCart();
    refreshHist(0, false);
    refreshHist(1, false);
    if (fujiShowing())
        refreshFujinet(false);
    m_wasPaused = paused;
}
