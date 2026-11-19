/*
 * SettingsDialog: machine options (ECS/Intellivoice/video standard) and
 * ECS keyboard input mode. See SettingsDialog.cpp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include <vector>

#include "intvsession.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTimer;
class QVBoxLayout;
class QWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(intvsession *session, QWidget *parent = nullptr);

    /* True when one of the restart-class options (ECS/Intellivoice/video
     * standard) changed, so the caller knows to restart the session for it
     * to take effect. */
    bool sessionDirty() const { return m_sessionDirty; }

    /* True when the dialog was closed with "Open Controllers". */
    bool openControllersRequested() const { return m_openControllers; }

signals:
    /* A ROM import ran (successful or not): the caller may now be able to
     * start the session. */
    void romsImported();

private:
    void refreshRoms();
    void importRoms(bool folder);
    void rebuildPads();
    void refreshPadSides();
    QComboBox *addHwModeRow(class QFormLayout *form, const QString &label,
                            const char *key);

    intvsession *m_session;
    QComboBox *m_ecsCombo = nullptr;
    QCheckBox *m_ecsKeyboard = nullptr;
    QCheckBox *m_external = nullptr;
    QLineEdit *m_externalAddr = nullptr;
    std::vector<QLabel *> m_romRows;
    bool m_sessionDirty = false;
    bool m_openControllers = false;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;
    unsigned m_padGen = ~0u;
};
