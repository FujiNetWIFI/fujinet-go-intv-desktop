/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * plain QWidget, letterboxed to 4:3 -- NOT the frame buffer's own raw pixel
 * ratio (160/200 = 0.8, portrait): the STIC's pixels are not square, and
 * real Intellivision video (like jzIntv's own default display) fills a
 * standard NTSC 4:3 picture. Matches the reasoning in frontends/gnome/
 * display.c exactly. A QTimer poll drives the repaint (there is no
 * intvsession_notify_vsync to feed -- unlike CoCo/MSX, intv_host.c's own
 * paced thread already governs itself to NTSC/PAL speed independent of any
 * frontend, see frontends/gnome/display.c's own comment on this), so
 * there is nothing a QOpenGLWidget's frameSwapped signal would buy here --
 * plain QWidget avoids depending on the separate OpenGLWidgets Qt module
 * for no benefit.
 *
 * Key events are translated to intvsession.h's own private
 * INTVSESSION_KEYSYM_* numbering, NOT an X11/GDK keysym -- see
 * frontends/gnome/keysym_map.h's header comment for why that distinction
 * matters (a real bug was caught there: passing a toolkit's native keyval
 * straight through silently drops every arrow/numpad/modifier key while
 * ASCII letters keep working by coincidence). Both press and release are
 * forwarded: jzIntv's pad_t tracks held keys via direct bit injection, so
 * a missed release leaves that key down in the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>

namespace {

/* Qt key code -> intvsession.h's private INTVSESSION_KEYSYM_* numbering,
 * for the keys intvsession_key_from_keysym resolves by symbol rather than
 * by character. F9 (keypad)/F11 (fullscreen)/F12 (debugger) are the
 * frontend's own and never reach here -- DisplayWidget::event() lets those
 * three through as shortcuts instead of claiming them. */
quint32 keysymForKey(const QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Up:    return INTVSESSION_KEYSYM_UP;
    case Qt::Key_Down:  return INTVSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:  return INTVSESSION_KEYSYM_LEFT;
    case Qt::Key_Right: return INTVSESSION_KEYSYM_RIGHT;
    /* Qt reports NumLock-off navigation keypad keys with the same
     * Key_Insert/End/Down/... codes as the main keyboard, distinguished
     * only by the KeypadModifier flag -- so those cases are handled
     * before this switch, in forwardKey/keysymForKey's caller. This
     * switch only needs the NumLock-on digit/decimal/enter codes, which
     * Qt reports distinctly regardless of the modifier. */
    case Qt::Key_0: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_0; break;
    case Qt::Key_1: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_1; break;
    case Qt::Key_2: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_2; break;
    case Qt::Key_3: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_3; break;
    case Qt::Key_4: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_4; break;
    case Qt::Key_5: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_5; break;
    case Qt::Key_6: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_6; break;
    case Qt::Key_7: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_7; break;
    case Qt::Key_8: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_8; break;
    case Qt::Key_9: if (event->modifiers() & Qt::KeypadModifier) return INTVSESSION_KEYSYM_KP_9; break;
    case Qt::Key_Period:
    case Qt::Key_Comma:
        if (event->modifiers() & Qt::KeypadModifier)
            return INTVSESSION_KEYSYM_KP_PERIOD;
        break;
    case Qt::Key_Enter:
        if (event->modifiers() & Qt::KeypadModifier)
            return INTVSESSION_KEYSYM_KP_ENTER;
        break;
    /* Modifiers are real Intellivision action-button bindings (mapping.c
     * crosses RSHIFT/RALT/RCTRL to the LEFT controller and LSHIFT/LALT/
     * LCTRL to the right -- see intv_keymap.c), so forwarded rather than
     * swallowed -- but Qt, unlike GDK, reports left and right as the SAME
     * key code for Shift/Control/Alt, so both land on the left-side
     * binding here (matching the MSX port's own DisplayWidget.cpp
     * precedent for this exact platform limitation). Control and Alt are
     * handled in forwardKey's own fallback below, not here, since they
     * also double as printable-key modifiers.
    */
    case Qt::Key_Shift: return INTVSESSION_KEYSYM_LSHIFT;
    /* Non-printable keys the base controller map has no use for, but the
     * ECS keyboard map (forwarded below when "keyboard_mode" is on) does. */
    case Qt::Key_Escape:    return INTVSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Return:    return INTVSESSION_KEYSYM_RETURN;
    case Qt::Key_Backspace: return INTVSESSION_KEYSYM_BACKSPACE;
    default:
        break;
    }
    return 0;
}

} // namespace

DisplayWidget::DisplayWidget(intvsession *session, QWidget *parent)
    : QWidget(parent),
      m_session(session),
      m_frame((size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT, 0)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT);
    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] { update(); });
    m_tick->start(16); /* ~60Hz repaint poll */
}

