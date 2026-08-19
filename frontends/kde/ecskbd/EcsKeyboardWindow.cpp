/*
 * EcsKeyboardWindow -- an on-screen ECS keyboard: 48 buttons covering
 * every key of the ECS's own 7x8 scan-matrix keyboard (see
 * core/jzintv/intv_host.h's intv_ecs_key), each driving
 * intvsession_ecs_key_set directly. Independent of the "keyboard_mode"
 * setting (DisplayWidget::forwardKey) -- these on-screen buttons work
 * whether or not the physical keyboard is currently routed to the ECS, so
 * opening this window never silently changes that setting.
 *
 * Ordinary keys use QPushButton's own pressed()/released() signals
 * directly, same as KeypadWindow.cpp: a real ECS key is held for as long
 * as it's down. Shift and Ctrl are checkable (QPushButton::setCheckable)
 * instead: a mouse can't hold two buttons down to chord a Shifted letter
 * the way a hand can, so they latch, and intv_host_ecs_key's OR-in-a-bit
 * chording (core/jzintv/intv_host.c) does the rest.
 *
 * FOCUS: forwards keyboard events through KeyForward.h's shared translation,
 * the same one DisplayWidget and KeypadWindow use -- see that header for why
 * the three windows no longer each carry their own copy. This window's own
 * forwardKey still routes to the ECS matrix unconditionally rather than
 * calling intvForwardKey, for the "keyboard_mode"-independence reason above.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EcsKeyboardWindow.h"

#include "KeyForward.h"

#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

/* An ordinary ECS key: held for as long as the mouse button is down. */
QPushButton *makeKey(const QString &label, intvsession *session,
                    intvsession_ecs_key key, int width = 40)
{
    auto *btn = new QPushButton(label);
    btn->setFixedSize(width, 40);
    QObject::connect(btn, &QPushButton::pressed, btn, [session, key] {
        intvsession_ecs_key_set(session, key, 1);
    });
    QObject::connect(btn, &QPushButton::released, btn, [session, key] {
        intvsession_ecs_key_set(session, key, 0);
    });
    return btn;
}

/* Shift/Ctrl: checkable/latching -- see file header. */
QPushButton *makeModKey(const QString &label, intvsession *session,
                       intvsession_ecs_key key)
{
    auto *btn = new QPushButton(label);
    btn->setCheckable(true);
    btn->setFixedSize(64, 40);
    QObject::connect(btn, &QPushButton::toggled, btn,
                     [session, key](bool on) {
                         intvsession_ecs_key_set(session, key, on);
                     });
    return btn;
}

struct EcsKeyLabel { const char *label; intvsession_ecs_key key; };

/* Four QWERTY-ish rows plus a function row, covering all 48 keys of
 * intv_ecs_key exactly once. See core/jzintv/intv_host.h for the row/mask
 * this ultimately maps to. */
constexpr EcsKeyLabel kRowDigits[10] = {
    {"1", INTVSESSION_ECS_KEY_1}, {"2", INTVSESSION_ECS_KEY_2},
    {"3", INTVSESSION_ECS_KEY_3}, {"4", INTVSESSION_ECS_KEY_4},
    {"5", INTVSESSION_ECS_KEY_5}, {"6", INTVSESSION_ECS_KEY_6},
    {"7", INTVSESSION_ECS_KEY_7}, {"8", INTVSESSION_ECS_KEY_8},
    {"9", INTVSESSION_ECS_KEY_9}, {"0", INTVSESSION_ECS_KEY_0},
};
constexpr EcsKeyLabel kRowQwerty[10] = {
    {"Q", INTVSESSION_ECS_KEY_Q}, {"W", INTVSESSION_ECS_KEY_W},
    {"E", INTVSESSION_ECS_KEY_E}, {"R", INTVSESSION_ECS_KEY_R},
    {"T", INTVSESSION_ECS_KEY_T}, {"Y", INTVSESSION_ECS_KEY_Y},
    {"U", INTVSESSION_ECS_KEY_U}, {"I", INTVSESSION_ECS_KEY_I},
    {"O", INTVSESSION_ECS_KEY_O}, {"P", INTVSESSION_ECS_KEY_P},
};
constexpr EcsKeyLabel kRowAsdf[10] = {
    {"A", INTVSESSION_ECS_KEY_A}, {"S", INTVSESSION_ECS_KEY_S},
    {"D", INTVSESSION_ECS_KEY_D}, {"F", INTVSESSION_ECS_KEY_F},
    {"G", INTVSESSION_ECS_KEY_G}, {"H", INTVSESSION_ECS_KEY_H},
    {"J", INTVSESSION_ECS_KEY_J}, {"K", INTVSESSION_ECS_KEY_K},
    {"L", INTVSESSION_ECS_KEY_L}, {";", INTVSESSION_ECS_KEY_SEMI},
};
constexpr EcsKeyLabel kRowZxcv[10] = {
    {"Z", INTVSESSION_ECS_KEY_Z}, {"X", INTVSESSION_ECS_KEY_X},
    {"C", INTVSESSION_ECS_KEY_C}, {"V", INTVSESSION_ECS_KEY_V},
    {"B", INTVSESSION_ECS_KEY_B}, {"N", INTVSESSION_ECS_KEY_N},
    {"M", INTVSESSION_ECS_KEY_M}, {",", INTVSESSION_ECS_KEY_COMMA},
    {".", INTVSESSION_ECS_KEY_PERIOD}, {"←", INTVSESSION_ECS_KEY_LEFT},
};

