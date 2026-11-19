/*
 * EcsKeyboardWindow (F10): the ECS Computer Keyboard drawn as the real thing
 * (beige case, label plate, black keys with their shifted legends, the white
 * RTN, a long space bar) from intvsession_ecs_layout, clickable: a key is held
 * while the mouse is down, SHIFT and CTL latch. Held keys -- the mouse's, the
 * host keyboard's and a latched modifier -- are lit. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include <functional>

#include "intvsession.h"

class EcsKeyboardStage;
class EcsKeyboardHint;
class QTimer;

class EcsKeyboardWindow : public QWidget {
public:
    explicit EcsKeyboardWindow(intvsession *session, QWidget *parent = nullptr);

    /* Shows the window and raises it. */
    void present();

    /* Called for F9, which the main window's menu owns: set by MainWindow. */
    std::function<void()> onToggleControllers;
    /* F12 and Ctrl+R, which the main window's menu owns: set by MainWindow. */
    std::function<void()> onShowDebugger;
    std::function<void()> onResetToConfig;

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void closeEvent(QCloseEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void refreshNotice();
    void releaseAll();

    intvsession *m_session;
    EcsKeyboardStage *m_stage = nullptr;
    EcsKeyboardHint *m_notice = nullptr;
    QTimer *m_heldTimer = nullptr;
};
