/*
 * qetnaviv2dbackingstore.cpp — Backing store implementation.
 *
 * flush() sequence (KMS modes):
 *  1. Unlock the blittable (drains any pending CPU writes + GPU fence).
 *  2. etna2d_blit(): copy the entire backing BO into the scanout back-buffer.
 *  3. etna2d_finish(): wait for the GPU blit to complete before flip.
 *  4. screen->pageFlip(): display the back-buffer (blocks for vblank).
 *
 * flush() sequence (Wayland mode):
 *  1. Unlock the blittable.
 *  2. Get the current free ring BO from the transport.
 *  3. etna2d_blit(): copy the backing BO into the ring BO.
 *  4. etna2d_finish(): wait for the GPU blit.
 *  5. transport->commitFrame(): mark dirty; actual wl_surface_attach+commit
 *     happens in the next wl_surface.frame callback (vsync-aligned).
 *
 * Reference: src/plugins/platforms/directfb/qdirectfbbackingstore.cpp
 */
#include "qetnaviv2dbackingstore.h"
#include "qetnaviv2dwaylandtransport.h"

#include <QDebug>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

static bool bsDebug()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
    return v > 0;
}

#define BS_DBG  if (bsDebug()) qDebug() << "[etnaviv2d|backingstore]"
#define BS_WARN qWarning() << "[etnaviv2d|backingstore]"

/* -----------------------------------------------------------------------
 * Construction / destruction
 * ----------------------------------------------------------------------- */
QEtnaviv2dBackingStore::QEtnaviv2dBackingStore(QWindow *window,
                                               QEtnaviv2dScreen *screen,
                                               QEtnaviv2dWaylandTransport *transport)
    : QPlatformBackingStore(window)
    , m_screen(screen)
    , m_transport(transport)
{
    BS_DBG << "created for window" << window
           << (transport ? "(wayland mode)" : "(KMS mode)");
    /* Allocate initial backing surface sized to the window. */
    resize(window->size(), QRegion());
}

QEtnaviv2dBackingStore::~QEtnaviv2dBackingStore()
{
    BS_DBG << "destroyed";
}

/* -----------------------------------------------------------------------
 * paintDevice — the surface Qt paints into
 * ----------------------------------------------------------------------- */
QPaintDevice *QEtnaviv2dBackingStore::paintDevice()
{
    return m_pixmap.data();
}

/* -----------------------------------------------------------------------
 * resize — reallocate backing surface on window geometry changes
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::resize(const QSize &size,
                                    const QRegion & /*staticContents*/)
{
    if (!size.isValid() || size.isEmpty())
        return;

    /* Nothing to do if size unchanged. */
    if (m_pmdata && m_pmdata->width() == size.width() &&
                    m_pmdata->height() == size.height()) {
        return;
    }

    BS_DBG << "resize" << size;

    /* Create a new EtnaBlittablePlatformPixmap; it owns an offscreen BO. */
    auto *pmdata = new EtnaBlittablePlatformPixmap();
    pmdata->resize(size.width(), size.height());

    m_pixmap.reset(new QPixmap(pmdata)); /* QPixmap takes ownership of pmdata */
    m_pmdata = pmdata;

    BS_DBG << "resize done: backing surf=" << (m_pmdata->etnaBlittable()
            ? m_pmdata->etnaBlittable()->surface() : nullptr);
}