QHBoxLayout *addRow(const EcsKeyLabel *row, int count, intvsession *session)
{
    auto *hbox = new QHBoxLayout;
    hbox->addStretch();
    for (int i = 0; i < count; i++)
        hbox->addWidget(
            makeKey(QString::fromUtf8(row[i].label), session, row[i].key));
    hbox->addStretch();
    return hbox;
}

QPointer<EcsKeyboardWindow> g_singleton;
QPointer<QWidget> g_topLevel;

} // namespace

void EcsKeyboardWindow::showFor(QWidget *parent, intvsession *session)
{
    if (g_singleton) {
        if (g_topLevel->isVisible()) {
            g_topLevel->hide();
            g_singleton->releaseAll();
        } else {
            g_singleton->m_notice->setVisible(
                !intvsession_has_ecs_rom(session) ||
                intvsession_get_int(session, "ecs", INTVSESSION_HW_AUTO) ==
                    INTVSESSION_HW_OFF);
            g_topLevel->show();
            g_topLevel->raise();
            g_topLevel->activateWindow();
        }
        return;
    }
    g_singleton = new EcsKeyboardWindow(parent, session);
    g_topLevel = g_singleton;
    g_topLevel->show();
}

EcsKeyboardWindow::EcsKeyboardWindow(QWidget *parent, intvsession *session)
    : QWidget(parent, Qt::Window), m_session(session)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(QStringLiteral("ECS Keyboard"));

    auto *root = new QVBoxLayout(this);

    m_notice = new QLabel(
        QStringLiteral("ECS is not enabled -- see Settings to turn it on."));
    m_notice->setVisible(
        !intvsession_has_ecs_rom(session) ||
        intvsession_get_int(session, "ecs", INTVSESSION_HW_AUTO) ==
            INTVSESSION_HW_OFF);
    root->addWidget(m_notice);

    root->addWidget(buildKeyboard());
}

QWidget *EcsKeyboardWindow::buildKeyboard()
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);

    layout->addLayout(addRow(kRowDigits, 10, m_session));
    layout->addLayout(addRow(kRowQwerty, 10, m_session));
    layout->addLayout(addRow(kRowAsdf, 10, m_session));
    layout->addLayout(addRow(kRowZxcv, 10, m_session));

    auto *fnRow = new QHBoxLayout;
    fnRow->addStretch();
    fnRow->addWidget(makeKey(QStringLiteral("Esc"), m_session,
                            INTVSESSION_ECS_KEY_ESC, 56));
    fnRow->addWidget(
        makeModKey(QStringLiteral("Ctrl"), m_session, INTVSESSION_ECS_KEY_CTRL));
    fnRow->addWidget(makeModKey(QStringLiteral("Shift"), m_session,
                               INTVSESSION_ECS_KEY_SHIFT));
    fnRow->addWidget(makeKey(QStringLiteral("Space"), m_session,
                            INTVSESSION_ECS_KEY_SPACE, 160));
    fnRow->addWidget(
        makeKey(QStringLiteral("↑"), m_session, INTVSESSION_ECS_KEY_UP));
    fnRow->addWidget(makeKey(QStringLiteral("↓"), m_session,
                            INTVSESSION_ECS_KEY_DOWN));
    fnRow->addWidget(makeKey(QStringLiteral("→"), m_session,
                            INTVSESSION_ECS_KEY_RIGHT));
    fnRow->addWidget(makeKey(QStringLiteral("Enter"), m_session,
                            INTVSESSION_ECS_KEY_ENTER, 56));
    fnRow->addStretch();
    layout->addLayout(fnRow);

    return box;
}

void EcsKeyboardWindow::forwardKey(const QKeyEvent *event, int down)
{
    /* Deliberately NOT intvForwardKey: this window is the ECS keyboard, so
     * it routes to the ECS matrix whether or not "keyboard_mode" is on --
     * same reasoning as its on-screen buttons (see this file's header).
     * Only the translation is shared. */
    const quint32 keysym = intvKeysymForKeyEvent(event);
    if (keysym == 0)
        return;

    intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
    if (key != INTVSESSION_ECS_KEY_NONE)
        intvsession_ecs_key_set(m_session, key, down);
}

void EcsKeyboardWindow::releaseAll()
{
    intvsession_ecs_keys_clear(m_session);
}

void EcsKeyboardWindow::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forwardKey(event, 1);
    event->accept();
}

void EcsKeyboardWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forwardKey(event, 0);
    event->accept();
}

void EcsKeyboardWindow::focusOutEvent(QFocusEvent *event)
{
    releaseAll();
    QWidget::focusOutEvent(event);
}

void EcsKeyboardWindow::closeEvent(QCloseEvent *event)
{
    /* Singleton: hide and reuse, matching KeypadWindow's own window. */
    releaseAll();
    hide();
    event->ignore();
}
