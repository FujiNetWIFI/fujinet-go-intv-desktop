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
#include "Accent.h"
#include "VideoTab.h"

#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QShortcut>
#include <QPointer>
#include <QStatusBar>
#include <QStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <QCloseEvent>
#include <QRegularExpression>
#include <cmath>
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
constexpr int kFujiTicks = 15;      /* the FujiNet tab, 500 ms at 33 ms */
constexpr int kConsoleLines = 2000;

/* The tabs, in order; INTV_DEBUGGER_TAB and INTV_POPOUT pick one by number or
 * by the start of its label (any case), for tests and captures. The older
 * names still work: cpu is the Disassembly, stic the Video tab. */
const char *const kTabNames[] = {"Console", "Disassembly", "BASIC", "Memory", "Breakpoints", "Watches",
                                 "Cart & I/O", "Trace", "History", "Video", "Sound", "Symbols", "FujiNet"};
enum { kTabConsole, kTabDisasm, kTabBasic, kTabMemory, kTabBreaks, kTabWatches, kTabCart, kTabTrace,
       kTabHistory, kTabVideo, kTabSound, kTabSymbols, kTabFujinet };
enum { kPickFile, kPickFolder, kPickMap };
constexpr int kTabCount = int(sizeof(kTabNames) / sizeof(kTabNames[0]));
/* Help contexts (intvhelp.h), one per tab */
const char *const kHelpTabs[kTabCount] = {"console", "disassembly", "basic", "memory", "breakpoints", "watches",
                                          "cart", "trace", "history", "video", "sound", "symbols", "fujinet"};

/* The label a tab shows: "&" starts a mnemonic in tab text, so it is doubled. */
QString tabLabel(int tab)
{
    return QString::fromLatin1(kTabNames[tab]).replace(QLatin1Char('&'), QStringLiteral("&&"));
}

int tabFromEnv(const char *var = "INTV_DEBUGGER_TAB")
{
    const char *tab = std::getenv(var);
    if (!tab || !*tab)
        return -1;
    char *end;
    const long n = std::strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < kTabCount ? int(n) : -1;
    static const struct { const char *name; int tab; } kAliases[] = {{"cpu", kTabDisasm}, {"stic", kTabVideo}};
    for (const auto &alias : kAliases)
        if (qstrnicmp(tab, alias.name, uint(std::strlen(tab))) == 0 && std::strlen(tab) >= 3)
            return alias.tab;
    for (int i = 0; i < kTabCount; i++)
        if (qstrnicmp(tab, kTabNames[i], uint(std::strlen(tab))) == 0)
            return i;
    return -1;
}

QString hex(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
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
    setBg(block, intvAccentFill(v));
    prev = block;

    QTextEdit::ExtraSelection text;
    const QTextBlock b = doc->findBlockByNumber(block);
    text.cursor = QTextCursor(b);
    text.cursor.setPosition(b.position() + 1);
    text.cursor.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    text.format.setForeground(intvAccentFillInk(v));
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
    resize(1280, 780);
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
            std::snprintf(bp.condition, sizeof(bp.condition), "value != old");
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
        /* one critical section: the stop, its serial and the paused flag agree */
        intvdebug_snapshot snap;
        intvdebug_stop_snapshot(m_dbg, &snap);
        const unsigned gen = intvdebug_generation(m_dbg);
        if (snap.serial != m_seenSerial || snap.paused != m_wasPaused || gen != m_seenGen) {
            m_seenSerial = snap.serial;
            m_seenGen = gen;
            refreshAll();
        } else if (tabVisible(kTabVideo)) {
            refreshVideo(false);   /* a running machine publishes a frame at a time */
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
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    /* in the status bar: the toolbar has no room left for it */
    statusBar()->addWidget(m_status, 1);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addWidget(intvHelpButton(QString()));

    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { intvHelpFor(this); });
    new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { pauseContinue(); });
    new QShortcut(QKeySequence(Qt::Key_F7), this, [this] { doStep(); });
    new QShortcut(QKeySequence(Qt::Key_F8), this, [this] { doStepOver(); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this, [this] { doStepOut(); });

    auto *tabs = new QTabWidget;
    m_tabs = tabs;

    /* ---- Disassembly tab ---- */
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

    /* The registers (editable) sit under every tab. */
    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->addWidget(tabs, 1);
    centralLayout->addWidget(buildRegs());
    setCentralWidget(central);

    tabs->addTab(buildConsole(), tabLabel(kTabConsole));
    tabs->addTab(cpu, tabLabel(kTabDisasm));
    tabs->addTab(buildBasic(), tabLabel(kTabBasic));
    tabs->addTab(buildMemory(), tabLabel(kTabMemory));
    tabs->addTab(buildBreaks(), tabLabel(kTabBreaks));
    tabs->addTab(buildWatches(), tabLabel(kTabWatches));
    tabs->addTab(buildCart(), tabLabel(kTabCart));
    tabs->addTab(buildHist(0), tabLabel(kTabTrace));
    tabs->addTab(buildHist(1), tabLabel(kTabHistory));
    tabs->addTab(buildVideo(), tabLabel(kTabVideo));
    tabs->addTab(buildSound(), tabLabel(kTabSound));

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

    tabs->addTab(symTab, tabLabel(kTabSymbols));

    m_fujiPage = buildFujinet();
    tabs->addTab(m_fujiPage, tabLabel(kTabFujinet));
    for (int i = 0; i < kTabCount; i++) {   /* each page keeps its identity */
        QWidget *pg = tabs->widget(i);
        pg->setProperty("tab", i);
        m_pages[i] = pg;
    }
    /* Help's contexts (intvhelp.h), one per tab */
    {
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
        if (tabVisible(kTabVideo))
            refreshVideo(true);
    });
}

