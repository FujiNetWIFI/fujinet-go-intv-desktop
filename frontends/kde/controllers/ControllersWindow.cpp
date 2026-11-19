/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * One painted widget over the session's layout (intvsession_controls_layout,
 * in layout units; intvsession_controls_hit says what a click landed on), so
 * every frontend shows the same parts in the same places: the Intellivision II
 * handsets (a light-grey slab, the black keypad with its red-outlined keys, the
 * disc in its cream ring, four grey side buttons) -- two more for the ECS pair
 * when the ECS is on -- over the console's front strip with its label band and
 * its RESET key. The colours are the INTVSESSION_*_RGB defines, read off
 * photographs of the real thing (controllers-port/DESIGN-intv.md).
 *
 * Gestures act on mouse press AND release, not "clicked": the machine scans
 * its pads every frame, so a press present only for the instant of a click
 * would fall between frames. A key or side button is HELD while the mouse is
 * down on it (dragging off lets go, back on holds again); pressing the disc
 * and dragging holds the sector under the pointer (intvsession_pointer_set, 16
 * of them) and releasing lets go; RESET fires on the release. Everything the
 * mouse holds is let go on release, on losing the focus, on hiding, on the ECS
 * layout changing and on entering Map. A hold whose release was lost (a mouse
 * move with no button down) is let go too.
 *
 * What the machine sees held -- host keyboard, gamepads and this window -- is
 * lit (a ring in the HELD colour and a fill), read from the session's source
 * table, never from the decoded pad byte (a keypad press also grounds disc and
 * button lines on the real hardware), so the window doubles as an input tester
 * for a new pad. Keys typed with the window active go to the machine as in the
 * main window (keyboard_mode included).
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts and gamepad names are elided, so neither
 * Map mode nor a long name resizes it. The timers run only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a key, a side button, a
 * disc sector, the console's RESET or the two system buttons below), then press
 * a host key or a gamepad button. Rebinding steals the key from whatever held
 * it (named in the hint). Escape cancels; while Map is armed nothing is typed
 * into the machine. F9-F12 cannot be bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSignalBlocker>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "../KeyForward.h"
#include "../debugger/Accent.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kDimAlpha = 0.4;        /* an unpicked part while Map waits for a key */
constexpr qreal kUnit = 17.0;           /* pixels per layout unit */
constexpr int kMargin = 12;             /* round the drawing */
constexpr int kTextRows = 3;

QColor rgb(unsigned value) { return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF); }

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont fnt, qreal pixels, bool bold, bool italic = false)
{
    fnt.setPixelSize(qMax(1, qRound(pixels)));
    fnt.setBold(bold);
    fnt.setItalic(italic);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt, int align = Qt::AlignCenter)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = fnt.pixelSize();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5)
            break;
        fnt.setPixelSize(size - 1);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, align, text);
}

/* Long key names shortened to fit a part (tooltips keep the full name). */
QString shorten(QString text)
{
    text.replace(QStringLiteral("Up Arrow"), QStringLiteral("\u2191"));
    text.replace(QStringLiteral("Down Arrow"), QStringLiteral("\u2193"));
    text.replace(QStringLiteral("Left Arrow"), QStringLiteral("\u2190"));
    text.replace(QStringLiteral("Right Arrow"), QStringLiteral("\u2192"));
    text.replace(QStringLiteral("Keypad "), QStringLiteral("KP "));
    text.replace(QStringLiteral("Numpad "), QStringLiteral("KP "));
    text.replace(QStringLiteral("Left "), QStringLiteral("L"));
    text.replace(QStringLiteral("Right "), QStringLiteral("R"));
    text.replace(QStringLiteral("Backspace"), QStringLiteral("BkSp"));
    text.replace(QStringLiteral("Escape"), QStringLiteral("Esc"));
    text.replace(QStringLiteral("Return"), QStringLiteral("Ret"));
    text.replace(QStringLiteral("Enter"), QStringLiteral("Ent"));
    text.replace(QStringLiteral("Control"), QStringLiteral("Ctl"));
    text.replace(QStringLiteral("Ctrl"), QStringLiteral("Ctl"));
    text.replace(QStringLiteral("Shift"), QStringLiteral("Sh"));
    text.replace(QStringLiteral("Down"), QStringLiteral("Dn"));
    return text;
}

bool sameTarget(const intvsession_key_mapping &lhs, const intvsession_key_mapping &rhs)
{
    if (lhs.kind != rhs.kind) return false;
    switch (lhs.kind) {
    case INTVSESSION_MAP_KEY: return lhs.side == rhs.side && lhs.key == rhs.key;
    case INTVSESSION_MAP_DISC: return lhs.side == rhs.side && lhs.direction == rhs.direction;
    case INTVSESSION_MAP_SYSACT: return lhs.sysact == rhs.sysact;
    default: return false;
    }
}

