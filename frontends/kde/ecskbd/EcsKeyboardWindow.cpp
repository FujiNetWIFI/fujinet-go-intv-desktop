/*
 * EcsKeyboardWindow -- see EcsKeyboardWindow.h.
 *
 * One painted widget over intvsession_ecs_layout (layout units; every cap is
 * one of the ECS matrix's 48 physical keys, the SHIFT key twice as on the real
 * keyboard), the colours the INTVSESSION_ECS_*_RGB defines, read off photographs
 * (controllers-port/DESIGN-intv.md section 4). Clicking a key holds it while the
 * mouse is down (dragging off lets go); SHIFT and CTL latch -- a mouse cannot
 * hold two keys, so a click flips them and they stay lit until clicked again.
 * What the machine sees down is lit with the HELD colour, whoever put it there
 * (the mouse, the host keyboard in this window, a latch), read from
 * intvsession_ecs_key_held.
 *
 * Typing in this window always goes to the ECS matrix, whether or not
 * "keyboard_mode" is on, so opening it never silently changes that setting; the
 * line under the picture says which mode the host keyboard is in. Everything
 * is let go when the window loses the focus or hides (a latch too), so nothing
 * stays stuck in the emulated matrix.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EcsKeyboardWindow.h"

#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <vector>

#include "../KeyForward.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 20.0;           /* pixels per layout unit */
constexpr int kMargin = 12;

QColor rgb(unsigned value) { return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF); }
QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }
qreal unit(qreal value) { return value * kUnit; }

QFont pixelFont(QFont fnt, qreal pixels, bool bold, bool italic = false)
{
    fnt.setPixelSize(qMax(1, qRound(pixels)));
    fnt.setBold(bold);
    fnt.setItalic(italic);
    return fnt;
}

/* Text in rect, the font shrunk until it fits. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt, int align = Qt::AlignCenter)
{
    for (;;) {
        const QFontMetricsF metrics(fnt);
        const int size = fnt.pixelSize();
        if ((metrics.horizontalAdvance(text) <= rect.width() && metrics.height() <= rect.height() + 3) || size <= 5) break;
        fnt.setPixelSize(size - 1);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, align, text);
}

} // namespace

/* ---- EcsKeyboardHint ------------------------------------------------------------------ */

/* A label that never widens the window: it wraps into a fixed height. */
class EcsKeyboardHint : public QWidget {
public:
    EcsKeyboardHint()
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * 3); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(QColor(Qt::gray));
        pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
    }

private:
    QString m_text;
};

/* ---- EcsKeyboardStage: the keyboard ----------------------------------------------------- */

class EcsKeyboardStage : public QWidget {
public:
    explicit EcsKeyboardStage(intvsession *session) : m_session(session)
    {
        float wide = 0, tall = 0;
        m_lay = intvsession_ecs_layout(&m_count, &wide, &tall);
        m_held.assign((size_t)m_count, 0);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(qCeil(wide * kUnit) + 2 * kMargin, qCeil(tall * kUnit) + 2 * kMargin);
    }

    /* Lets go of what the mouse holds and every key in the matrix (latches too). */
    void releaseAll()
    {
        intvsession_ecs_keys_clear(m_session);
        m_holdIdx = -1;
        pollHeld();
        update();
    }