/* ---- Console, the register strip, Memory, Video and Sound ------------------------ */

QWidget *DebuggerWindow::buildConsole()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(kConsoleLines);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $5000 or 0x5000 "
                                          "for hex, bare digits are decimal.\n"));
    m_conIn = new QLineEdit;
    QFont f = m_conIn->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    m_conIn->setFont(f);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, bpa, print, mem, poke, runto, "
                                               "disasm, watch, find, video, cart ...) -- Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    layout->addWidget(m_conOut, 1);
    layout->addWidget(m_conIn);
    return page;
}

void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow)
        m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty())
        return;
    if (m_history.isEmpty() || m_history.last() != cmd)
        m_history << cmd;
    m_historyPos = int(m_history.size());
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    intvdebug_command(m_dbg, cmd.toUtf8().constData(), out, int(sizeof(out)));
    QString text = QString::fromUtf8(out);
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0B-\\x1F]")));
    if (!text.isEmpty())
        consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    m_seenSerial = 0;   /* a command may have moved anything */
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    const int sp = int(text.lastIndexOf(QLatin1Char(' ')));
    const QString head = text.left(sp + 1);
    comps[0] = '\0';
    if (intvdebug_completions(m_dbg, text.mid(sp + 1).toUtf8().constData(), comps, int(sizeof(comps))) <= 0)
        return;
    const QStringList words = QString::fromUtf8(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return;
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common))
            common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1)
        m_conIn->setText(head + common);
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0)
                --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size())
                ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

/* A number the way the prompt takes it: bare hex digits are hex (as the
 * register and value entries always were), anything else goes to the
 * engine's expression evaluator ($5000, 20480, a label, r0 + 1 ...). */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (why) *why = QStringLiteral("Type a value");
        return false;
    }
    if (hexOnly.match(t).hasMatch()) {
        *out = t.toLong(nullptr, 16);
        return true;
    }
    char err[160] = "";
    if (intvdebug_eval(m_dbg, t.toUtf8().constData(), out, err, int(sizeof(err))) == 0)
        return true;
    if (why) *why = QString::fromUtf8(err);
    else showNote(QString::fromUtf8(err));
    return false;
}

