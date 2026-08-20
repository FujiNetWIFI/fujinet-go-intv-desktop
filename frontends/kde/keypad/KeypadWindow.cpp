/*
 * KeypadWindow -- both hand controllers side by side: a 3x4 keypad grid
 * (1-9, Clear/0/Enter), three action buttons (top, lower-left,
 * lower-right), and a 16-position direction disc, each driving
 * intvsession_pad_key/intvsession_pad_disc directly.
 *
 * Digit/action buttons use QPushButton's own pressed()/released() signals
 * directly (Qt, unlike GTK, gives every QAbstractButton these natively --
 * no raw gesture/event plumbing needed the way the GNOME port's
 * keypad_window.c requires): a keypad digit is a real button on real
 * hardware, held for as long as the finger is down, which is exactly what
 * those two signals give.
 *
 * The disc is a small custom QWidget (DiscWidget, local to this file)
 * offering all 16 positions the hardware has, marking all 16 sectors, and
 * lighting exactly the one under the pointer. Hit testing is
 * intvsession_disc_from_point (core/src/disc_geom.c), shared with the
 * GNOME, macOS and Windows keypad windows -- one definition of which
 * sector a click belongs to, rather than the four subtly different private
 * copies these files used to carry; the painting below follows the same
 * step-by-step recipe they do, so the four discs look and behave alike.
 *
 * FOCUS: like the GNOME keypad window, this forwards keyboard events to
 * the session itself (see forwardKey) rather than trying to refuse focus
 * outright -- no toolkit reliably prevents a clicked toplevel from taking
 * keyboard focus, so typing still drives the machine correctly whichever
 * window currently has it. That forwarding goes through KeyForward.h's
 * shared translation now: this file used to carry its own cut-down copy
 * that handled the arrow keys and event->text() and nothing else, so with
 * this window focused the numpad, the modifiers/action buttons and
 * "keyboard_mode" were all quietly ignored.
 *
 * MAP MODE: a bottom row (buildMapRow, onMapClicked and friends, near the
 * end of this file) lets a click on any of the 15 digit/action buttons
 * above be rebound to a different keyboard key or gamepad button, through
 * core/src/bindings.c's remappable layer -- see intvsession.h's own
 * "remappable bindings" section for the contract. While armed, the same
 * pressed()/released() pair that would normally inject a pad key instead
 * picks the map target, and forwardKey's own keyboard capture intercepts
 * the next keystroke instead of forwarding it to the machine. A gamepad
 * button press is polled for on a short QTimer (g_gamepadPollTimer below)
 * since core/src/gamepad_sdl.c's capture result lands on its own
 * background thread, not this one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"

#include "KeyForward.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kDiscSize = 150;

/* Segments the highlighted sector's arc edge is drawn with -- see
 * paintEvent. */
constexpr int kWedgeSteps = 6;

/* Angles here are ordinary compass/math degrees (0 = East, growing
 * counter-clockwise), converted to Qt's y-DOWN widget coordinates at the
 * point of use: cos for x, MINUS sin for y. */
QPointF discPoint(double cx, double cy, double radius, double deg)
{
    const double a = deg * M_PI / 180.0;
    return QPointF(cx + radius * std::cos(a), cy - radius * std::sin(a));
}