    /* Which keys are down in the machine; repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            if (m_lay[idx].kind != INTVSESSION_ECS_KEYCAP) continue;
            const char held = intvsession_ecs_key_held(m_session, (intvsession_ecs_key)m_lay[idx].key) ? 1 : 0;
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
            pnt.save();
            if (m_lay[idx].kind == INTVSESSION_ECS_BODY) paintBody(pnt, idx);
            else paintCap(pnt, idx);
            pnt.restore();
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_holdIdx >= 0) release();
        const int idx = hitAt(evt->position());
        if (idx < 0) return;
        const intvsession_ecs_cap &cap = m_lay[idx];
        const auto key = (intvsession_ecs_key)cap.key;
        if (cap.flags & INTVSESSION_ECSCAP_LATCH) {
            /* a latch flips on the press; the release does nothing */
            intvsession_ecs_key_set(m_session, key, intvsession_ecs_key_held(m_session, key) ? 0 : 1);
        } else {
            m_holdIdx = idx;
            intvsession_ecs_key_set(m_session, key, 1);
        }
        pollHeld();
    }

    /* Dragging off a key lets it go (and back on, holds it again). */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_holdIdx >= 0 && !(evt->buttons() & Qt::LeftButton)) {
            release();      /* the release never came (a grab lost) */
            return;
        }
        const QPointF pos = evt->position();
        const int hover = m_holdIdx < 0 ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_holdIdx >= 0) {
            const auto key = (intvsession_ecs_key)m_lay[m_holdIdx].key;
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != (intvsession_ecs_key_held(m_session, key) != 0)) {
                intvsession_ecs_key_set(m_session, key, over ? 1 : 0);
                pollHeld();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_holdIdx >= 0) release();
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