/* The registers, under every tab. Enter sets one (only while stopped); a
 * flag takes 0 or 1. D is the SDBD prefix pending and cannot be typed. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *grid = new QGridLayout(box);
    grid->setVerticalSpacing(2);
    static const char *const kRegNames[8] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6 (SP)", "R7 (PC)"};
    auto narrow = [](QLineEdit *e, int chars) {
        QFont f = e->font();
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::TypeWriter);
        e->setFont(f);
        e->setMaximumWidth(e->fontMetrics().horizontalAdvance(QLatin1Char('0')) * (chars + 3) + 16);
        e->setMaxLength(chars + 10);   /* room for an expression */
    };
    for (int i = 0; i < 8; i++) {
        grid->addWidget(new QLabel(QLatin1String(kRegNames[i])), 0, i * 2, Qt::AlignRight);
        m_regEdit[i] = new QLineEdit;
        narrow(m_regEdit[i], 4);
        m_regEdit[i]->setToolTip(QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
        connect(m_regEdit[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i); });
        grid->addWidget(m_regEdit[i], 0, i * 2 + 1);
    }
    static const char *const kFlagNames[5] = {"S", "Z", "O", "C", "I"};
    static const char *const kFlagTips[5] = {"Sign", "Zero", "Overflow", "Carry", "Interrupt enable"};
    for (int i = 0; i < 5; i++) {
        grid->addWidget(new QLabel(QLatin1String(kFlagNames[i])), 1, i * 2, Qt::AlignRight);
        m_flagEdit[i] = new QLineEdit;
        narrow(m_flagEdit[i], 1);
        m_flagEdit[i]->setToolTip(QStringLiteral("%1 flag: Enter sets it to 0 or 1 (only while stopped)")
                                      .arg(QLatin1String(kFlagTips[i])));
        connect(m_flagEdit[i], &QLineEdit::returnPressed, this, [this, i] { setFlag(i); });
        grid->addWidget(m_flagEdit[i], 1, i * 2 + 1);
    }
    m_flags = new QLabel;
    QFont mono = m_flags->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_flags->setFont(mono);
    grid->addWidget(m_flags, 1, 10, 1, 6);
    grid->setColumnStretch(16, 1);
    return box;
}

void DebuggerWindow::setReg(int reg)
{
    QLineEdit *e = m_regEdit[reg];
    long v = 0;
    QString why;
    if (!intvdebug_is_paused(m_dbg))
        showNote(QStringLiteral("Stop the machine first"));
    else if (!numberOf(e->text(), &v, &why))
        showNote(why);
    else
        intvdebug_regs_set(m_dbg, INTVDEBUG_REG_R0 + reg, unsigned(v) & 0xFFFF);
    e->clearFocus();
    m_seenSerial = 0;
    if (intvdebug_is_paused(m_dbg))
        refreshAll();
}

void DebuggerWindow::setFlag(int flag)
{
    static const int ids[5] = {INTVDEBUG_REG_S, INTVDEBUG_REG_Z, INTVDEBUG_REG_O, INTVDEBUG_REG_C, INTVDEBUG_REG_I};
    QLineEdit *e = m_flagEdit[flag];
    long v = 0;
    QString why;
    if (!intvdebug_is_paused(m_dbg))
        showNote(QStringLiteral("Stop the machine first"));
    else if (!numberOf(e->text(), &v, &why))
        showNote(why);
    else
        intvdebug_regs_set(m_dbg, ids[flag], v ? 1u : 0u);
    e->clearFocus();
    m_seenSerial = 0;
    if (intvdebug_is_paused(m_dbg))
        refreshAll();
}