QString keyText(intvsession *session, const intvsession_key_mapping &target)
{
    const intvsession_binding bnd = intvsession_target_binding_get(session, target);
    char key[48] = "";
    if (bnd.keysym) intvsession_keysym_name(bnd.keysym, key, sizeof key);
    return local(key);
}

QString padText(intvsession *session, const intvsession_key_mapping &target)
{
    const intvsession_binding bnd = intvsession_target_binding_get(session, target);
    return bnd.button == INTVSESSION_PAD_BTN_NONE ? QString() : local(intvsession_pad_button_name(bnd.button));
}

} // namespace

/* ---- ControllersHint --------------------------------------------------------------- */

/* A label that never widens the window: it wraps into a fixed height. */
class ControllersHint : public QWidget {
public:
    ControllersHint(bool dim, int lines) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ControllersStage: the handsets and the console --------------------------------- */

class ControllersStage : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target) */
    ControllersStage(intvsession *session, std::function<void(const intvsession_key_mapping &)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Rebuilds the layout when the ECS state changed (it takes a restart). True if it did. */
    bool reload()
    {
        if (m_lay && intvsession_ecs_on(m_session) == m_ecs) return false;
        releaseAll();
        load();
        return true;
    }

    /* Lets go of everything the mouse holds (also on focus loss, Map entry and hide). */
    void releaseAll()
    {
        intvsession_release_pointer(m_session);
        m_hold = None;
        m_holdIdx = -1;
        m_holdDir = -1;
        m_resetDown = false;
        pollHeld();
        update();
    }

    /* -2 idle, -1 armed, 0 waiting for a key or pad button for target */
    void setMap(int state, const intvsession_key_mapping &target)
    {
        m_map = state;
        m_target = target;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the machine sees held (keyboard, gamepads, the mouse); repaints only on a change. */
    void pollHeld()
    {
        reload();
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const intvsession_control &ctl = m_lay[idx];
            int held;
            if (ctl.kind == INTVSESSION_CTL_KEY || ctl.kind == INTVSESSION_CTL_ACTION) {
                held = (intvsession_buttons_held(m_session, (intvsession_pad_side)ctl.side) >> ctl.target) & 1;
            } else if (ctl.kind == INTVSESSION_CTL_DISC) {
                held = intvsession_disc_held(m_session, (intvsession_pad_side)ctl.side);
            } else if (ctl.kind == INTVSESSION_CTL_BUTTON) {
                held = m_resetDown ? 1 : 0;
            } else {
                continue;
            }
            if (held != m_held[(size_t)idx]) {
                m_held[(size_t)idx] = held;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        /* opaque: Qt's Format_RGB32 grabs carry no alpha (the KDE transparency trap) */
        pnt.fillRect(rect(), palette().color(QPalette::Window));
        for (int idx = 0; idx < m_count; ++idx) {
            const intvsession_control &ctl = m_lay[idx];
            pnt.save();
            switch (ctl.kind) {
            case INTVSESSION_CTL_CAPTION: paintCaption(pnt, idx); break;
            case INTVSESSION_CTL_BODY: paintBody(pnt, idx); break;
            case INTVSESSION_CTL_KEY: paintKey(pnt, idx); break;
            case INTVSESSION_CTL_ACTION: paintAction(pnt, idx); break;
            case INTVSESSION_CTL_DISC: paintDisc(pnt, idx); break;
            case INTVSESSION_CTL_BUTTON: paintReset(pnt, idx); break;
            default: break;
            }
            pnt.restore();
            pnt.setOpacity(1.0);
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();     /* a stale hold: its release was lost */
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const intvsession_control &ctl = m_lay[idx];
        if (m_map != -2) {
            const intvsession_key_mapping target = targetAt(idx, pos);
            if (target.kind != INTVSESSION_MAP_NONE) m_onPick(target);
            return;
        }
        m_holdIdx = idx;
        if (ctl.kind == INTVSESSION_CTL_DISC) {
            m_hold = Disc;
            driveDisc(pos);
        } else if (ctl.kind == INTVSESSION_CTL_BUTTON) {
            m_hold = Reset;
            m_resetDown = true;
        } else {
            m_hold = Part;
            m_partDown = true;
            intvsession_pointer_set(m_session, (intvsession_pad_side)ctl.side, ctl.target, -1);
        }
        pollHeld();
        update();
    }

    /* Dragging off a part lets it go (and back on, holds it again). */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_hold != None && !(evt->buttons() & Qt::LeftButton)) {
            releaseAll();       /* the release never came (a grab lost) */
            return;
        }
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Disc) {
            driveDisc(pos);
            pollHeld();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                const intvsession_control &ctl = m_lay[m_holdIdx];
                if (over) intvsession_pointer_set(m_session, (intvsession_pad_side)ctl.side, ctl.target, -1);
                else intvsession_release_pointer(m_session);
                pollHeld();
                update();
            }
        } else if (m_hold == Reset) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_resetDown) {
                m_resetDown = over;
                pollHeld();
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const bool fire = m_hold == Reset && m_resetDown;
        const int idx = m_holdIdx;
        if (m_hold != None) releaseAll();
        if (fire) intvsession_sysaction_post(m_session, (intvsession_sysaction)m_lay[idx].target);
        m_hover = hitAt(evt->position());
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Part, Disc, Reset };

    void load()
    {
        m_ecs = intvsession_ecs_on(m_session);
        float wide = 0, tall = 0;
        m_lay = intvsession_controls_layout(m_ecs, &m_count, &wide, &tall);
        m_held.assign((size_t)m_count, 0);
        setFixedSize(qCeil(wide * kUnit) + 2 * kMargin, qCeil(tall * kUnit) + 2 * kMargin);
        update();
    }

    QRectF rectOf(int idx) const
    {
        const intvsession_control &ctl = m_lay[idx];
        return QRectF(kMargin + ctl.x * kUnit, kMargin + ctl.y * kUnit, ctl.w * kUnit, ctl.h * kUnit);
    }
    QPointF centreOf(int idx) const { return rectOf(idx).center(); }
    static qreal unit(qreal value) { return value * kUnit; }

    int hitAt(const QPointF &pos) const
    {
        return intvsession_controls_hit(m_ecs, float((pos.x() - kMargin) / kUnit), float((pos.y() - kMargin) / kUnit));
    }

    /* The disc sector under a point (0-15), or -1 in its dead centre. */
    int sectorAt(int idx, const QPointF &pos) const
    {
        return intvsession_controls_disc_dir(&m_lay[idx], float((pos.x() - kMargin) / kUnit),
                                             float((pos.y() - kMargin) / kUnit));
    }

    /* The Map target of a click on a part: a disc names the sector it landed in. */
    intvsession_key_mapping targetAt(int idx, const QPointF &pos) const
    {
        const intvsession_control &ctl = m_lay[idx];
        if (ctl.kind == INTVSESSION_CTL_DISC) {
            const int dir = sectorAt(idx, pos);
            if (dir < 0) return intvsession_key_mapping();
            return intvsession_target_disc((intvsession_pad_side)ctl.side, dir);
        }
        return intvsession_control_target(&ctl);
    }

    /* The mouse is dragging the disc of m_holdIdx: the sector it points into. */
    void driveDisc(const QPointF &pos)
    {
        const intvsession_control &ctl = m_lay[m_holdIdx];
        const int dir = sectorAt(m_holdIdx, pos);
        if (dir == m_holdDir) return;
        m_holdDir = dir;
        intvsession_pointer_set(m_session, (intvsession_pad_side)ctl.side, -1, dir);
    }

    QString bound(const intvsession_key_mapping &target) const
    {
        const QString key = keyText(m_session, target), pad = padText(m_session, target);
        QString text = key;
        if (!pad.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral(" / ")) + pad;
        return text.isEmpty() ? QStringLiteral("nothing") : text;
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const intvsession_control &ctl = m_lay[idx];
        const intvsession_key_mapping target = targetAt(idx, pos);
        if (ctl.kind == INTVSESSION_CTL_DISC) {
            const QString head = QStringLiteral("%1 disc: press and drag to hold a direction")
                                     .arg(local(intvsession_pad_side_name((intvsession_pad_side)ctl.side)));
            if (target.kind == INTVSESSION_MAP_NONE) return head;
            char name[96];
            intvsession_target_name(target, name, sizeof name);
            return QStringLiteral("%1\n%2 is bound to: %3").arg(head, local(name), bound(target));
        }
        char name[96];
        intvsession_target_name(target, name, sizeof name);
        return QStringLiteral("%1\nBound to: %2").arg(local(name), bound(target));
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or (in Map) the one picked. */
    bool picked(const intvsession_key_mapping &target) const { return m_map >= 0 && sameTarget(target, m_target); }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(int idx) const
    {
        if (m_map < 0) return false;
        const intvsession_control &ctl = m_lay[idx];
        if (ctl.kind == INTVSESSION_CTL_DISC)
            return !(m_target.kind == INTVSESSION_MAP_DISC && m_target.side == ctl.side);
        return !sameTarget(intvsession_control_target(&ctl), m_target);
    }

    /* The held look: a ring in the HELD colour and a 40% fill of it. */
    void heldLook(QPainter &pnt, const QPainterPath &shape, qreal alpha = 0.4) const
    {
        QColor tint = rgb(INTVSESSION_HELD_RGB);
        tint.setAlphaF(alpha);
        pnt.setPen(QPen(rgb(INTVSESSION_HELD_RGB), unit(0.18)));
        pnt.setBrush(tint);
        pnt.drawPath(shape);
    }

    /* ---- painting ---- */

    void paintCaption(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QString text = local(ctl.label);
        if (ctl.side >= 0) {
            pnt.setPen(palette().color(QPalette::WindowText));
            drawFitted(pnt, box, text, pixelFont(font(), unit(0.85), true));
        } else if (text.startsWith(QLatin1String("INTELLIVISION"))) {
            pnt.setPen(rgb(INTVSESSION_LEGEND_RGB));
            drawFitted(pnt, box, text, pixelFont(font(), unit(0.95), true, true), Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            pnt.setPen(rgb(INTVSESSION_MATTEL_RED_RGB));
            drawFitted(pnt, box, text, pixelFont(font(), unit(0.6), false, true), Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    void paintBody(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QString name = local(ctl.label);
        pnt.setPen(Qt::NoPen);
        if (name == QLatin1String("Console")) {
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, rgb(INTVSESSION_BEVEL_RGB));
            grad.setColorAt(0.12, rgb(INTVSESSION_CONSOLE_RGB));
            grad.setColorAt(1, rgb(INTVSESSION_CONSOLE_RGB).darker(108));
            pnt.setPen(QPen(rgb(INTVSESSION_SHADE_RGB), 1));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, unit(0.6), unit(0.6));
        } else if (name == QLatin1String("Band")) {
            pnt.setBrush(rgb(INTVSESSION_BAND_RGB));
            pnt.drawRect(box);
        } else if (name == QLatin1String("RedLine")) {
            pnt.setBrush(rgb(INTVSESSION_MATTEL_RED_RGB));
            pnt.drawRect(box);
        } else if (name == QLatin1String("Grille")) {
            pnt.setBrush(rgb(INTVSESSION_SHADE_RGB).lighter(110));
            pnt.drawRoundedRect(box, unit(0.2), unit(0.2));
            pnt.setPen(QPen(rgb(INTVSESSION_GRILLE_RGB).lighter(125), unit(0.16), Qt::SolidLine, Qt::RoundCap));
            for (int slat = 0; slat < 6; ++slat) {
                const qreal ypos = box.top() + box.height() * (slat + 0.6) / 6.2;
                pnt.drawLine(QPointF(box.left() + unit(0.25), ypos), QPointF(box.right() - unit(0.25), ypos));
            }
        } else if (name == QLatin1String("Block")) {
            pnt.setPen(QPen(rgb(INTVSESSION_BAND_RGB), 1));
            pnt.setBrush(rgb(INTVSESSION_BLOCK_RGB));
            pnt.drawRoundedRect(box, unit(0.25), unit(0.25));
        } else if (name == QLatin1String("OnOff")) {
            pnt.setOpacity(0.8);
            pnt.setPen(rgb(INTVSESSION_BLOCK_LEGEND_RGB));
            drawFitted(pnt, box, QStringLiteral("ON/OFF"), pixelFont(font(), unit(0.6), true));
        } else if (name == QLatin1String("Led")) {
            pnt.setBrush(rgb(INTVSESSION_LED_RGB));
            pnt.drawEllipse(box);
        } else if (name == QLatin1String("Cable")) {
            pnt.setBrush(rgb(INTVSESSION_CABLE_RGB));
            pnt.drawRoundedRect(box, unit(0.25), unit(0.25));
            pnt.setPen(QPen(QColor(255, 255, 255, 50), 1));
            for (qreal ypos = box.top() + unit(0.12); ypos < box.bottom() - unit(0.1); ypos += unit(0.2))
                pnt.drawLine(QPointF(box.left() + 1, ypos), QPointF(box.right() - 1, ypos + unit(0.08)));
        } else if (name == QLatin1String("Handset")) {
            /* a soft shadow, then the slab with its lit top edge and shaded foot */
            pnt.setBrush(QColor(0, 0, 0, 40));
            pnt.drawRoundedRect(box.translated(unit(0.15), unit(0.22)), unit(0.8), unit(0.8));
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, rgb(INTVSESSION_BEVEL_RGB));
            grad.setColorAt(0.04, rgb(INTVSESSION_HANDSET_RGB));
            grad.setColorAt(0.92, rgb(INTVSESSION_HANDSET_RGB));
            grad.setColorAt(1, rgb(INTVSESSION_SHADE_RGB));
            pnt.setPen(QPen(rgb(INTVSESSION_SHADE_RGB), 1.2));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, unit(0.8), unit(0.8));
        } else if (name == QLatin1String("Keypad")) {
            /* the raised bezel round the plate, then the black plate */
            const QRectF bezel = box.adjusted(-unit(0.3), -unit(0.3), unit(0.3), unit(0.3));
            pnt.setBrush(Qt::NoBrush);
            pnt.setPen(QPen(rgb(INTVSESSION_BEVEL_RGB), 1.5));
            pnt.drawRoundedRect(bezel.translated(-0.8, -0.8), unit(0.6), unit(0.6));
            pnt.setPen(QPen(rgb(INTVSESSION_SHADE_RGB), 1.5));
            pnt.drawRoundedRect(bezel.translated(0.8, 0.8), unit(0.6), unit(0.6));
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(rgb(INTVSESSION_KEYPAD_RGB));
            pnt.drawRoundedRect(box, unit(0.4), unit(0.4));
        } else if (name == QLatin1String("Card")) {
            pnt.setBrush(Qt::NoBrush);
            pnt.setPen(QPen(rgb(INTVSESSION_CARD_EDGE_RGB), 1));
            pnt.drawRoundedRect(box, unit(0.5), unit(0.5));
        } else if (name == QLatin1String("Ring")) {
            QRadialGradient grad(box.center() - QPointF(box.width() * 0.12, box.height() * 0.15), box.width() * 0.7);
            grad.setColorAt(0, rgb(INTVSESSION_RING_RGB).lighter(112));
            grad.setColorAt(1, rgb(INTVSESSION_RING_RGB).darker(112));
            pnt.setPen(QPen(rgb(INTVSESSION_SHADE_RGB), 1.2));
            pnt.setBrush(grad);
            pnt.drawEllipse(box);
        }
    }

    /* A keypad key: black face, red outline, white legend; held it is the HELD
     * green solid with the legend turned black. */
    void paintKey(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const intvsession_key_mapping target = intvsession_control_target(&ctl);
        const bool on = m_held[(size_t)idx] != 0;
        const bool pick = !on && picked(target);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(idx) ? kDimAlpha : 1.0);

        QPainterPath shape;
        shape.addRoundedRect(box, unit(0.4), unit(0.4));
        if (on) {
            pnt.setPen(QPen(rgb(INTVSESSION_HELD_RGB), unit(0.18)));
            pnt.setBrush(rgb(INTVSESSION_HELD_RGB));
            pnt.drawPath(shape);
        } else {
            pnt.setPen(QPen(rgb(INTVSESSION_KEYRED_RGB), unit(0.14)));
            pnt.setBrush(over ? rgb(INTVSESSION_KEYFACE_RGB).lighter(250) : rgb(INTVSESSION_KEYFACE_RGB));
            pnt.drawPath(shape);
            if (pick) heldLook(pnt, shape);
        }
        const bool word = std::strlen(ctl.label) > 1;
        pnt.setPen(on ? QColor(Qt::black) : rgb(INTVSESSION_LEGEND_RGB));
        if (showBind()) {
            const QString key = shorten(keyText(m_session, target));
            drawFitted(pnt, box.adjusted(1, 0, -1, 0), key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 11, true));
        } else {
            drawFitted(pnt, box.adjusted(1, 0, -1, 0), local(ctl.label),
                       word ? pixelFont(font(), unit(0.62), false) : pixelFont(font(), unit(0.95), true));
        }
    }

    /* A side button: a plain grey tab, a faint T / L / R inside; held it gets the HELD ring and fill. */
    void paintAction(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const intvsession_key_mapping target = intvsession_control_target(&ctl);
        const bool on = m_held[(size_t)idx] != 0;
        const bool pick = !on && picked(target);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(idx) ? kDimAlpha : 1.0);

        QPainterPath shape;
        shape.addRoundedRect(box, unit(0.3), unit(0.3));
        pnt.setPen(QPen(rgb(INTVSESSION_TAB_EDGE_RGB), 1));
        pnt.setBrush(over ? rgb(INTVSESSION_TAB_RGB).lighter(112) : rgb(INTVSESSION_TAB_RGB));
        pnt.drawPath(shape);
        if (on) heldLook(pnt, shape, 0.55);
        else if (pick) heldLook(pnt, shape);
        pnt.setPen(on ? rgb(INTVSESSION_ACCENT_RGB) : rgb(INTVSESSION_TAB_LEGEND_RGB));
        if (showBind()) {
            /* a bound key name stands along the tab, turned */
            const QString key = shorten(keyText(m_session, target));
            pnt.translate(box.center());
            pnt.rotate(-90);
            drawFitted(pnt, QRectF(-box.height() / 2, -box.width() / 2, box.height(), box.width()),
                       key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 9, true));
        } else {
            drawFitted(pnt, box, local(ctl.label), pixelFont(font(), unit(0.8), true));
        }
    }

    /* The disc: a black pad with its lathe rings and 16 sector marks; the held
     * sector is a wedge from 22% of the radius to the rim. */
    void paintDisc(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QPointF mid = box.center();
        const qreal rad = box.width() / 2;
        const int held = m_held[(size_t)idx];
        pnt.setOpacity(dimmed(idx) ? kDimAlpha : 1.0);

        QRadialGradient grad(mid - QPointF(rad * 0.2, rad * 0.25), rad * 1.3);
        grad.setColorAt(0, rgb(INTVSESSION_DISC_RGB).lighter(135));
        grad.setColorAt(1, rgb(INTVSESSION_DISC_RGB));
        pnt.setPen(QPen(rgb(INTVSESSION_BAND_RGB), 1));
        pnt.setBrush(grad);
        pnt.drawEllipse(mid, rad, rad);
        /* the lathe finish */
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(rgb(INTVSESSION_DISC_RING_RGB), 1));
        for (qreal frac = 0.3; frac < 0.97; frac += 0.14) pnt.drawEllipse(mid, rad * frac, rad * frac);
        /* the 16 sectors */
        pnt.setPen(QPen(rgb(INTVSESSION_DISC_RING_RGB).lighter(115), 1));
        pnt.drawEllipse(mid, rad * INTVSESSION_DISC_DEADZONE_FRAC, rad * INTVSESSION_DISC_DEADZONE_FRAC);
        for (int sector = 0; sector < INTVSESSION_DISC_POSITIONS; ++sector) {
            const qreal ang = (sector + 0.5) * qDegreesToRadians(INTVSESSION_DISC_SECTOR_DEG);
            const QPointF dir(std::cos(ang), -std::sin(ang));
            pnt.drawLine(mid + dir * rad * INTVSESSION_DISC_DEADZONE_FRAC, mid + dir * rad * 0.97);
        }
        if (held >= 0) wedge(pnt, mid, rad, held, 0.75, 2.0);
        if (mapping() && m_target.kind == INTVSESSION_MAP_DISC && m_target.side == ctl.side && m_target.direction >= 0)
            wedge(pnt, mid, rad, m_target.direction, 0.4, 2.0);
        if (showBind()) {
            pnt.setPen(Qt::white);
            const QFont small = pixelFont(font(), 8, true);
            for (int sector = 0; sector < INTVSESSION_DISC_POSITIONS; ++sector) {
                const QString key = shorten(keyText(m_session, intvsession_target_disc((intvsession_pad_side)ctl.side, sector)));
                if (key.isEmpty()) continue;
                /* even sectors inside, odd ones outside, so neighbours do not touch */
                const qreal ang = sector * qDegreesToRadians(INTVSESSION_DISC_SECTOR_DEG);
                const QPointF spot = mid + QPointF(std::cos(ang), -std::sin(ang)) * rad * (sector % 2 ? 0.82 : 0.5);
                drawFitted(pnt, QRectF(spot.x() - rad * 0.2, spot.y() - 6, rad * 0.4, 12), key, small);
            }
        }
    }

    void wedge(QPainter &pnt, const QPointF &mid, qreal rad, int sector, qreal alpha, qreal ring) const
    {
        const QRectF outer(mid.x() - rad, mid.y() - rad, rad * 2, rad * 2);
        const qreal inRad = rad * INTVSESSION_DISC_DEADZONE_FRAC;
        const QRectF inner(mid.x() - inRad, mid.y() - inRad, inRad * 2, inRad * 2);
        const qreal start = sector * INTVSESSION_DISC_SECTOR_DEG - INTVSESSION_DISC_SECTOR_DEG / 2;
        QPainterPath shape;
        shape.arcMoveTo(outer, start);
        shape.arcTo(outer, start, INTVSESSION_DISC_SECTOR_DEG);
        shape.arcTo(inner, start + INTVSESSION_DISC_SECTOR_DEG, -INTVSESSION_DISC_SECTOR_DEG);
        shape.closeSubpath();
        QColor tint = rgb(INTVSESSION_HELD_RGB);
        tint.setAlphaF(alpha);
        pnt.setPen(QPen(rgb(INTVSESSION_HELD_RGB), ring));
        pnt.setBrush(tint);
        pnt.drawPath(shape);
    }

    /* The console's RESET half of the black block (the ON/OFF half beside it is dimmed). */
    void paintReset(QPainter &pnt, int idx)
    {
        const intvsession_control &ctl = m_lay[idx];
        const intvsession_key_mapping target = intvsession_control_target(&ctl);
        const bool on = m_held[(size_t)idx] != 0;
        const bool pick = !on && picked(target);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(idx) ? kDimAlpha : 1.0);

        QPainterPath shape;
        shape.addRoundedRect(box, unit(0.2), unit(0.2));
        pnt.setPen(QPen(rgb(INTVSESSION_BAND_RGB), 1));
        pnt.setBrush(over ? rgb(INTVSESSION_BLOCK_RGB).lighter(135) : rgb(INTVSESSION_BLOCK_RGB).lighter(112));
        pnt.drawPath(shape);
        if (on) heldLook(pnt, shape, 0.5);
        else if (pick) heldLook(pnt, shape);
        pnt.setPen(on ? QColor(Qt::white) : rgb(INTVSESSION_BLOCK_LEGEND_RGB).lighter(125));
        if (showBind()) {
            const QString key = shorten(keyText(m_session, target));
            drawFitted(pnt, box, key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 10, true));
        } else {
            drawFitted(pnt, box, local(ctl.label), pixelFont(font(), unit(0.62), true));
        }
    }

    intvsession *m_session;
    std::function<void(const intvsession_key_mapping &)> m_onPick;
    const intvsession_control *m_lay = nullptr; /* the session's static layout table */
    int m_count = 0;
    int m_ecs = -1;                             /* the layout drawn */
    std::vector<int> m_held;                    /* per layout entry: 0/1, or a disc's held sector (-1 none) */
    bool m_showBindings = false;
    int m_map = -2;
    intvsession_key_mapping m_target = {};
    Hold m_hold = None;
    int m_holdIdx = -1;                         /* the part the mouse holds */
    int m_holdDir = -1;                         /* the disc's drag, as last sent */
    bool m_partDown = false;                    /* the pointer is still over the part it pressed */
    bool m_resetDown = false;
    int m_hover = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

