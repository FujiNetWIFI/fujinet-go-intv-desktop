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
 * The disc is a small custom QWidget (DiscWidget, local to this file) that
 * snaps to 8 positions (E/NE/N/NW/W/SW/S/SE) rather than the full 16 --
 * matching intv_disc_from_stick's own reasoning (core/src/gamepad_sdl.c):
 * the 8 odd half-step codes are unreachable from a keyboard and OR-combine
 * two adjacent cardinal bits, so a drag that clips one during a sweep
 * toward the intended direction can spuriously register a combined
 * reading. dy is negated before computing the angle because Qt widget
 * coordinates put y increasing DOWNWARD, while intv_host.h's disc_codes
 * numbering (0=E, 4=N, 8=W, 12=S) is defined in ordinary compass/math
 * terms where "up" is a lower y -- the same correction
 * frontends/gnome/keypad/keypad_window.c's direction_from_point applies,
 * and for the identical reason (an earlier version of that GTK file got
 * this backwards and sent north for a downward click; see that file's own
 * header for the story).
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
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kDiscSize = 150;
constexpr double kDeadzoneFrac = 0.22;

/* See this file's header for the dy-negation and 8-way-snap reasoning. */
int directionFromPoint(double dx, double dy, double radius)
{
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist < radius * kDeadzoneFrac)
        return -1;

    double angleDeg = std::atan2(-dy, dx) * 180.0 / M_PI;
    if (angleDeg < 0)
        angleDeg += 360.0;
    int dir = int(std::floor(angleDeg / 45.0 + 0.5)) % 8;
    if (dir < 0)
        dir += 8;
    return dir * 2;
}

class DiscWidget : public QWidget {
public:
    DiscWidget(intvsession *session, intvsession_pad_side side,
              QWidget *parent = nullptr)
        : QWidget(parent), m_session(session), m_side(side)
    {
        setFixedSize(kDiscSize, kDiscSize);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double cx = width() / 2.0, cy = height() / 2.0;
        const double r = std::min(width(), height()) / 2.0 - 4;

        p.setBrush(QColor(38, 38, 43));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), r, r);

        if (m_direction >= 0) {
            /* Qt's arcTo(rect, startAngle, sweepLength) already matches
             * directionFromPoint's own angle convention exactly: degrees,
             * 0 = East (3 o'clock), positive = counter-clockwise. No unit
             * conversion or sign flip needed here. */
            const double a0Deg = m_direction * 22.5 - 22.5;
            const double sweepDeg = 45.0;
            QPainterPath wedge(QPointF(cx, cy));
            wedge.arcTo(QRectF(cx - r, cy - r, 2 * r, 2 * r), a0Deg, sweepDeg);
            wedge.closeSubpath();
            p.setBrush(QColor(77, 140, 230));
            p.drawPath(wedge);
        }

        p.setPen(QPen(QColor(115, 115, 128), 1.5));
        for (int i = 0; i < 8; i++) {
            const double a = (i * 45.0 - 22.5) * M_PI / 180.0;
            p.drawLine(QPointF(cx, cy),
                      QPointF(cx + r * std::cos(a), cy - r * std::sin(a)));
        }
        p.drawEllipse(QPointF(cx, cy), r, r);

        p.setBrush(QColor(64, 64, 71));
        p.drawEllipse(QPointF(cx, cy), r * kDeadzoneFrac, r * kDeadzoneFrac);
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
        setDirection(directionFromPoint(pos.x() - r, pos.y() - r, r));
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

/* A keypad digit/action button: real hardware holds while pressed, which
 * QAbstractButton's own pressed()/released() signals give directly. */
QPushButton *makeKey(const QString &label, intvsession *session,
                    intvsession_pad_side side, intvsession_key key)
{
    auto *btn = new QPushButton(label);
    btn->setFixedSize(48, 40);
    QObject::connect(btn, &QPushButton::pressed, btn,
                     [session, side, key] {
                         intvsession_pad_key(session, side, key, 1);
                     });
    QObject::connect(btn, &QPushButton::released, btn,
                     [session, side, key] {
                         intvsession_pad_key(session, side, key, 0);
                     });
    return btn;
}

QPointer<KeypadWindow> g_singleton;
QPointer<QWidget> g_topLevel;

} // namespace

void KeypadWindow::showFor(QWidget *parent, intvsession *session)
{
    if (g_singleton) {
        if (g_topLevel->isVisible()) {
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

    auto *root = new QHBoxLayout(this);
    root->addWidget(buildController(INTVSESSION_PAD_LEFT,
                                    QStringLiteral("Left Controller")));
    root->addWidget(buildController(INTVSESSION_PAD_RIGHT,
                                    QStringLiteral("Right Controller")));
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
    /* Singleton: hide and reuse, matching the GNOME port's own window. */
    hide();
    event->ignore();
}
