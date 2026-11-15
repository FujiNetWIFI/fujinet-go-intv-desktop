/*
 * The app's accent as Qt colours, from the session header's macros: the
 * fill, the ink on it, and the accent as text or lines (the light variant on
 * a dark theme, where the fill itself is only ~1.5:1).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QWidget>

#include "intvsession.h"

inline QColor intvRgb(unsigned rgb)
{
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

inline QColor intvAccentColor() { return intvRgb(INTVSESSION_ACCENT_RGB); }
inline QColor intvAccentInk() { return intvRgb(INTVSESSION_ACCENT_INK_RGB); }

/* The accent for text and lines: light on a dark theme, the fill otherwise. */
inline QColor intvAccentTextColor(const QWidget *w)
{
    if (w && w->palette().color(QPalette::Window).lightness() < 128)
        return intvRgb(INTVSESSION_ACCENT_LIGHT_RGB);
    return intvAccentColor();
}

/* A highlight fill and the ink on it: the dark green with white ink on a light
 * theme; on a dark one the dark green barely shows, so the light green with
 * black ink. */
inline bool intvDarkTheme(const QWidget *w)
{
    return w && w->palette().color(QPalette::Window).lightness() < 128;
}
inline QColor intvAccentFill(const QWidget *w)
{
    return intvDarkTheme(w) ? intvRgb(INTVSESSION_ACCENT_LIGHT_RGB) : intvAccentColor();
}
inline QColor intvAccentFillInk(const QWidget *w)
{
    return intvDarkTheme(w) ? QColor(Qt::black) : intvAccentInk();
}