QRectF DisplayWidget::destRect() const
{
    const qreal w = width(), h = height();
    constexpr qreal aspect = 4.0 / 3.0;
    qreal dw, dh;

    if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    uint64_t serial = m_serial;

    if (intvsession_copy_frame(m_session, m_frame.data(), &serial))
        m_serial = serial;

    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    const QImage frame((const uchar *)m_frame.data(), INTVSESSION_FB_WIDTH,
                       INTVSESSION_FB_HEIGHT, INTVSESSION_FB_WIDTH * 4,
                       QImage::Format_RGB32);
    p.drawImage(destRect(), frame);
}

bool DisplayWidget::event(QEvent *event)
{
    /* Claim every key except the reserved chords (F9 keypad, F11
     * fullscreen, F12 debugger) while focused, so menu accelerators
     * cannot shadow what the machine should receive. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F9 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void DisplayWidget::forwardKey(const QKeyEvent *event, int down)
{
    /* intvsession_key_from_keysym takes only the keysym -- no shift/ctrl
     * parameters exist to feed (matching the GNOME frontend's own
     * window.c, which doesn't compute them either) -- Ctrl is only
     * examined here for the unshifted-letter fallback just below. */
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    quint32 keysym = keysymForKey(event);
    if (keysym == 0) {
        if (event->key() == Qt::Key_Control)
            keysym = INTVSESSION_KEYSYM_LCTRL;
        else if (event->key() == Qt::Key_Alt)
            keysym = INTVSESSION_KEYSYM_LALT;
        else if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            /* Printable: Qt gives no text for a Ctrl combo, so fall back
             * to the unshifted letter, which is what the keysym contract
             * wants. */
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    if (keysym == 0)
        return;

    /* "ECS Keyboard" input mode (Settings, or toggled live from there)
     * steals the host keyboard for the ECS's own keyboard instead of the
     * hand controllers -- see intvsession_ecs_key_from_keysym's own
     * comment on why the two can't both claim it at once. */
    if (intvsession_get_int(m_session, "keyboard_mode", 0)) {
        intvsession_ecs_key key = intvsession_ecs_key_from_keysym(keysym);
        if (key != INTVSESSION_ECS_KEY_NONE)
            intvsession_ecs_key_set(m_session, key, down);
        return;
    }

    intvsession_key_mapping m = intvsession_key_from_keysym(keysym);
    if (m.kind == INTVSESSION_MAP_KEY)
        intvsession_pad_key(m_session, m.side, m.key, down);
    else if (m.kind == INTVSESSION_MAP_DISC)
        intvsession_pad_disc(m_session, m.side, down ? m.direction : -1);
}

void DisplayWidget::focusOutEvent(QFocusEvent *event)
{
    /* Losing keyboard focus shouldn't leave a key stuck down in whichever
     * matrix was active -- see intvsession_ecs_keys_clear's own comment. */
    intvsession_ecs_keys_clear(m_session);
    QWidget::focusOutEvent(event);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 1);
    event->accept();
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 0);
    event->accept();
}