private:
    /* The mouse lets go of the key it holds (not a latch, not another source). */
    void release()
    {
        intvsession_ecs_key_set(m_session, (intvsession_ecs_key)m_lay[m_holdIdx].key, 0);
        m_holdIdx = -1;
        pollHeld();
        update();
    }

    QRectF rectOf(int idx) const
    {
        const intvsession_ecs_cap &cap = m_lay[idx];
        return QRectF(kMargin + cap.x * kUnit, kMargin + cap.y * kUnit, cap.w * kUnit, cap.h * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return intvsession_ecs_hit(float((pos.x() - kMargin) / kUnit), float((pos.y() - kMargin) / kUnit));
    }

    void paintBody(QPainter &pnt, int idx)
    {
        const intvsession_ecs_cap &cap = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QString name = local(cap.label);
        pnt.setPen(Qt::NoPen);
        if (name == QLatin1String("Case")) {
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, rgb(INTVSESSION_ECS_CASE_RGB).lighter(108));
            grad.setColorAt(1, rgb(INTVSESSION_ECS_CASE_RGB).darker(106));
            pnt.setPen(QPen(rgb(INTVSESSION_ECS_CASE_RGB).darker(145), 1.2));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, unit(0.8), unit(0.8));
        } else if (name == QLatin1String("Plate")) {
            pnt.setPen(QPen(rgb(INTVSESSION_ECS_PLATE_RGB).darker(125), 1.2));
            pnt.setBrush(rgb(INTVSESSION_ECS_PLATE_RGB));
            pnt.drawRoundedRect(box, unit(0.6), unit(0.6));
        } else if (name == QLatin1String("Label")) {
            pnt.setBrush(rgb(INTVSESSION_ECS_LABEL_RGB));
            pnt.drawRect(box);
            const QStringList lines = local(cap.shifted).split(QLatin1Char('|'));
            pnt.setPen(rgb(INTVSESSION_LEGEND_RGB));
            /* the second line keeps to the left: the red script owns the right */
            const QRectF text(box.left() + unit(0.5), box.top() + unit(0.15), unit(8.5), box.height() - unit(0.3));
            if (lines.size() > 0)
                drawFitted(pnt, QRectF(text.left(), text.top(), text.width(), unit(1.05)), lines[0],
                           pixelFont(font(), unit(0.95), true, true), Qt::AlignLeft | Qt::AlignVCenter);
            if (lines.size() > 1)
                drawFitted(pnt, QRectF(text.left(), text.top() + unit(1.0), unit(4.6), unit(0.7)), lines[1],
                           pixelFont(font(), unit(0.45), false), Qt::AlignLeft | Qt::AlignVCenter);
        } else if (name == QLatin1String("Mattel")) {
            pnt.setPen(rgb(INTVSESSION_MATTEL_RED_RGB));
            drawFitted(pnt, box, local(cap.shifted), pixelFont(font(), unit(0.42), false, true), Qt::AlignRight | Qt::AlignVCenter);
        } else if (name == QLatin1String("Vents")) {
            pnt.setPen(QPen(rgb(INTVSESSION_ECS_RIDGE_RGB), unit(0.14)));
            for (int ridge = 0; ridge < 6; ++ridge) {
                const qreal ypos = box.top() + box.height() * (ridge + 0.5) / 6;
                pnt.drawLine(QPointF(box.left(), ypos), QPointF(box.right(), ypos));
            }
        } else if (name == QLatin1String("Pinstripe")) {
            pnt.setBrush(rgb(INTVSESSION_ECS_PINSTRIPE_RGB));
            pnt.drawRect(box);
        } else if (name == QLatin1String("Cable")) {
            pnt.setBrush(rgb(INTVSESSION_CABLE_RGB));
            pnt.drawRoundedRect(box, unit(0.2), unit(0.2));
        }
    }

    /* A key: black with white legends (the shifted one small above), the RTN white
     * with a black legend; lit it is the HELD green with a black legend. */
    void paintCap(QPainter &pnt, int idx)
    {
        const intvsession_ecs_cap &cap = m_lay[idx];
        const QRectF box = rectOf(idx);
        const bool on = m_held[(size_t)idx] != 0;
        const bool over = !on && m_hover == idx && m_holdIdx < 0;
        const bool white = (cap.flags & INTVSESSION_ECSCAP_WHITE) != 0;

        QPainterPath shape;
        shape.addRoundedRect(box, unit(0.3), unit(0.3));
        pnt.setPen(QPen(on ? rgb(INTVSESSION_HELD_RGB) : QColor(0, 0, 0, 160), on ? unit(0.18) : 1.0));
        if (on) {
            pnt.setBrush(rgb(INTVSESSION_HELD_RGB));
        } else {
            QColor base = white ? rgb(INTVSESSION_ECS_RTN_RGB) : rgb(INTVSESSION_ECS_KEY_RGB);
            if (over) base = white ? base.darker(106) : base.lighter(160);
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, base.lighter(white ? 103 : 135));
            grad.setColorAt(1, base);
            pnt.setBrush(grad);
        }
        pnt.drawPath(shape);

        pnt.setPen(on ? QColor(Qt::black) : white ? rgb(INTVSESSION_ECS_RTN_INK_RGB) : rgb(INTVSESSION_LEGEND_RGB));
        const QString main = local(cap.label);
        if (cap.key == INTVSESSION_ECS_KEY_SPACE) return;       /* the space bar carries no legend */
        if (cap.shifted) {
            drawFitted(pnt, QRectF(box.left(), box.top() + unit(0.08), box.width(), unit(0.8)), local(cap.shifted),
                       pixelFont(font(), unit(0.6), true));
            const bool arrow = main.size() == 1 && main.at(0).unicode() >= 0x2190;
            drawFitted(pnt, QRectF(box.left(), box.top() + unit(0.85), box.width(), box.height() - unit(0.9)), main,
                       pixelFont(font(), unit(arrow ? 0.9 : 0.85), true));
        } else if (white) {
            drawFitted(pnt, QRectF(box.left(), box.top() + unit(0.1), box.width(), unit(0.9)), QStringLiteral("↵"),
                       pixelFont(font(), unit(0.85), true));
            drawFitted(pnt, QRectF(box.left(), box.top() + unit(1.0), box.width(), unit(0.8)), main,
                       pixelFont(font(), unit(0.48), true));
        } else {
            const bool word = main.size() > 1;
            drawFitted(pnt, box.adjusted(1, 0, -1, 0), main, pixelFont(font(), unit(word ? 0.55 : 0.85), true));
        }
    }

    intvsession *m_session;
    const intvsession_ecs_cap *m_lay = nullptr;     /* the session's static layout table */
    int m_count = 0;
    std::vector<char> m_held;                       /* per layout entry: the key is down */
    int m_holdIdx = -1;                             /* the key the mouse holds (not a latch) */
    int m_hover = -1;
};

/* ---- EcsKeyboardWindow ---------------------------------------------------------------- */