/* The dump, Go to, the Poke bar and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *memTab = new QWidget;
    auto *memLayout = new QVBoxLayout(memTab);
    m_memAddr = new QLineEdit;
    m_memAddr->setText(QStringLiteral("0000"));
    m_memAddr->setPlaceholderText(QStringLiteral("Address, symbol or BASIC variable"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, &DebuggerWindow::goToAddr);
    memLayout->addWidget(m_memAddr);

    auto *poke = new QWidget;
    auto *pokeRow = new QHBoxLayout(poke);
    pokeRow->setContentsMargins(0, 0, 0, 0);
    pokeRow->addWidget(new QLabel(QStringLiteral("Poke:")));
    m_pokeAddr = new QLineEdit;
    m_pokeAddr->setPlaceholderText(QStringLiteral("address or symbol"));
    m_pokeVal = new QLineEdit;
    m_pokeVal->setPlaceholderText(QStringLiteral("word (hex or expression)"));
    m_pokeAddr->setToolTip(QStringLiteral("The word to write: a 16-bit word, or a byte in the 8-bit areas "
                                          "(scratchpad, GROM/GRAM, ECS RAM). Only while stopped."));
    auto *pokeBtn = new QPushButton(QStringLiteral("Poke"));
    connect(m_pokeAddr, &QLineEdit::returnPressed, this, &DebuggerWindow::pokeMem);
    connect(m_pokeVal, &QLineEdit::returnPressed, this, &DebuggerWindow::pokeMem);
    connect(pokeBtn, &QPushButton::clicked, this, &DebuggerWindow::pokeMem);
    pokeRow->addWidget(m_pokeAddr, 1);
    pokeRow->addWidget(m_pokeVal, 1);
    pokeRow->addWidget(pokeBtn);
    memLayout->addWidget(poke);

    memLayout->addWidget(buildSearchBar());
    m_memView = monoView();
    memLayout->addWidget(m_memView, 1);
    return memTab;
}

void DebuggerWindow::pokeMem()
{
    long addr = 0, val = 0;
    QString why;
    uint16_t lo, hi;
    if (!intvdebug_is_paused(m_dbg)) {
        showNote(QStringLiteral("Stop the machine first"));
        return;
    }
    if (!intvsym_parse_range(m_symtab, m_pokeAddr->text().toUtf8().constData(), &lo, &hi, nullptr)) {
        showNote(QStringLiteral("No such symbol, variable or address: %1").arg(m_pokeAddr->text()));
        return;
    }
    addr = lo;
    if (!numberOf(m_pokeVal->text(), &val, &why)) {
        showNote(why);
        return;
    }
    const int bits = intvdebug_word_width(m_dbg, uint16_t(addr));
    const long limit = bits == 8 ? 0xFF : 0xFFFF;
    if (val < 0 || val > limit) {
        showNote(QStringLiteral("The value is %1 ($0-$%2)")
                              .arg(bits == 8 ? QStringLiteral("a byte here") : QStringLiteral("a 16-bit word"))
                              .arg(hex(unsigned(limit), bits == 8 ? 2 : 4)));
        return;
    }
    const uint16_t word = uint16_t(val);
    intvdebug_write(m_dbg, uint16_t(addr), &word, 1);
    showNote(QStringLiteral("Wrote $%1 to $%2").arg(hex(word, bits == 8 ? 2 : 4), hex(uint16_t(addr), 4)));
    m_seenSerial = 0;
    refreshAll();
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

void DebuggerWindow::refreshVideo(bool force)
{
    if (m_video)
        m_video->refresh(force || m_videoForce);
    m_videoForce = false;
}

QWidget *DebuggerWindow::buildSound()
{
    m_soundText = monoView();
    return m_soundText;
}

/* The two AY-3-8914s, read straight out of the chips (stopped only). */
void DebuggerWindow::refreshSound()
{
    QString t;
    if (!intvdebug_is_paused(m_dbg)) {
        setText(m_soundText, QStringLiteral("Pause the machine to read the sound chips."));
        return;
    }
    static const char *const kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int unit = 0; unit < 2; unit++) {
        intvdebug_psg p;
        if (!intvdebug_sound_get(m_dbg, unit, &p) || !p.present) {
            if (unit == 1)
                t += QStringLiteral("\nECS PSG ($00F0): not present (no ECS)\n");
            continue;
        }
        t += unit == 0 ? QStringLiteral("PSG 0, AY-3-8914 at $01F0\n") : QStringLiteral("\nECS PSG 1, AY-3-8914 at $00F0\n");
        for (int v = 0; v < 3; v++) {
            QString pitch = QStringLiteral("-");
            if (p.tone_on[v] && p.period[v] > 0) {
                const double midi = 69.0 + 12.0 * std::log2(p.hz[v] / 440.0);
                const long note = std::lround(midi);
                pitch = QStringLiteral("%1 Hz  %2%3 (%4 cents)")
                            .arg(p.hz[v], 0, 'f', 1)
                            .arg(QLatin1String(kNotes[((note % 12) + 12) % 12]))
                            .arg(note / 12 - 1)
                            .arg(std::lround((midi - double(note)) * 100.0));
            }
            t += QStringLiteral("  %1  period %2  tone %3  noise %4  level %5%6  now %7  %8\n")
                     .arg(QLatin1Char(char('A' + v)))
                     .arg(p.period[v], 4)
                     .arg(p.tone_on[v] ? QStringLiteral("on ") : QStringLiteral("off"))
                     .arg(p.noise_on[v] ? QStringLiteral("on ") : QStringLiteral("off"))
                     .arg(p.level[v], 2)
                     .arg(p.envelope[v] ? QStringLiteral(" (envelope)") : QString())
                     .arg(p.channel_state[v] ? QStringLiteral("high") : QStringLiteral("low "))
                     .arg(pitch);
        }
        t += QStringLiteral("  Noise period %1   Envelope period %2, shape %3, level now %4\n")
                 .arg(p.noise_period).arg(p.env_period).arg(hex(unsigned(p.env_shape), 1)).arg(p.env_level);
        t += QStringLiteral("  Registers:");
        for (int r = 0; r < 14; r++)
            t += QStringLiteral(" %1").arg(hex(p.reg[r], 4));
        t += QStringLiteral("\n  (the mixer, register 8, is active-low: a set bit turns that voice off)\n");
    }
    setText(m_soundText, t);
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
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional): value != old, r0 == $12, [$0200] > 3"));
    m_bpCond->setToolTip(QStringLiteral(
        "An expression: numbers ($5000 or 20480), r0-r7 pc sp, flags s z o c i d, labels, [addr], frame, cycles, "
        "and for a read or write value, addr and old (the word before a write). Empty stops every time."));
    connect(m_bpCond, &QLineEdit::returnPressed, this, &DebuggerWindow::addBreak);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, &DebuggerWindow::addBreak);
    form->addWidget(m_bpType);
    form->addWidget(m_bpAddr, 1);
    form->addWidget(m_bpCond, 1);
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
    auto *editCond = new QPushButton(QStringLiteral("Edit Condition…"));
    editCond->setToolTip(QStringLiteral("Change the selected breakpoint's condition"));
    connect(editCond, &QPushButton::clicked, this, &DebuggerWindow::editCondition);
    auto *removeAll = new QPushButton(QStringLiteral("Remove All"));
    connect(removeAll, &QPushButton::clicked, this, [this] {
        intvdebug_breakpoint_clear_all(m_dbg);
        m_seenSerial = 0;
    });
    foot->addWidget(remove);
    foot->addWidget(editCond);
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
        showNote(QStringLiteral("No such symbol, variable or address: %1").arg(m_bpAddr->text()));
        return;
    }
    if (type == 0)
        hi = lo;
    bp.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE);
    bp.lo = lo;
    bp.hi = hi;
    std::snprintf(bp.condition, sizeof(bp.condition), "%s", m_bpCond->text().trimmed().toUtf8().constData());
    if (intvdebug_bp_add(m_dbg, &bp) < 0) {
        showNote(bp.condition[0] ? QStringLiteral("The breakpoint table is full, or the condition does not parse: %1")
                                                .arg(QString::fromUtf8(bp.condition))
                                          : QStringLiteral("The breakpoint table is full"));
        return;
    }
    m_bpAddr->clear();
    m_bpCond->clear();
    m_seenSerial = 0;
}

