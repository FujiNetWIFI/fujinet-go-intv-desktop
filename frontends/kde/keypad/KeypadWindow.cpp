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