ControllersWindow::ControllersWindow(intvsession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of
     * fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers (F9)"));
    setProperty("intvHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new ControllersStage(session, [this](const intvsession_key_mapping &target) { pick(target); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    auto *crossed = new ControllersHint(true, kTextRows);
    crossed->setText(QStringLiteral("The side buttons are crossed by design: the right-hand modifier keys "
                                    "(Shift, Ctrl, Alt) drive the LEFT controller's buttons, as in jzIntv's own default map."));
    root->addWidget(crossed);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        intvsession_binding_clear(m_session, m_target);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        intvsession_bindings_reset(m_session);
        for (int pad = 0, count = intvsession_gamepad_count(m_session); pad < count; pad++)
            intvsession_gamepad_assign(m_session, pad, -1);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
        rebuildPads();
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(intvsession_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        intvsession_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(intvHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    /* The machine's own actions that are not on the picture: they are here,
     * firing when clicked and picked for rebinding in Map. (Reset Game is the
     * console's RESET.) */
    auto *sysRow = new QHBoxLayout;
    for (int sysact = INTVSESSION_SYSACT_RESET_CONFIG; sysact < INTVSESSION_SYSACT_COUNT; ++sysact) {
        auto *button = new QPushButton(local(intvsession_sysaction_name((intvsession_sysaction)sysact)));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [this, sysact] {
            const intvsession_key_mapping target = intvsession_target_sysaction((intvsession_sysaction)sysact);
            if (m_mapState != -2) { pick(target); return; }
            intvsession_sysaction_post(m_session, (intvsession_sysaction)sysact);
        });
        m_sysButton[sysact] = button;
        sysRow->addWidget(button);
    }
    sysRow->addStretch(1);
    root->addLayout(sysRow);

    m_hint = new ControllersHint(true, kTextRows);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_stage->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    rebuildPads();
    setMapState(-2);
    m_stage->pollHeld();
}

void ControllersWindow::present()
{
    rebuildPads();
    show();
    raise();
    activateWindow();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::pick(const intvsession_key_mapping &target)
{
    setMapState(0, target);
}

void ControllersWindow::setMapState(int state, const intvsession_key_mapping &target)
{
    /* Entering (or changing) Map: nothing may stay held by the mouse, and the
     * keyboard's holds go too -- Map swallows key-ups, so a key held into it
     * would stick. */
    m_stage->releaseAll();
    intvsession_release_all(m_session);
    m_mapState = state;
    m_target = state >= 0 ? target : intvsession_key_mapping();
    m_clearButton->setEnabled(state >= 0);
    for (int sysact = INTVSESSION_SYSACT_RESET_CONFIG; sysact < INTVSESSION_SYSACT_COUNT; ++sysact)
        m_sysButton[sysact]->setStyleSheet(state >= 0 && m_target.kind == INTVSESSION_MAP_SYSACT && m_target.sysact == sysact
            ? QStringLiteral("background: %1; color: %2;").arg(intvAccentFill(this).name(), intvAccentFillInk(this).name())
            : QString());
    if (state == -2) {
        m_captureTimer.stop();
        intvsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Press a key or side button with the mouse to hold it, press and drag the disc for a "
                                       "direction, click RESET to reset the game."));
    } else if (state == -1) {
        m_captureTimer.stop();
        intvsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: %2;")
                                       .arg(intvAccentFill(this).name(), intvAccentFillInk(this).name()));
        m_hint->setText(QStringLiteral("Click a part to remap it (a sector of the disc for a direction). Escape leaves Map."));
    } else {
        char name[96];
        intvsession_target_name(m_target, name, sizeof name);
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).").arg(local(name)));
        intvsession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState, m_target);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("Bound %1").arg(what)
                                     : QStringLiteral("Bound %1 (was %2)").arg(what, stolen));
}