void DebuggerWindow::editCondition()
{
    QListWidgetItem *it = m_bpList->currentItem();
    if (!it) {
        showNote(QStringLiteral("Select a breakpoint first"));
        return;
    }
    const int id = it->data(Qt::UserRole).toInt();
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    const int n = intvdebug_bp_list(m_dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
    QString current;
    for (int i = 0; i < n; i++)
        if (bps[i].id == id)
            current = QString::fromUtf8(bps[i].condition);
    bool ok = false;
    const QString text = QInputDialog::getText(this, QStringLiteral("Breakpoint Condition"),
                                               QStringLiteral("Condition (empty for none):"), QLineEdit::Normal, current, &ok);
    if (!ok)
        return;
    if (!intvdebug_bp_set_condition(m_dbg, id, text.trimmed().toUtf8().constData()))
        showNote(QStringLiteral("That condition does not parse: %1").arg(text));
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
    intvdebug_snapshot snap;
    intvdebug_stop_snapshot(m_dbg, &snap);
    m_bpStop->setText(snap.paused ? QStringLiteral("Stopped: %1").arg(QString::fromUtf8(snap.text))
                                  : QStringLiteral("Running."));
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
        std::snprintf(bp.condition, sizeof(bp.condition), "value != old");
        if (intvdebug_bp_add(m_dbg, &bp) >= 0)
            showNote(QStringLiteral("Breaking when it changes"));
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
        showNote(QStringLiteral("Type an address, symbol or BASIC variable (x, #score, a(3))"));
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

/* The cartridge and its memory map above, the I/O below (controllers, PSG
 * ports, ECS keyboard, Intellivoice, JLP). */
QWidget *DebuggerWindow::buildCart()
{
    m_cartText = monoView();
    m_ioText = monoView();
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_cartText);
    split->addWidget(m_ioText);
    split->setChildrenCollapsible(false);
    split->setSizes({400, 300});
    return split;
}

namespace {
QString yesNo(int v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); }

/* The nonzero event inputs of a controller: [index]=value. */
QString padEvents(const uint32_t *v, int n)
{
    QString t;
    for (int i = 0; i < n; i++)
        if (v[i])
            t += QStringLiteral(" [%1]=%2").arg(i).arg(v[i], 0, 16);
    return t.isEmpty() ? QStringLiteral(" idle") : t;
}
} // namespace