class DiscWidget : public QWidget {
public:
    DiscWidget(intvsession *session, intvsession_pad_side side,
              QWidget *parent = nullptr)
        : QWidget(parent), m_session(session), m_side(side)
    {
        setFixedSize(kDiscSize, kDiscSize);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double cx = width() / 2.0, cy = height() / 2.0;
        const double r = std::min(width(), height()) / 2.0 - 4;
        const double hub = r * INTVSESSION_DISC_DEADZONE_FRAC;

        p.setBrush(QColor(38, 38, 43));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), r, r);

        /* The held sector: exactly the 22.5 degrees centred on the
         * direction intvsession_disc_from_point returned, so the lit wedge
         * is always the one bounded by the two spokes the pointer is
         * between -- never wider, never mirrored.
         *
         * Drawn as a filled polygon sampled along the arc rather than with
         * QPainterPath::arcTo: every toolkit in this project has its own
         * answer to which way an arc sweeps in a y-down surface, and each
         * of those answers has been wrong in one of these four files at
         * some point. A polygon has no such convention. At r ~= 71px a
         * 3.75-degree chord bulges 0.04px inside the true arc. */
        if (m_direction >= 0) {
            const double centreDeg =
                m_direction * INTVSESSION_DISC_SECTOR_DEG;
            QPolygonF wedge;
            wedge << QPointF(cx, cy);
            for (int i = 0; i <= kWedgeSteps; i++) {
                const double a = centreDeg -
                                 INTVSESSION_DISC_SECTOR_DEG / 2.0 +
                                 INTVSESSION_DISC_SECTOR_DEG * i / kWedgeSteps;
                wedge << discPoint(cx, cy, r, a);
            }
            p.setBrush(QColor(77, 140, 230));
            p.drawPolygon(wedge);
        }

        /* One spoke per sector boundary -- 16 of them, at the half-way
         * angles BETWEEN the 16 positions, so each position gets a visible
         * sector of its own to aim at. */
        p.setPen(QPen(QColor(115, 115, 128), 1.5));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < INTVSESSION_DISC_POSITIONS; i++) {
            const double a = i * INTVSESSION_DISC_SECTOR_DEG -
                             INTVSESSION_DISC_SECTOR_DEG / 2.0;
            p.drawLine(discPoint(cx, cy, hub, a), discPoint(cx, cy, r, a));
        }
        p.drawEllipse(QPointF(cx, cy), r, r);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(64, 64, 71));
        p.drawEllipse(QPointF(cx, cy), hub, hub);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_held = true;
        updateDirection(event->position());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_held)
            updateDirection(event->position());
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_held = false;
        setDirection(-1);
    }

private:
    void updateDirection(QPointF pos)
    {
        const double r = kDiscSize / 2.0;
        setDirection(
            intvsession_disc_from_point(pos.x() - r, pos.y() - r, r));
    }

    void setDirection(int dir)
    {
        if (dir == m_direction)
            return;
        m_direction = dir;
        intvsession_pad_disc(m_session, m_side, dir);
        update();
    }

    intvsession *m_session;
    intvsession_pad_side m_side;
    int m_direction = -1;
    bool m_held = false;
};

/* ---- Map mode (see this file's own header) --------------------------------
 * File-static, same as g_singleton/g_topLevel below: this window is a
 * singleton, so there is never more than one map sequence in flight, and
 * plain statics let makeKey's per-button lambdas (which capture by value,
 * not `this`) reach the shared state without threading a KeypadWindow*
 * through every helper. */
enum class MapState { Idle, PickTarget, WaitInput };

MapState g_mapState = MapState::Idle;
intvsession_pad_side g_mapSide = INTVSESSION_PAD_LEFT;
intvsession_key g_mapKey = INTVSESSION_KEY_0;
intvsession *g_mapSession = nullptr;
QPushButton *g_mapBtn = nullptr;
QLabel *g_statusLabel = nullptr;
/* [side][key] -- only INTVSESSION_PAD_LEFT/_RIGHT are ever populated, the
 * two panels this window builds. */
QPushButton *g_keyButtons[2][INTVSESSION_KEY_COUNT] = {};
QTimer *g_gamepadPollTimer = nullptr;

void setHighlighted(QPushButton *btn, bool on)
{
    if (!btn)
        return;
    /* Qt has no CSS-class idiom as convenient as GTK's add_css_class for a
     * one-off highlight, so this reaches for the palette directly --
     * palette(highlight)/palette(highlighted-text) track whatever the
     * user's light/dark theme already uses for a selected item, same as
     * this button would look selected in a list view. */
    btn->setStyleSheet(on ? QStringLiteral(
                               "background-color: palette(highlight); "
                               "color: palette(highlighted-text);")
                          : QString());
}

