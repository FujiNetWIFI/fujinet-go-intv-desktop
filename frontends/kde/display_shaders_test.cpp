/*
 * The display's baked shaders must be loadable from the exact resource paths
 * DisplayWidget.cpp asks for. A wrong qt_add_shaders PREFIX put them at
 * :/shaders/shaders/..., the loads came up empty, pipeline creation failed
 * and the software display silently took over everywhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QFile>
#include <cstdio>
#include <rhi/qshader.h>

static int check(const char *path)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "display_shaders_test: %s is not in the resources\n", path);
        return 1;
    }
    const QShader s = QShader::fromSerialized(f.readAll());
    if (!s.isValid()) {
        fprintf(stderr, "display_shaders_test: %s is empty or not a shader\n", path);
        return 1;
    }
    return 0;
}

int main()
{
    return check(":/shaders/display.vert.qsb") | check(":/shaders/display.frag.qsb");
}