void ControllersWindow::pollCapture()
{
    intvsession_pad_button button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!intvsession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    intvsession_target_set_button(m_session, m_target, button, stolen, sizeof stolen);
    bound(local(intvsession_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes and a changed ECS state. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = intvsession_gamepad_generation(m_session);
    if (gen != m_padGen || intvsession_ecs_on(m_session) != m_padEcs) rebuildPads();
    else refreshPadSides();
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadSides()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = intvsession_gamepad_effective_side(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("plays %1").arg(local(intvsession_pad_side_name((intvsession_pad_side)eff)))
                                      : QStringLiteral("plays nothing");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the controller it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    m_padGen = intvsession_gamepad_generation(m_session);
    m_padEcs = intvsession_ecs_on(m_session);
    const int sides = m_padEcs ? 4 : 2;
    const int maxRows = m_padEcs ? 4 : 3;
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * maxRows + m_padBox->spacing() * (maxRows - 1));
    const int count = intvsession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < maxRows; ++idx) {
        char name[128];
        intvsession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ControllersHint(false, 1);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0.. = that controller */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItem(QStringLiteral("Automatic"));
        for (int side = 0; side < sides; ++side)
            box->addItem(local(intvsession_pad_side_name((intvsession_pad_side)side)));
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
    if (count > maxRows)
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Settings").arg(count - maxRows));
    refreshPadSides();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer.start(40);      /* 25 Hz */
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    m_swallow = 0;
    QWidget::hideEvent(evt);
}

/* Closing only hides: the window is kept for the next F9. */
void ControllersWindow::closeEvent(QCloseEvent *evt)
{
    hide();
    evt->ignore();
}

/* Losing the focus lets go of the mouse's holds and the keyboard's. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        m_stage->releaseAll();
        intvsession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

/* While Map waits for a key, menu shortcuts are taken here instead, so the hint
 * can say F9-F12 are reserved (and so nothing but Escape closes Map). */
bool ControllersWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && m_mapState >= 0) {
        evt->accept();
        return true;
    }
    if (evt->type() == QEvent::WindowDeactivate) {
        m_stage->releaseAll();
        intvsession_release_all(m_session);
        m_swallow = 0;
    }
    return QWidget::event(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    /* F9-F12 are never bindings: in Map they only explain */
    const int qtKey = evt->key();
    if (qtKey >= Qt::Key_F9 && qtKey <= Qt::Key_F12) {
        if (evt->isAutoRepeat()) return;
        if (m_mapState >= 0) m_hint->setText(QStringLiteral("F9, F10, F11 and F12 are reserved and cannot be bound."));
        else if (qtKey == Qt::Key_F9) hide();
        else if (qtKey == Qt::Key_F10 && onToggleEcsKeyboard) onToggleEcsKeyboard();
        else if (qtKey == Qt::Key_F12 && onShowDebugger) onShowDebugger();
        return;
    }
    /* Ctrl+R is the main window's Reset to CONFIG, never the right disc's R */
    if (m_mapState == -2 && qtKey == Qt::Key_R && (evt->modifiers() & Qt::ControlModifier)) {
        if (!evt->isAutoRepeat() && onResetToConfig) onResetToConfig();
        return;
    }
    const uint32_t keysym = intvKeysymForKeyEvent(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (qtKey == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (intvsession_key_reserved(keysym)) {
            m_hint->setText(QStringLiteral("F9, F10, F11 and F12 are reserved and cannot be bound."));
            return;
        }
        char name[48] = "", stolen[128] = "";
        intvsession_target_set_key(m_session, m_target, keysym, stolen, sizeof stolen);
        intvsession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(local(name), local(stolen));
        return;
    }
    if (evt->isAutoRepeat()) return;
    /* Otherwise what the main window does with it (keyboard_mode included) */
    intvForwardKey(m_session, evt, 1);
    evt->accept();
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = intvKeysymForKeyEvent(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    intvForwardKey(m_session, evt, 0);
    evt->accept();
}