void DebuggerWindow::refreshIo()
{
    intvdebug_io io;
    QString t;
    if (!intvdebug_io_get(m_dbg, &io)) {
        setText(m_ioText, QStringLiteral("No machine running."));
        return;
    }
    t += QStringLiteral("I/O\n");
    for (int i = 0; i < 2; i++) {
        const intvdebug_pad &pad = io.pad[i];
        t += QStringLiteral("\n%1 controllers%2\n").arg(i ? QStringLiteral("ECS") : QStringLiteral("Console"))
                 .arg(pad.present ? QString() : QStringLiteral(" (not present)"));
        if (!pad.present)
            continue;
        const unsigned base = i ? 0x00FE : 0x01FE;
        t += QStringLiteral("  PSG port $%1 (left)  %2   $%3 (right)  %4\n")
                 .arg(hex(base, 4), hex(pad.side[0], 2), hex(base + 1, 4), hex(pad.side[1], 2));
        if (i)
            t += QStringLiteral("  output: left %1, right %2\n").arg(yesNo(pad.output[0]), yesNo(pad.output[1]));
        t += QStringLiteral("  left pad: %1\n  right pad:%2\n").arg(padEvents(pad.left, 18), padEvents(pad.right, 18));
        if (i) {
            t += QStringLiteral("  keyboard rows:");
            for (int r = 0; r < 8; r++)
                t += QStringLiteral(" %1").arg(hex(pad.keys[r], 2));
            t += QLatin1Char('\n');
        }
    }
    t += QStringLiteral("\nECS: %1").arg(io.ecs ? QStringLiteral("present") : QStringLiteral("no"));
    if (io.ecs)
        t += QStringLiteral("   ROM windows $2xxx $7xxx $Exxx: page %1 %2 %3")
                 .arg(io.ecs_page[0]).arg(io.ecs_page[1]).arg(io.ecs_page[2]);
    t += QStringLiteral("\nIntellivoice: %1").arg(io.ivoice ? QStringLiteral("present") : QStringLiteral("no"));
    if (io.ivoice)
        t += QStringLiteral("\n  LRQ %1  ALD %2  PC %3  %4  mode %5  FIFO %6/%7\n")
                 .arg(io.ivoice_lrq).arg(io.ivoice_ald).arg(hex(unsigned(io.ivoice_pc), 4))
                 .arg(io.ivoice_halted ? QStringLiteral("halted") : QStringLiteral("running"))
                 .arg(hex(io.ivoice_mode, 2)).arg(io.ivoice_fifo_head).arg(io.ivoice_fifo_tail);
    t += QStringLiteral("\nJLP: %1").arg(io.jlp ? QStringLiteral("present") : QStringLiteral("no"));
    if (io.jlp)
        t += QStringLiteral("\n  accelerators ($9F80-$9FFF) %1   flash %2%3   FujiNet mailbox hole in the window: %4")
                 .arg(io.jlp_accel ? QStringLiteral("answer") : QStringLiteral("off"))
                 .arg(io.jlp_flash ? QStringLiteral("open") : QStringLiteral("none"))
                 .arg(io.jlp_flash ? QStringLiteral(" (%1 bytes)").arg(io.jlp_flash_bytes) : QString())
                 .arg(yesNo(io.jlp_mb_hole));
    t += QStringLiteral("\nLocutus mapper: %1\nFujiNet mailbox: %2\n").arg(yesNo(io.locutus), yesNo(io.fujinet_mailbox));
    setText(m_ioText, t);
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
    refreshIo();
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
    m_tabs->insertTab(pos, page, tabLabel(tab));
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
        showNote(QStringLiteral("No code on that line"));
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
        showNote(QStringLiteral("No such symbol, variable or address: %1").arg(m_memAddr->text()));
}