void mapSetStatus(const QString &text)
{
    if (g_statusLabel)
        g_statusLabel->setText(text);
}

/* Back to MapState::Idle from any state, undoing whatever highlight is
 * showing and disarming gamepad capture -- shared by Map-to-abort, Reset,
 * and the window being hidden/closed mid-sequence. */
void mapResetUi()
{
    if (g_mapState == MapState::WaitInput)
        setHighlighted(g_keyButtons[g_mapSide][g_mapKey], false);
    setHighlighted(g_mapBtn, false);
    g_mapState = MapState::Idle;
    mapSetStatus(QString());
    if (g_mapSession)
        intvsession_gamepad_capture_cancel(g_mapSession);
    if (g_gamepadPollTimer) {
        g_gamepadPollTimer->stop();
    }
}

void mapSelectTarget(intvsession_pad_side side, intvsession_key key)
{
    char desc[128];
    intvsession_binding b = intvsession_binding_get(g_mapSession, side, key);

    g_mapSide = side;
    g_mapKey = key;
    g_mapState = MapState::WaitInput;
    setHighlighted(g_keyButtons[side][key], true);
    intvsession_binding_describe(b, desc, sizeof(desc));
    mapSetStatus(QStringLiteral("Currently mapped to %1. Press target key "
                                "or gamepad button, or press MAP to abort.")
                    .arg(QString::fromUtf8(desc)));
    intvsession_gamepad_capture_begin(g_mapSession);
    if (g_gamepadPollTimer)
        g_gamepadPollTimer->start(66); /* ~15Hz, see the timer's own setup */
}

/* Completes the mapping, keyboard or gamepad half alike -- both land here
 * once bindings.c has already made the change, just with a different verb
 * for the status line. */
void mapFinish(const QString &boundTo, const char *stolen)
{
    QString status = QStringLiteral("%1 %2 mapped to %3")
                         .arg(intvsession_pad_side_name(g_mapSide),
                             intvsession_pad_key_name(g_mapKey), boundTo);
    if (stolen && stolen[0])
        status += QStringLiteral(" (taken from %1)")
                     .arg(QString::fromUtf8(stolen));

    setHighlighted(g_keyButtons[g_mapSide][g_mapKey], false);
    setHighlighted(g_mapBtn, false);
    g_mapState = MapState::Idle;
    if (g_gamepadPollTimer)
        g_gamepadPollTimer->stop();
    mapSetStatus(status);
}

void mapCompleteKey(quint32 keysym)
{
    char stolen[128];
    char namebuf[64];

    intvsession_binding_set_key(g_mapSession, g_mapSide, g_mapKey, keysym,
                                stolen, sizeof(stolen));
    intvsession_gamepad_capture_cancel(g_mapSession);
    intvsession_keysym_name(keysym, namebuf, sizeof(namebuf));
    mapFinish(QString::fromUtf8(namebuf), stolen);
}

void mapCompleteButton(intvsession_pad_button button)
{
    char stolen[128];

    intvsession_binding_set_button(g_mapSession, g_mapSide, g_mapKey, button,
                                   stolen, sizeof(stolen));
    mapFinish(QStringLiteral("Gamepad %1")
                 .arg(intvsession_pad_button_name(button)),
             stolen);
}

/* A keypad digit/action button: real hardware holds while pressed, which
 * QAbstractButton's own pressed()/released() signals give directly. Map
 * mode intercepts both (see this file's own header): PickTarget turns a
 * press into "select this button as the map target" instead of an
 * injection, and WaitInput swallows clicks outright -- at that point the
 * window wants a *keyboard or gamepad* press, not another mouse click
 * reinterpreted as one. */
