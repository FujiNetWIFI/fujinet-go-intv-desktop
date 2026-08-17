/*
 * Debugger window (Qt6): disassembly with click-to-toggle breakpoints,
 * editable registers, a memory view, and an STIC tab (BACKTAB, all 8 MOBs,
 * the GRAM/GROM card sheet with pager, palette, decoded register/mode
 * text) -- the same content as the GNOME debugger window
 * (frontends/gnome/debugger/dbg_window.c), built from plain Qt6 Widgets.
 *
 * Refreshing is a POLL (a QTimer watching intvdebug_stop_serial), not a
 * callback: intvdebug.h has no stop-callback mechanism at all (unlike the
 * MSX port's msxdebug_set_stop_callback) -- the engine stops the machine
 * synchronously inside cp1600_instr_tick on the emulator thread, and a UI
 * watches intvdebug_stop_serial()/intvdebug_is_paused() on its own timer
 * instead. Same reasoning the GNOME port's own header documents.
 *
 * The symbol table (core/debugger/symbols.h) is not owned by intvdebug --
 * unlike msxdebug's own -- so this window creates and owns its own
 * intvsymtab instance purely for disassembly annotation, destroyed when
 * the window closes. (No "Load Symbols" UI yet -- the GNOME port's own has
 * one; porting it here is a straightforward follow-up, not done in this
 * pass to keep the KDE port's own scope matched to what's verified.)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "symbols.h"

namespace {
constexpr int kDisasmLines = 32;
constexpr int kMemWordsPerRow = 8;
constexpr int kMemRows = 16;
constexpr int kCardRowsPerPage = 8; /* 128 cards/page at CARDS_PER_ROW=16 */

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
    (void)m_session;
    m_dbg = intvdebug_get();
    m_symtab = intvsymtab_create();
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Intellivision Debugger"));
    resize(1080, 780);
    buildUi();

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
        }
    });
    m_tick->start(33);
    refreshAll();
}

DebuggerWindow::~DebuggerWindow()
{
    /* Releasing costs the machine nothing further (see intvdebug.h) and
     * leaving it engaged+paused would look like the emulator had hung. */
    intvdebug_set_engaged(m_dbg, 0);
    intvsymtab_destroy(m_symtab);
}

void DebuggerWindow::buildUi()
{
    auto *tb = addToolBar(QStringLiteral("Control"));
    tb->setMovable(false);

    m_pauseBtn = new QPushButton(QStringLiteral("Pause (F5)"));
    connect(m_pauseBtn, &QPushButton::clicked, this,
            &DebuggerWindow::pauseContinue);
    tb->addWidget(m_pauseBtn);

    auto *step = new QPushButton(QStringLiteral("Step (F7)"));
    connect(step, &QPushButton::clicked, this,
            [this] { intvdebug_step(m_dbg); });
    tb->addWidget(step);
    auto *stepOver = new QPushButton(QStringLiteral("Step Over (F8)"));
    connect(stepOver, &QPushButton::clicked, this,
            [this] { intvdebug_step_over(m_dbg); });
    tb->addWidget(stepOver);
    auto *stepOut = new QPushButton(QStringLiteral("Step Out (Shift+F8)"));
    connect(stepOut, &QPushButton::clicked, this,
            [this] { intvdebug_step_out(m_dbg); });
    tb->addWidget(stepOut);
    auto *clearBp = new QPushButton(QStringLiteral("Clear Breakpoints"));
    connect(clearBp, &QPushButton::clicked, this, [this] {
        intvdebug_breakpoint_clear_all(m_dbg);
        m_seenSerial = 0;
    });
    tb->addWidget(clearBp);

    m_status = new QLabel(QStringLiteral("Running"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(m_status);

    new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { pauseContinue(); });
    new QShortcut(QKeySequence(Qt::Key_F7), this,
                  [this] { intvdebug_step(m_dbg); });
    new QShortcut(QKeySequence(Qt::Key_F8), this,
                  [this] { intvdebug_step_over(m_dbg); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this,
                  [this] { intvdebug_step_out(m_dbg); });

    auto *tabs = new QTabWidget;
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
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const uint16_t addr = uint16_t(m_memAddr->text().toUInt(&ok, 16));
        if (ok) {
            m_memBase = addr;
            m_seenSerial = 0;
        }
    });
    memLayout->addWidget(m_memAddr);
    m_memView = monoView();
    memLayout->addWidget(m_memView);
    rightLayout->addWidget(memBox, 1);

    cpuLayout->addWidget(right);
    tabs->addTab(cpu, QStringLiteral("CPU"));

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
    tabs->addTab(sticScroll, QStringLiteral("STIC"));
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
        char symname[32];
        QString symsuffix;
        if (intvsymtab_lookup_addr(m_symtab, lines[i].addr, symname,
                                   sizeof(symname)))
            symsuffix = QStringLiteral("   ; %1").arg(
                QString::fromUtf8(symname));
        text += QStringLiteral("%1%2 %3  %4%5\n")
                    .arg(lines[i].addr == r.pc ? QLatin1Char('>')
                                               : QLatin1Char(' '))
                    .arg(intvdebug_breakpoint_is_set(m_dbg, lines[i].addr)
                             ? QLatin1Char('*')
                             : QLatin1Char(' '))
                    .arg(lines[i].addr, 4, 16, QLatin1Char('0'))
                    .arg(QString::fromUtf8(lines[i].text), -24)
                    .arg(symsuffix);
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
    for (int row = 0; row * kMemWordsPerRow < n; row++) {
        text += QStringLiteral("%1 ").arg(
            uint16_t(m_memBase + row * kMemWordsPerRow), 4, 16,
            QLatin1Char('0')).toUpper();
        for (int col = 0; col < kMemWordsPerRow; col++) {
            const int idx = row * kMemWordsPerRow + col;
            if (idx < n)
                text += QStringLiteral(" %1").arg(words[idx], 4, 16,
                                                  QLatin1Char('0')).toUpper();
        }
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
    m_wasPaused = paused;
}