void DebuggerWindow::breakAtAddr()
{
    uint16_t addr;
    if (intvsym_parse_addr(m_symtab, m_breakAddr->text().toUtf8().constData(),
                           &addr)) {
        intvdebug_breakpoint_toggle(m_dbg, addr);
        m_seenSerial = 0; /* redraw the marker immediately */
    } else {
        showNote(QStringLiteral("No such symbol or address: %1")
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
    /* a field being typed into keeps its text */
    auto put = [](QLineEdit *e, const QString &text) {
        if (!e->hasFocus())
            e->setText(text);
    };
    if (!intvdebug_regs_get(m_dbg, &r)) {
        for (auto &e : m_regEdit) put(e, QString());
        for (auto &e : m_flagEdit) put(e, QString());
        m_flags->setText(QString());
        return;
    }
    for (int i = 0; i < 8; i++)
        put(m_regEdit[i], hex(r.r[i], 4));
    const int flags[5] = {r.S, r.Z, r.O, r.C, r.I};
    for (int i = 0; i < 5; i++)
        put(m_flagEdit[i], QString::number(flags[i] ? 1 : 0));
    m_flags->setText(QStringLiteral("D %1%2   %3")
                         .arg(r.D)
                         .arg(r.D ? QStringLiteral(" (SDBD pending)") : QString())
                         .arg(r.interruptible ? QStringLiteral("interruptible") : QStringLiteral("INTR masked")));
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
        text += hex(rowAddr, 4) + QLatin1Char(' ');
        /* every cell is five characters; a byte-wide word is two digits at its right */
        for (int col = 0; col < kMemWordsPerRow; col++) {
            const int idx = row * kMemWordsPerRow + col;
            if (idx >= n)
                break;
            const int bits = intvdebug_word_width(m_dbg, uint16_t(rowAddr + col));
            text += bits == 8 ? QStringLiteral("   ") + hex(words[idx] & 0xFF, 2)
                              : QLatin1Char(' ') + hex(words[idx], 4);
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
                sel.format.setBackground(intvAccentFill(m_memView));
                sel.format.setForeground(intvAccentFillInk(m_memView));
                sels.append(sel);
            }
        }
    }
    m_memView->setExtraSelections(sels);
}

void DebuggerWindow::refreshAll()
{
    intvdebug_snapshot snap;
    intvdebug_stop_snapshot(m_dbg, &snap);
    const int paused = snap.paused;
    m_pauseBtn->setText(paused ? QStringLiteral("Continue (F5)")
                               : QStringLiteral("Pause (F5)"));
    /* the action on offer wears the accent: Continue while stopped */
    m_pauseBtn->setProperty("accent", bool(paused));
    m_pauseBtn->style()->unpolish(m_pauseBtn);
    m_pauseBtn->style()->polish(m_pauseBtn);
    refreshStatus(snap);

    if (paused) {
        refreshRegs();
        refreshDisasm();
        refreshMem();
    } else {
        refreshRegs();   /* clears the fields */
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
    refreshSound();
    refreshHist(0, false);
    refreshHist(1, false);
    if (tabVisible(kTabVideo))
        refreshVideo(true);
    if (fujiShowing())
        refreshFujinet(false);
    m_wasPaused = paused;
}

/* The core words why the machine stopped, so every window agrees. */
/* A message that stays a few seconds: the next refresh would wipe the status line. */
void DebuggerWindow::showNote(const QString &text)
{
    statusBar()->showMessage(text, 6000);
}

void DebuggerWindow::refreshStatus(const intvdebug_snapshot &snap)
{
    m_status->setText(snap.paused ? QString::fromUtf8(snap.text) : QStringLiteral("Running"));
}