EcsKeyboardWindow::EcsKeyboardWindow(intvsession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("ecskeyboard"));
    setWindowTitle(QStringLiteral("ECS Keyboard (F10)"));
    setProperty("intvHelpContext", QStringLiteral("ecskbd"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new EcsKeyboardStage(session);
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    auto *row = new QHBoxLayout;
    m_notice = new EcsKeyboardHint;
    row->addWidget(m_notice, 1);
    row->addWidget(intvHelpButton(QStringLiteral("ecskbd")), 0, Qt::AlignTop);
    root->addLayout(row);
    refreshNotice();

    m_heldTimer = new QTimer(this);
    connect(m_heldTimer, &QTimer::timeout, this, [this] {
        m_stage->pollHeld();
        refreshNotice();
    });
}

void EcsKeyboardWindow::present()
{
    refreshNotice();
    show();
    raise();
    activateWindow();
}

/* Whether the ECS is on, and where the host keyboard goes. */
void EcsKeyboardWindow::refreshNotice()
{
    if (!intvsession_has_ecs_rom(m_session) || intvsession_get_int(m_session, "ecs", INTVSESSION_HW_AUTO) == INTVSESSION_HW_OFF)
        m_notice->setText(QStringLiteral("The ECS is not enabled: turn it on in Settings (a restart applies it)."));
    else if (intvsession_get_int(m_session, "keyboard_mode", 0))
        m_notice->setText(QStringLiteral("The host keyboard types on this keyboard (Settings > Input). Click SHIFT or CTL to latch it."));
    else
        m_notice->setText(QStringLiteral("Typing in this window goes to the ECS; elsewhere the host keyboard drives the hand "
                                         "controllers (Settings > Input). Click SHIFT or CTL to latch it."));
}

void EcsKeyboardWindow::releaseAll()
{
    m_stage->releaseAll();
}

void EcsKeyboardWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer->start(40);     /* 25 Hz */
    QWidget::showEvent(evt);
}

void EcsKeyboardWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer->stop();
    releaseAll();
    QWidget::hideEvent(evt);
}

/* Closing only hides: the window is kept for the next F10. */
void EcsKeyboardWindow::closeEvent(QCloseEvent *evt)
{
    hide();
    evt->ignore();
}

/* Losing the focus lets go of every key, the latches too. */
void EcsKeyboardWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) releaseAll();
    QWidget::changeEvent(evt);
}

bool EcsKeyboardWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::WindowDeactivate) releaseAll();
    return QWidget::event(evt);
}

void EcsKeyboardWindow::keyPressEvent(QKeyEvent *evt)
{
    const int qtKey = evt->key();
    if (qtKey >= Qt::Key_F9 && qtKey <= Qt::Key_F12) {
        if (evt->isAutoRepeat()) return;
        if (qtKey == Qt::Key_F10) hide();
        else if (qtKey == Qt::Key_F9 && onToggleControllers) onToggleControllers();
        else if (qtKey == Qt::Key_F12 && onShowDebugger) onShowDebugger();
        return;
    }
    if (qtKey == Qt::Key_R && (evt->modifiers() & Qt::ControlModifier)) {
        if (!evt->isAutoRepeat() && onResetToConfig) onResetToConfig();
        return;
    }
    if (evt->isAutoRepeat()) return;
    /* Deliberately NOT intvForwardKey: this window is the ECS keyboard, so it
     * routes to the ECS matrix whether or not "keyboard_mode" is on. */
    const quint32 keysym = intvKeysymForKeyEvent(evt);
    if (keysym)
        intvsession_ecs_key_event(m_session, evt->nativeScanCode(), keysym, intvEcsCharForKeyEvent(evt), 1);
    evt->accept();
}

void EcsKeyboardWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const quint32 keysym = intvKeysymForKeyEvent(evt);
    if (keysym)
        intvsession_ecs_key_event(m_session, evt->nativeScanCode(), keysym, intvEcsCharForKeyEvent(evt), 0);
    evt->accept();
}
