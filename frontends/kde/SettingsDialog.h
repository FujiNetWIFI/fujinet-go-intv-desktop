/*
 * SettingsDialog: machine options (ECS/Intellivoice/video standard) and
 * ECS keyboard input mode. See SettingsDialog.cpp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include "intvsession.h"

class QComboBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(intvsession *session, QWidget *parent = nullptr);

    /* True when one of the restart-class options (ECS/Intellivoice/video
     * standard) changed, so the caller knows to restart the session for it
     * to take effect. */
    bool sessionDirty() const { return m_sessionDirty; }

private:
    QComboBox *addHwModeRow(class QFormLayout *form, const QString &label,
                            const char *key);

    intvsession *m_session;
    bool m_sessionDirty = false;
};