QPushButton *makeKey(const QString &label, intvsession *session,
                    intvsession_pad_side side, intvsession_key key)
{
    auto *btn = new QPushButton(label);
    btn->setFixedSize(48, 40);
    QObject::connect(btn, &QPushButton::pressed, btn,
                     [session, side, key] {
                         if (g_mapState == MapState::PickTarget) {
                             mapSelectTarget(side, key);
                             return;
                         }
                         if (g_mapState == MapState::WaitInput)
                             return;
                         intvsession_pad_key(session, side, key, 1);
                     });
    QObject::connect(btn, &QPushButton::released, btn,
                     [session, side, key] {
                         if (g_mapState != MapState::Idle)
                             return;
                         intvsession_pad_key(session, side, key, 0);
                     });
    g_keyButtons[side][key] = btn;
    return btn;
}

void onMapClicked()
{
    if (g_mapState != MapState::Idle) {
        mapResetUi();
        return;
    }
    g_mapState = MapState::PickTarget;
    setHighlighted(g_mapBtn, true);
    mapSetStatus(QStringLiteral("Press button to map"));
}

void onResetClicked()
{
    mapResetUi();
    if (g_mapSession) {
        intvsession_bindings_reset(g_mapSession);
        mapSetStatus(QStringLiteral("Bindings reset to default"));
    }
}

/* The Map/Reset row beneath both controller panels -- see this file's own
 * header for the state machine onMapClicked/onResetClicked/mapSelectTarget/
 * mapComplete* implement together. Polls for a captured gamepad button on
 * g_gamepadPollTimer since core/src/gamepad_sdl.c's own capture result
 * lands on its background thread, not this one -- same reasoning as
 * DisplayWidget's repaint QTimer (see this file's own header). */
QWidget *buildMapRow(intvsession *session)
{
    auto *row = new QVBoxLayout;
    auto *buttons = new QHBoxLayout;

    g_mapBtn = new QPushButton(QStringLiteral("Map"));
    QObject::connect(g_mapBtn, &QPushButton::clicked, g_mapBtn, onMapClicked);
    buttons->addWidget(g_mapBtn);

    auto *resetBtn = new QPushButton(QStringLiteral("Reset Bindings"));
    QObject::connect(resetBtn, &QPushButton::clicked, resetBtn, onResetClicked);
    buttons->addWidget(resetBtn);

    auto *buttonsWrap = new QHBoxLayout;
    buttonsWrap->addStretch();
    buttonsWrap->addLayout(buttons);
    buttonsWrap->addStretch();
    row->addLayout(buttonsWrap);

    g_statusLabel = new QLabel;
    g_statusLabel->setAlignment(Qt::AlignHCenter);
    g_statusLabel->setWordWrap(true);
    row->addWidget(g_statusLabel);

    g_gamepadPollTimer = new QTimer(g_mapBtn);
    QObject::connect(g_gamepadPollTimer, &QTimer::timeout, g_mapBtn, [session] {
        intvsession_pad_button button;
        if (g_mapState != MapState::WaitInput)
            return;
        if (intvsession_gamepad_capture_poll(session, &button))
            mapCompleteButton(button);
    });

    auto *wrap = new QWidget;
    wrap->setLayout(row);
    return wrap;
}

QPointer<KeypadWindow> g_singleton;
QPointer<QWidget> g_topLevel;

} // namespace

void KeypadWindow::showFor(QWidget *parent, intvsession *session)
{
    if (g_singleton) {
        if (g_topLevel->isVisible()) {
            mapResetUi(); /* see closeEvent's own comment */
            g_topLevel->hide();
        } else {
            g_topLevel->show();
            g_topLevel->raise();
            g_topLevel->activateWindow();
        }
        return;
    }
    g_singleton = new KeypadWindow(parent, session);
    g_topLevel = g_singleton;
    g_topLevel->show();
}

