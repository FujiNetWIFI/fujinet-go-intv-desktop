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

#include "help/HelpWindow.h"

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
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
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
    /* The groups scroll when the screen is too short for all of them. */
    auto *frame = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget;
    auto *outer = new QVBoxLayout(body);
    outer->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(body);
    frame->addWidget(scroll, 1);

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
                       "be driven by a gamepad or the Controllers window (F9)."),
        inputBox);
    inputNote->setWordWrap(true);
    inputForm->addRow(inputNote);

    /* Gamepads (a change of the polling switch is applied by the restart) */
    inputBox->setProperty("intvHelpContext", QStringLiteral("prefs/Input"));
    auto *gamepads = new QCheckBox(inputBox);
    gamepads->setChecked(intvsession_get_int(m_session, "enable_gamepad", 1) != 0);
    gamepads->setToolTip(QStringLiteral("Poll USB/Bluetooth gamepads (applied by restarting the session)"));
    connect(gamepads, &QCheckBox::toggled, this, [this](bool on) {
        if (intvsession_get_int(m_session, "enable_gamepad", 1) == (on ? 1 : 0))
            return;
        intvsession_set_int(m_session, "enable_gamepad", on ? 1 : 0);
        m_sessionDirty = true;
    });
    inputForm->addRow(QStringLiteral("Enable gamepads"), gamepads);

    /* One row per pad, refreshed as they come and go */
    auto *padBox = new QGroupBox(QStringLiteral("Gamepads (assigned in connection order unless chosen here)"), inputBox);
    m_padBox = new QVBoxLayout(padBox);
    m_padBox->setSpacing(2);
    inputForm->addRow(padBox);
    auto *padTimer = new QTimer(this);
    connect(padTimer, &QTimer::timeout, this, [this] {
        if (intvsession_gamepad_generation(m_session) != m_padGen)
            rebuildPads();
        else
            refreshPadSides();
    });
    padTimer->start(1000);
    rebuildPads();

    auto *controlsRow = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"), inputBox);
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openWindow = new QPushButton(QStringLiteral("Open Controllers"), inputBox);
    openWindow->setToolTip(QStringLiteral("Close Settings and open the Controllers window (F9), where keys and gamepad buttons are re-mapped"));
    auto *resetState = new QLabel(inputBox);
    resetState->setStyleSheet(QStringLiteral("color: gray;"));
    connect(resetControls, &QPushButton::clicked, this, [this, resetState] {
        intvsession_bindings_reset(m_session);
        resetState->setText(QStringLiteral("Every key and gamepad button is back to its default."));
    });
    connect(openWindow, &QPushButton::clicked, this, [this] {
        m_openControllers = true;
        accept();
    });
    controlsRow->addWidget(resetControls);
    controlsRow->addWidget(openWindow);
    controlsRow->addWidget(resetState, 1);
    controlsRow->addWidget(intvHelpButton(QStringLiteral("prefs/Input")));
    inputForm->addRow(controlsRow);

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

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    setProperty("intvHelpContext", QStringLiteral("prefs/Machine"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Help, this);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { intvHelpFor(this); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), this, [this] { intvHelpFor(this); });
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
    frame->addWidget(buttons);

    /* Room for every group when the screen has it, else as tall as it allows. */
    const QSize want = body->sizeHint();
    const int avail = screen() ? screen()->availableGeometry().height() - 170 : want.height();
    scroll->setMinimumSize(want.width() + scroll->verticalScrollBar()->sizeHint().width() + 4,
                           qMin(want.height(), qMax(avail, 300)));
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

/* "plays Left" next to each pad. */
void SettingsDialog::refreshPadSides()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = intvsession_gamepad_effective_side(m_session, idx);
        const QString text = eff >= 0
            ? QStringLiteral("plays %1").arg(QString::fromUtf8(intvsession_pad_side_name((intvsession_pad_side)eff)))
            : QStringLiteral("plays nothing");
        if (m_padEff[(size_t)idx]->text() != text)
            m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the controller it plays and the one it is set to
 * (Automatic, Left, Right, and the ECS pair when the ECS is on). */
void SettingsDialog::rebuildPads()
{
    for (QWidget *row : m_padRows)
        delete row;
    m_padRows.clear();
    m_padEff.clear();
    m_padGen = intvsession_gamepad_generation(m_session);
    const int sides = intvsession_ecs_on(m_session) ? 4 : 2;
    const int count = intvsession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count; ++idx) {
        char name[128];
        intvsession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(QString::fromUtf8(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0.. = that controller */
        auto *box = new QComboBox;
        box->addItem(QStringLiteral("Automatic"));
        for (int side = 0; side < sides; ++side)
            box->addItem(QString::fromUtf8(intvsession_pad_side_name((intvsession_pad_side)side)));
        const int assign = intvsession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= sides ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            intvsession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadSides();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    refreshPadSides();
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
