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
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
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

    m_ecsCombo = addHwModeRow(machineForm, QStringLiteral("ECS"), "ecs");
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
    auto *ecsKeyboard = m_ecsKeyboard = new QCheckBox(inputBox);
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
                       "driving the hand controllers (applies immediately).\n"
                       "Warning: the FujiNet CONFIG ROM has no ECS keyboard "
                       "support yet, so while this is on its menus can only "
                       "be driven by a gamepad or the keypad window (F9)."),
        inputBox);
    inputNote->setWordWrap(true);
    inputForm->addRow(inputNote);

    auto *fujinetBox = new QGroupBox(QStringLiteral("FujiNet"), this);
    auto *fujinetForm = new QFormLayout(fujinetBox);
    m_external = new QCheckBox(fujinetBox);
    m_external->setChecked(
        intvsession_get_int(m_session, "fujinet_external", 0) != 0);
    connect(m_external, &QCheckBox::toggled, this, [this](bool wanted) {
        if (intvsession_get_int(m_session, "fujinet_external", 0) ==
            (wanted ? 1 : 0))
            return;
        intvsession_set_int(m_session, "fujinet_external", wanted ? 1 : 0);
        m_sessionDirty = true;
    });
    fujinetForm->addRow(QStringLiteral("Use external FujiNet"), m_external);
    m_externalAddr = new QLineEdit(
        QString::fromUtf8(intvsession_get_str(m_session,
                                              "fujinet_external_addr", "")),
        fujinetBox);
    m_externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:65503"));
    m_externalAddr->setEnabled(m_external->isChecked());
    connect(m_external, &QCheckBox::toggled, m_externalAddr,
            &QWidget::setEnabled);
    /* Valid text (or none, meaning 127.0.0.1:65503) is saved; anything else
     * is shown in red and not saved. */
    connect(m_externalAddr, &QLineEdit::textChanged, this,
            [this](const QString &text) {
        char host[64];
        int port;
        const QByteArray addr = text.trimmed().toUtf8();
        if (intvsession_parse_hostport(addr.constData(), host, sizeof host,
                                       &port) != 0) {
            m_externalAddr->setStyleSheet(
                QStringLiteral("QLineEdit { color: #d83b3b; }"));
            return;
        }
        m_externalAddr->setStyleSheet(QString());
        if (QString::fromUtf8(intvsession_get_str(
                m_session, "fujinet_external_addr", "")) == text.trimmed())
            return;
        intvsession_set_str(m_session, "fujinet_external_addr",
                            addr.constData());
        m_sessionDirty = true;
    });
    fujinetForm->addRow(QStringLiteral("Address:port"), m_externalAddr);
    auto *fujinetNote = new QLabel(
        QStringLiteral("Skips the built-in FujiNet and dials another "
                       "instance. Start that instance listening on this "
                       "address (its [BOIP] port). Applied by restarting the "
                       "session when this dialog closes."),
        fujinetBox);
    fujinetNote->setWordWrap(true);
    fujinetForm->addRow(fujinetNote);

    outer->addWidget(machineBox);
    outer->addWidget(inputBox);
    outer->addWidget(fujinetBox);

    /* ROMs: the system ROMs, grouped, each imported or not. */
    auto *roms = new QGroupBox(QStringLiteral("ROMs"), this);
    auto *rv = new QVBoxLayout(roms);
    auto *rnote = new QLabel(
        QStringLiteral("The Intellivision's system ROMs are not part of the "
                       "app. Import them from files, a zip or a folder "
                       "(MAME's intv set works); they are recognised by "
                       "their checksum. Kept in %1.")
            .arg(QString::fromUtf8(intvsession_roms_path(m_session))),
        roms);
    rnote->setWordWrap(true);
    rnote->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    auto *rbuttons = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import…"), roms);
    auto *dir = new QPushButton(QStringLiteral("Import a Folder…"), roms);
    connect(files, &QPushButton::clicked, this, [this] { importRoms(false); });
    connect(dir, &QPushButton::clicked, this, [this] { importRoms(true); });
    rbuttons->addWidget(files);
    rbuttons->addWidget(dir);
    rbuttons->addStretch(1);
    rv->addWidget(rnote);
    rv->addLayout(rbuttons);
    QGroupBox *group = nullptr;
    QFormLayout *rform = nullptr;
    QString last;
    for (int r = 0; r < intvsession_rom_item_count(); r++) {
        const intv_rom_item *item = intvsession_rom_item(r);
        const QString g = QString::fromUtf8(item->group);
        if (!group || g != last) {
            group = new QGroupBox(g, roms);
            rform = new QFormLayout(group);
            rv->addWidget(group);
            last = g;
        }
        auto *state = new QLabel(roms);
        state->setStyleSheet(
            QStringLiteral("color: palette(placeholder-text);"));
        rform->addRow(new QLabel(QString::fromUtf8(item->name)), state);
        m_romRows.push_back(state);
    }
    outer->addWidget(roms);
    refreshRoms();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        char host[64];
        int port;
        if (m_external->isChecked() &&
            intvsession_parse_hostport(
                m_externalAddr->text().trimmed().toUtf8().constData(), host,
                sizeof host, &port) != 0) {
            QMessageBox::warning(
                this, QStringLiteral("External FujiNet"),
                QStringLiteral("\"%1\" is not an address:port. Example: "
                               "127.0.0.1:65503")
                    .arg(m_externalAddr->text()));
            m_externalAddr->setFocus();
            return;
        }
        accept();
    });
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

void SettingsDialog::refreshRoms()
{
    for (size_t r = 0; r < m_romRows.size(); r++)
        m_romRows[r]->setText(
            intvsession_rom_item_available(m_session, (int)r)
                ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
    const bool haveEcsRom = intvsession_has_ecs_rom(m_session) != 0;
    const QString tip = haveEcsRom
        ? QStringLiteral("Entertainment Computer System -- second "
                         "controller pair, ECS keyboard, extra sound")
        : QStringLiteral("Unavailable: ecs.bin not found in the ROM "
                         "directory");
    for (QWidget *w : {static_cast<QWidget *>(m_ecsCombo),
                       static_cast<QWidget *>(m_ecsKeyboard)}) {
        if (!w)
            continue;
        w->setEnabled(haveEcsRom);
        w->setToolTip(tip);
    }
}

void SettingsDialog::importRoms(bool folder)
{
    const QString path = folder
        ? QFileDialog::getExistingDirectory(
              this, QStringLiteral("Import BIOS from a Folder"))
        : QFileDialog::getOpenFileName(
              this, QStringLiteral("Import BIOS"), QString(),
              QStringLiteral("BIOS images and ROM sets (*.zip *.ZIP *.rom "
                             "*.ROM *.bin *.BIN);;All files (*)"));
    if (path.isEmpty())
        return;
    char msg[1024] = "";
    const int rc = intvsession_rom_import(m_session, path.toUtf8().constData(),
                                          msg, sizeof msg);
    QMessageBox::information(this,
                             rc == 0 ? QStringLiteral("BIOS Imported")
                                     : QStringLiteral("Nothing Imported"),
                             QString::fromUtf8(msg));
    refreshRoms();
    emit romsImported();
}