KeypadWindow::KeypadWindow(QWidget *parent, intvsession *session)
    : QWidget(parent, Qt::Window), m_session(session)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(QStringLiteral("Keypad"));
    g_mapSession = session;

    auto *panels = new QHBoxLayout;
    panels->addWidget(buildController(INTVSESSION_PAD_LEFT,
                                      QStringLiteral("Left Controller")));
    panels->addWidget(buildController(INTVSESSION_PAD_RIGHT,
                                      QStringLiteral("Right Controller")));

    auto *outer = new QVBoxLayout(this);
    outer->addLayout(panels);
    outer->addWidget(buildMapRow(session));
}

QWidget *KeypadWindow::buildController(intvsession_pad_side side,
                                       const QString &title)
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);

    auto *label = new QLabel(title);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    label->setAlignment(Qt::AlignHCenter);
    layout->addWidget(label);

    static const struct { const char *label; intvsession_key key; }
        kDigits[12] = {
            {"1", INTVSESSION_KEY_1}, {"2", INTVSESSION_KEY_2},
            {"3", INTVSESSION_KEY_3}, {"4", INTVSESSION_KEY_4},
            {"5", INTVSESSION_KEY_5}, {"6", INTVSESSION_KEY_6},
            {"7", INTVSESSION_KEY_7}, {"8", INTVSESSION_KEY_8},
            {"9", INTVSESSION_KEY_9}, {"Clear", INTVSESSION_KEY_CLEAR},
            {"0", INTVSESSION_KEY_0}, {"Enter", INTVSESSION_KEY_ENTER},
        };
    auto *grid = new QGridLayout;
    for (int i = 0; i < 12; i++)
        grid->addWidget(makeKey(QString::fromUtf8(kDigits[i].label),
                                m_session, side, kDigits[i].key),
                        i / 3, i % 3);
    auto *gridWrap = new QHBoxLayout;
    gridWrap->addStretch();
    gridWrap->addLayout(grid);
    gridWrap->addStretch();
    layout->addLayout(gridWrap);

    auto *actionRow = new QHBoxLayout;
    actionRow->addStretch();
    actionRow->addWidget(makeKey(QStringLiteral("Top"), m_session, side,
                                 INTVSESSION_ACTION_TOP));
    actionRow->addWidget(makeKey(QStringLiteral("L-Lower"), m_session, side,
                                 INTVSESSION_ACTION_LOWER_LEFT));
    actionRow->addWidget(makeKey(QStringLiteral("R-Lower"), m_session, side,
                                 INTVSESSION_ACTION_LOWER_RIGHT));
    actionRow->addStretch();
    layout->addLayout(actionRow);

    auto *discRow = new QHBoxLayout;
    discRow->addStretch();
    discRow->addWidget(new DiscWidget(m_session, side));
    discRow->addStretch();
    layout->addLayout(discRow);

    return box;
}

void KeypadWindow::forwardKey(const QKeyEvent *event, int down)
{
    /* Map mode's keyboard half: a press while waiting for input completes
     * the mapping instead of reaching the machine at all (see this file's
     * own header). Only the press matters -- ignore the matching release
     * the same way a mouse click's own release is swallowed in makeKey's
     * lambdas above, so releasing the just-mapped key doesn't also get
     * forwarded as a live keystroke. */
    if (g_mapState == MapState::WaitInput) {
        if (down) {
            const quint32 keysym = intvKeysymForKeyEvent(event);
            if (keysym != 0)
                mapCompleteKey(keysym);
        }
        return;
    }
    if (g_mapState == MapState::PickTarget)
        return; /* still swallow -- waiting for a button click, not this */

    intvForwardKey(m_session, event, down);
}

void KeypadWindow::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forwardKey(event, 1);
    event->accept();
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forwardKey(event, 0);
    event->accept();
}

void KeypadWindow::closeEvent(QCloseEvent *event)
{
    /* A Map sequence left armed behind a hidden window would still be
     * capturing gamepad input on the next show -- abort it, same as
     * clicking Map again to cancel. */
    mapResetUi();
    /* Singleton: hide and reuse, matching the GNOME port's own window. */
    hide();
    event->ignore();
}