/* -----------------------------------------------------------------------
 * flush — blit backing BO → target back-buffer, then page-flip or commit
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::flush(QWindow * /*window*/,
                                   const QRegion &region,
                                   const QPoint  &offset)
{
    Q_UNUSED(offset); /* offset is always (0,0) for full-screen QFbScreen */

    if (!m_pmdata || !m_pmdata->etnaBlittable()) {
        BS_WARN << "flush: no backing blittable";
        return;
    }

    EtnaBlittable *blitter      = m_pmdata->etnaBlittable();
    etna2d_context *ctx         = blitter->context();
    etna2d_surface *backingSurf = blitter->surface();

    /* ---- Wayland mode: blit into the ring BO, hand off to transport ---- */
    if (m_transport) {
        etna2d_surface *ringBo = m_transport->currentBo();
        if (!ringBo) {
            /* All ring slots are in use — Weston hasn't released any yet.
             * Drop the frame (back-pressure): Qt will repaint on the next
             * frame callback when a slot is released. */
            BS_DBG << "flush: wayland ring full — dropping frame (back-pressure)";
            return;
        }

        /* 1. Unlock backing surface */
        blitter->unlock();

        /* 2. GPU blit full backing surface into the ring BO */
        int w = etna2d_surface_width(backingSurf);
        int h = etna2d_surface_height(backingSurf);
        BS_DBG << "flush(wayland): blit" << w << "x" << h << "→ ring BO" << ringBo;

        etna2d_blit(ctx,
                    backingSurf, 0, 0,
                    ringBo,      0, 0,
                    w, h,
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);

        /* 3. Wait for GPU blit */
        if (etna2d_finish(ctx) != ETNA2D_OK) {
            BS_WARN << "flush(wayland): etna2d_finish failed";
            return;
        }

        /* 4. Hand off to transport — frame-callback will commit */
        m_transport->commitFrame(ringBo);
        return;
    }

    /* ---- KMS modes: existing path ---- */
    etna2d_surface *scanoutBack = m_screen->backSurface();

    if (!scanoutBack) {
        /* isStub() with no transport shouldn't happen, but guard it just
         * in case. */
        if (!m_screen->isStub())
            BS_WARN << "flush: no scanout back surface";
        return;
    }

    /* 1. Unlock: drain CPU writes + GPU fence for any raster-fallback ops. */
    blitter->unlock();

    /* 2. GPU blit the full backing surface into the scanout back-buffer.
     *    Full blit avoids ghosting with double-buffering (stale back buffer). */
    int surfW = etna2d_surface_width(backingSurf);
    int surfH = etna2d_surface_height(backingSurf);
    BS_DBG << "flush(KMS): full blit" << surfW << "x" << surfH
           << "(dirty=" << region.boundingRect() << "ignored for correctness)";

    etna2d_blit(ctx,
                backingSurf, 0, 0,
                scanoutBack, 0, 0,
                surfW, surfH,
                ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);

    /* 3. Wait for GPU blit to complete before handing the buffer to display. */
    if (etna2d_finish(ctx) != ETNA2D_OK) {
        BS_WARN << "flush(KMS): etna2d_finish failed";
        return;
    }

    /* 4. Page-flip (blocks until vblank). */
    if (m_screen->pageFlip() != 0)
        BS_WARN << "flush(KMS): pageFlip failed";
}

/* -----------------------------------------------------------------------
 * toImage — for screenshots / accessibility
 * ----------------------------------------------------------------------- */
QImage QEtnaviv2dBackingStore::toImage() const
{
    return m_pixmap ? m_pixmap->toImage() : QImage();
}

/* -----------------------------------------------------------------------
 * scroll — GPU blit within the backing surface (avoids CPU memcpy)
 * ----------------------------------------------------------------------- */
bool QEtnaviv2dBackingStore::scroll(const QRegion &area, int dx, int dy)
{
    if (!m_pmdata || !m_pmdata->etnaBlittable())
        return false;

    /* Unlock first so we don't mix CPU and GPU reads of the same surface. */
    m_pmdata->blittable()->unlock();

    EtnaBlittable *blitter  = m_pmdata->etnaBlittable();
    etna2d_context *ctx     = blitter->context();
    etna2d_surface *surf    = blitter->surface();

    for (const QRect &r : area) {
        if (r.isEmpty()) continue;
        etna2d_blit(ctx,
                    surf, r.x(),      r.y(),
                    surf, r.x() + dx, r.y() + dy,
                    r.width(), r.height(),
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
    }
    etna2d_flush(ctx);

    BS_DBG << "scroll" << area.boundingRect() << "dx=" << dx << "dy=" << dy;
    return true;
}

QT_END_NAMESPACE
