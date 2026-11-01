/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include "DisplayGeometry.h"
#include "KeyForward.h"

#include <QEvent>
#include <QFile>
#include <QMatrix4x4>
#include <QVector4D>
#include <QWindow>

/* A wrong resource path used to fail silently: the shader came up empty,
 * pipeline creation failed, and renderFailed swapped in the software display
 * on every machine. Say so out loud. */
static QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    QShader s = f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
    if (!s.isValid())
        qWarning("DisplayWidget: shader %s is missing or empty (resource path wrong?)", path);
    return s;
}

DisplayWidget::DisplayWidget(intvsession *session, QWidget *parent)
    : QRhiWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_frame((size_t)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT, 0)
{
    setMinimumSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_feed, &FrameFeed::frameReady, this, &DisplayWidget::onFrame);
}

DisplayWidget::~DisplayWidget()
{
    m_feed->setActive(false);
}

/* ---- idle ------------------------------------------------------------------
 * A hidden or minimised window has nobody to draw for: the feed unregisters
 * from the session, so the GUI thread gets no wakeups at all until it is
 * shown again. The machine itself keeps running (netplay must not stall). */

void DisplayWidget::updateActive()
{
    QWidget *w = window();
    m_feed->setActive(isVisible() && w && !w->isMinimized());
}

void DisplayWidget::showEvent(QShowEvent *e)
{
    QRhiWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void DisplayWidget::hideEvent(QHideEvent *e)
{
    QRhiWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QRhiWidget::eventFilter(watched, e);
}

bool DisplayWidget::event(QEvent *e)
{
    if (claimShortcut(e))
        return true;
    /* The compositor can mark a window occluded without minimising it */
    if (e->type() == QEvent::ScreenChangeInternal || e->type() == QEvent::WindowActivate)
        updateActive();
    return QRhiWidget::event(e);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        intvForwardKey(m_session, event, 1);
    event->accept();
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        intvForwardKey(m_session, event, 0);
    event->accept();
}

/* Losing keyboard focus shouldn't leave a key stuck down in whichever
 * matrix was active -- see intvsession_ecs_keys_clear's own comment. */
void DisplayWidget::focusOutEvent(QFocusEvent *event)
{
    intvsession_ecs_keys_clear(m_session);
    QRhiWidget::focusOutEvent(event);
}

/* ---- frames ---------------------------------------------------------------- */

void DisplayWidget::onFrame()
{
    if (!intvsession_copy_frame(m_session, m_frame.data(), &m_serial))
        return;
    m_dirty = true;
    update();
}

/* ---- GPU ------------------------------------------------------------------- */

void DisplayWidget::releaseResources()
{
    m_pipeline.reset();
    m_srb.reset();
    m_nearest.reset();
    m_tex.reset();
    m_ubuf.reset();
}

void DisplayWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        releaseResources();
        m_rhi = rhi();
    }
    if (m_pipeline)
        return;

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
    m_nearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_nearest->create();
    if (!m_tex) {
        m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8,
                                      QSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT)));
        m_tex->create();
        m_dirty = true;   /* a fresh texture is empty; a still picture would stay black */
    }

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_nearest.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/display.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/display.frag.qsb") },
    });
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_pipeline.reset();
        emit renderFailed();
    }
}

void DisplayWidget::render(QRhiCommandBuffer *cb)
{
    if (!m_pipeline)
        return;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    if (m_dirty) {
        QRhiTextureSubresourceUploadDescription sub;
        const qsizetype bytes = (qsizetype)INTVSESSION_FB_WIDTH * INTVSESSION_FB_HEIGHT * 4;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(m_frame.data()), bytes));
        sub.setDataStride(INTVSESSION_FB_WIDTH * 4);
        sub.setSourceSize(QSize(INTVSESSION_FB_WIDTH, INTVSESSION_FB_HEIGHT));
        u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, sub) }));
        m_dirty = false;
    }

    const QSize px = renderTarget()->pixelSize();
    const QRectF r = displayRect(QSizeF(px));

    /* pixel rect -> clip space (y up), then through the backend's correction
     * so Vulkan's flipped y comes out the right way up */
    auto clip = [&](double x, double y) {
        return m_rhi->clipSpaceCorrMatrix() *
               QVector4D(float(x / px.width() * 2.0 - 1.0), float(1.0 - y / px.height() * 2.0), 0.f, 1.f);
    };
    const QVector4D tl = clip(r.left(), r.top());
    const QVector4D br = clip(r.right(), r.bottom());
    const float rect[4] = { tl.x(), tl.y(), br.x(), br.y() };
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof rect, rect);

    cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
    cb->setShaderResources(m_srb.get());
    cb->draw(4);
    cb->endPass();
}
