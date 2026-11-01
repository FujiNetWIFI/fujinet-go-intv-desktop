/*
 * The emulator display on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame (160x200) and draws one textured quad letterboxed to
 * 4:3, only when the session has published a new frame (see FrameFeed). Qt
 * picks Vulkan, Metal, D3D or OpenGL for it.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Key events are translated by KeyForward (intvForwardKey); press and
 * release both matter, since jzIntv's pad_t tracks a held key by level.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFocusEvent>
#include <QKeyEvent>
#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <vector>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "intvsession.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(intvsession *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    /* Tab belongs to the ECS keyboard, not to focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    intvsession *m_session;
    FrameFeed *m_feed;
    /* The session writes tightly-packed XRGB8888 rows straight into this;
     * the upload reads from it. The shader ignores the X byte. */
    std::vector<uint32_t> m_frame;
    uint64_t m_serial = 0;
    bool m_dirty = false;      /* m_frame holds a frame not yet uploaded */

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};
