/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform.
 *
 * Without the system ROMs (WITH_INTV_ROMS=OFF, as in CI) no machine can run,
 * so only what needs none is checked: the thirteen tabs and their order, the
 * INTV_DEBUGGER_TAB names and the older aliases, the Help contexts, the
 * minimum height, pop-out and dock. With the ROMs the machine is stopped and
 * a small program of NOP-padded instructions is written into system RAM
 * ($0310) and stepped through the window: the register strip, the Poke bar,
 * the console, a breakpoint with an expression condition, the Video and Sound
 * tabs and the Cart & I/O text.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>

#include "intvdebug.h"
#include "intvsession.h"
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"

namespace {

/* In system RAM at $0310, NOP-padded so a backward disassembly guess is
 * stable: NOP x3, MVII #$1234,R0, NOP, MVO R0,$0320, NOP x3. */
const uint16_t kProg[] = {0x0034, 0x0034, 0x0034, 0x02B8, 0x1234, 0x0034, 0x0240, 0x0320, 0x0034, 0x0034, 0x0034};
const int kStart = 0x0310;
const int kMvii = 0x0313;

QStringList expectedTabs()
{
    return {QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("BASIC"),
            QStringLiteral("Memory"), QStringLiteral("Breakpoints"), QStringLiteral("Watches"),
            QStringLiteral("Cart & I/O"), QStringLiteral("Trace"), QStringLiteral("History"),
            QStringLiteral("Video"), QStringLiteral("Sound"), QStringLiteral("Symbols"),
            QStringLiteral("FujiNet")};
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    intvsession *m_s = nullptr;
    intvdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    bool m_machine = false;

    /* a closed window deletes itself: run that, or showFor() finds it again */
    static void reap()
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QString tabText(int i) const { return m_tabs->tabText(i).replace(QStringLiteral("&&"), QStringLiteral("&")); }
    QPushButton *button(const QString &start)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(start)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QPushButton *addButton()   /* the Breakpoints tab's, not the Watches tab's */
    {
        for (QPushButton *b : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) return b;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) out << e;
        return out;
    }
    void waitPaused()
    {
        QTRY_VERIFY_WITH_TIMEOUT(intvdebug_is_paused(m_d), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Continue")) != nullptr, 2000);
    }
    int pc()
    {
        intvdebug_regs r;
        return intvdebug_regs_get(m_d, &r) ? r.pc : -1;
    }
    void loadProgram()
    {
        intvdebug_write(m_d, kStart, kProg, int(sizeof(kProg) / sizeof(kProg[0])));
        intvdebug_regs_set(m_d, INTVDEBUG_REG_I, 0);   /* no interrupt in the middle of a step */
        intvdebug_regs_set(m_d, INTVDEBUG_REG_PC, kStart);
    }
    QString consoleText()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v->toPlainText();
        return QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        intvsession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        m_s = intvsession_new(&paths);
        QVERIFY(m_s);
        m_d = intvdebug_get();
        QVERIFY(m_d);

        m_machine = intvsession_has_system_roms(m_s) != 0;
        if (m_machine) {
            intvsession_start_opts opts;
            intvsession_default_opts(m_s, &opts);
            opts.cart_path = nullptr;
            opts.external_fujinet = 1;                 /* dials a port nobody listens on */
            opts.fujinet_addr = "127.0.0.1:1";
            QCOMPARE(intvsession_start(m_s, &opts), 0);
        }

        /* the window opens on the tab INTV_DEBUGGER_TAB names (the older "stic" is the Video tab) */
        qputenv("INTV_DEBUGGER_TAB", "stic");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("INTV_DEBUGGER_TAB");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Intellivision Debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
    }

