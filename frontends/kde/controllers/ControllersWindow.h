/*
 * The Controllers window (F9): the Intellivision II hand controllers drawn as
 * the real thing (a light-grey handset with a black red-outlined keypad, the
 * 16-way disc in its cream ring and four side buttons; two more handsets for
 * the ECS pair when the ECS is on) over the console's front strip with its
 * RESET key, from intvsession_controls_layout; the gamepads with their
 * assignments, and Map mode for rebinding. Hidden at start; a fixed-size
 * floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <vector>

#include "intvsession.h"

class ControllersStage;
class ControllersHint;

class ControllersWindow : public QWidget {
public:
    explicit ControllersWindow(intvsession *session, QWidget *parent = nullptr);

    /* Shows the window and raises it. */
    void present();

    /* Called for F10, which the main window's menu owns: set by MainWindow. */
    std::function<void()> onToggleEcsKeyboard;
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
    /* Tab is a key of the machine's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void setMapState(int state, const intvsession_key_mapping &target = intvsession_key_mapping());
    void pick(const intvsession_key_mapping &target);
    void bound(const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void rebuildPads();
    void refreshPadSides();

    intvsession *m_session;
    ControllersStage *m_stage = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays Left" */
    unsigned m_padGen = ~0u;
    int m_padEcs = -1;                    /* the ECS state the rows were built for */
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_sysButton[INTVSESSION_SYSACT_COUNT] = {};
    ControllersHint *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, 0 waiting for a key or
     * gamepad button for m_target. */
    int m_mapState = -2;
    intvsession_key_mapping m_target = {};
};
