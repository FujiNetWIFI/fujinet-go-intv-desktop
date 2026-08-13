/*
 * SettingsDialog: plain Qt6 Widgets (QGroupBox/QFormLayout), no .ui file --
 * transposed from fujinet-go-coco-desktop's frontends/kde/SettingsDialog.cpp,
 * the newest and most complete settings dialog in the FujiNet Go Desktop
 * family. Key/default list matches frontends/gnome/prefs.c exactly (both
 * read/write the same shared settings.ini):
 *   "ecs"            INTVSESSION_HW_* (default AUTO)  -- restart to apply
 *   "ivoice"         INTVSESSION_HW_* (default AUTO)  -- restart to apply
 *   "video_standard" INTVSESSION_VIDEO_* (default NTSC) -- restart to apply
 *   "keyboard_mode"  0 = hand controllers, 1 = ECS keyboard -- applies live
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace {

/* Walks an intvsession_*_name(idx) table (NULL past the end) into a
 * QComboBox, so a new option added to that table needs no change here. */
void fillNames(QComboBox *box, const char *(*nameFn)(int))
{
    for (int i = 0; nameFn(i); i++)
        box->addItem(QString::fromUtf8(nameFn(i)));
}

} // namespace

SettingsDialog::SettingsDialog(intvsession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Settings"));
    auto *outer = new QVBoxLayout(this);

    auto *machineBox = new QGroupBox(QStringLiteral("Machine"), this);
    auto *machineForm = new QFormLayout(machineBox);

    bool haveEcsRom = intvsession_has_ecs_rom(m_session);
    QComboBox *ecs = addHwModeRow(machineForm, QStringLiteral("ECS"), "ecs");
    ecs->setEnabled(haveEcsRom);
    ecs->setToolTip(
        haveEcsRom
            ? QStringLiteral("Entertainment Computer System -- second "
                            "controller pair, ECS keyboard, extra sound")
            : QStringLiteral(
                  "Unavailable: ecs.bin not found in the ROM directory"));
    addHwModeRow(machineForm, QStringLiteral("Intellivoice"), "ivoice");

    auto *video = new QComboBox(machineBox);
    fillNames(video, intvsession_video_name);
    video->setCurrentIndex(
        intvsession_get_int(m_session, "video_standard",
                            INTVSESSION_VIDEO_NTSC));
    connect(video, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (intvsession_get_int(m_session, "video_standard",
                                INTVSESSION_VIDEO_NTSC) == idx)
            return;
        intvsession_set_int(m_session, "video_standard", idx);
        m_sessionDirty = true;
    });
    machineForm->addRow(QStringLiteral("Video Standard"), video);

    auto *note = new QLabel(
        QStringLiteral("Applied by restarting the session when this dialog "
                       "closes. FujiNet stays connected."),
        machineBox);
    note->setWordWrap(true);
    machineForm->addRow(note);

    auto *inputBox = new QGroupBox(QStringLiteral("Input"), this);
    auto *inputForm = new QFormLayout(inputBox);
    auto *ecsKeyboard = new QCheckBox(inputBox);
    ecsKeyboard->setChecked(
        intvsession_get_int(m_session, "keyboard_mode", 0) != 0);
    connect(ecsKeyboard, &QCheckBox::toggled, this, [this](bool on) {
        if (intvsession_get_int(m_session, "keyboard_mode", 0) ==
            (on ? 1 : 0))
            return;
        intvsession_set_int(m_session, "keyboard_mode", on ? 1 : 0);
        if (!on)
            /* Leaving ECS keyboard mode -- release anything still held; see
             * intvsession_ecs_keys_clear's own comment. */
            intvsession_ecs_keys_clear(m_session);
    });
    inputForm->addRow(QStringLiteral("ECS Keyboard"), ecsKeyboard);
    auto *inputNote = new QLabel(
        QStringLiteral("When on, the keyboard types on the ECS instead of "
                       "driving the hand controllers (applies immediately)."),
        inputBox);
    inputNote->setWordWrap(true);
    inputForm->addRow(inputNote);

    outer->addWidget(machineBox);
    outer->addWidget(inputBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
}

QComboBox *SettingsDialog::addHwModeRow(QFormLayout *form,
                                        const QString &label,
                                        const char *key)
{
    auto *box = new QComboBox(this);
    fillNames(box, intvsession_hw_mode_name);
    box->setCurrentIndex(intvsession_get_int(m_session, key,
                                             INTVSESSION_HW_AUTO));
    connect(box, &QComboBox::currentIndexChanged, this, [this, key](int idx) {
        if (intvsession_get_int(m_session, key, INTVSESSION_HW_AUTO) == idx)
            return;
        intvsession_set_int(m_session, key, idx);
        m_sessionDirty = true;
    });
    form->addRow(label, box);
    return box;
}