    void cleanupTestCase()
    {
        if (m_win) m_win->close();
        QCoreApplication::processEvents();
        if (m_s) {
            intvsession_stop(m_s);
            intvsession_free(m_s);
        }
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << tabText(i);
        QCOMPARE(names, expectedTabs());
        QCOMPARE(m_tabs->currentIndex(), 9);   /* "stic" is the Video tab */
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        m_win->close();
        reap();
        struct { const char *name; int index; } cases[] = {
            {"cpu", 1}, {"basic", 2}, {"CART", 6}, {"3", 3}, {"Sound", 10}, {"console", 0}};
        for (const auto &c : cases) {
            qputenv("INTV_DEBUGGER_TAB", c.name);
            DebuggerWindow::showFor(nullptr, m_s);
            qunsetenv("INTV_DEBUGGER_TAB");
            QMainWindow *w = nullptr;
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowTitle() == QLatin1String("Intellivision Debugger")) w = qobject_cast<QMainWindow *>(top);
            QVERIFY(w);
            QCOMPARE(w->findChild<QTabWidget *>()->currentIndex(), c.index);
            w->close();
            reap();
        }
        DebuggerWindow::showFor(nullptr, m_s);
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle() == QLatin1String("Intellivision Debugger")) m_win = qobject_cast<QMainWindow *>(top);
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
    }

    void helpContexts()
    {
        static const char *const kContexts[] = {"console", "disassembly", "basic", "memory", "breakpoints",
                                                "watches", "cart", "trace", "history", "video", "sound",
                                                "symbols", "fujinet"};
        for (int i = 0; i < 13; ++i) {
            m_tabs->setCurrentIndex(i);
            QCOMPARE(m_win->property("intvHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[i])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(10);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 12);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        for (QPushButton *b : window->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) b->click();
        QCOMPARE(m_tabs->count(), 13);
        QCOMPARE(tabText(10), QStringLiteral("Sound"));
    }

    void consoleAndRegisters()
    {
        if (!m_machine) QSKIP("needs the system ROMs (WITH_INTV_ROMS=ON)");
        intvdebug_pause(m_d);
        waitPaused();
        loadProgram();

        /* the console runs the core's prompt */
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QVERIFY(in);
        in->setText(QStringLiteral("print $10 + 2"));
        QTest::keyClick(in, Qt::Key_Return);
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")));
        in->setText(QStringLiteral("regs"));
        QTest::keyClick(in, Qt::Key_Return);
        QVERIFY(consoleText().contains(QStringLiteral("R7")));

        /* step the NOPs, then the MVII, with the toolbar button */
        QCOMPARE(pc(), kStart);
        QPushButton *step = button(QStringLiteral("Step ("));
        QVERIFY(step);
        for (int i = 1; i <= 3; ++i) {   /* a step lands at the next boundary: wait for each */
            step->click();
            QTest::qWait(300);   /* the step lands asynchronously, on the emulator thread */
            QTRY_COMPARE(pc(), kStart + i);
        }
        QCOMPARE(pc(), kMvii);
        step->click();
        QTRY_COMPARE(pc(), kMvii + 2);
        intvdebug_regs r;
        QVERIFY(intvdebug_regs_get(m_d, &r));
        QCOMPARE(int(r.r[0]), 0x1234);

        /* the strip: R0 takes hex or an expression, only while stopped */
        const QList<QLineEdit *> regs = regFields();
        QVERIFY(regs.size() >= 8);
        regs[0]->setText(QStringLiteral("abcd"));
        QTest::keyClick(regs[0], Qt::Key_Return);
        QVERIFY(intvdebug_regs_get(m_d, &r));
        QCOMPARE(int(r.r[0]), 0xABCD);
        regs[1]->setText(QStringLiteral("r0 + 1"));
        QTest::keyClick(regs[1], Qt::Key_Return);
        QVERIFY(intvdebug_regs_get(m_d, &r));
        QCOMPARE(int(r.r[1]), 0xABCE);
    }

    void pokeWidth()
    {
        if (!m_machine) QSKIP("needs the system ROMs (WITH_INTV_ROMS=ON)");
        QVERIFY(intvdebug_is_paused(m_d));
        QLineEdit *addr = entry(QStringLiteral("address or symbol"));
        QLineEdit *val = entry(QStringLiteral("word"));
        QVERIFY(addr && val);
        /* system RAM is a 16-bit word */
        addr->setText(QStringLiteral("$0330"));
        val->setText(QStringLiteral("$1FFF"));
        QTest::keyClick(val, Qt::Key_Return);
        uint16_t w = 0;
        intvdebug_read(m_d, 0x0330, &w, 1);
        QCOMPARE(int(w), 0x1FFF);
        /* the scratchpad is a byte: a word is refused, a byte is written */
        addr->setText(QStringLiteral("$0150"));
        val->setText(QStringLiteral("$1FF"));
        QTest::keyClick(val, Qt::Key_Return);
        intvdebug_read(m_d, 0x0150, &w, 1);
        QVERIFY(w != 0x1FF);
        val->setText(QStringLiteral("$AB"));
        QTest::keyClick(val, Qt::Key_Return);
        intvdebug_read(m_d, 0x0150, &w, 1);
        QCOMPARE(int(w), 0xAB);
    }

    void breakpointWithCondition()
    {
        if (!m_machine) QSKIP("needs the system ROMs (WITH_INTV_ROMS=ON)");
        intvdebug_breakpoint_clear_all(m_d);
        m_tabs->setCurrentIndex(4);
        QLineEdit *addr = entry(QStringLiteral("address, lo-hi"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("$0320"));
        cond->setText(QStringLiteral("value == $1234"));
        QComboBox *type = m_tabs->widget(4)->findChild<QComboBox *>();
        QVERIFY(type);
        type->setCurrentIndex(2);   /* Write */
        addButton()->click();
        intvdebug_bp bps[4];
        QCOMPARE(intvdebug_bp_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(INTVDEBUG_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $1234"));

        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("$0321"));
        cond->setText(QStringLiteral("value =="));
        addButton()->click();
        QCOMPARE(intvdebug_bp_list(m_d, bps, 4), 1);

        /* run the program: it stops after the MVO, the 16-bit value in the text */
        loadProgram();
        intvdebug_resume(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(intvdebug_is_paused(m_d), 3000);
        intvdebug_snapshot snap;
        intvdebug_stop_snapshot(m_d, &snap);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("$1234")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Continue")) != nullptr, 2000);
        intvdebug_breakpoint_clear_all(m_d);
    }

    void videoAndSoundAndIo()
    {
        if (!m_machine) QSKIP("needs the system ROMs (WITH_INTV_ROMS=ON)");
        QVERIFY(intvdebug_is_paused(m_d));
        m_tabs->setCurrentIndex(9);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        m_tabs->setCurrentIndex(10);
        auto soundText = [this] {
            QString text;
            if (auto *v = qobject_cast<QPlainTextEdit *>(m_tabs->widget(10))) text = v->toPlainText();
            return text;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(soundText().contains(QStringLiteral("PSG 0")), qPrintable(soundText()), 3000);
        QString io;
        for (QPlainTextEdit *v : m_tabs->widget(6)->findChildren<QPlainTextEdit *>()) io += v->toPlainText();
        QVERIFY(io.contains(QStringLiteral("Memory map")));
        QVERIFY(io.contains(QStringLiteral("controllers")));
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
