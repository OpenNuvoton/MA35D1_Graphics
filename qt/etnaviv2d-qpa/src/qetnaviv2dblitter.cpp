/*
 * qetnaviv2dblitter.cpp — EtnaBlittable implementation.
 *
 * Design notes:
 *  - All GPU ops are queued in the etna2d_context and NOT flushed here.
 *    Flush happens once per frame in doUnlock() and in the backing store
 *    flush() path — this avoids the ~2.5-3 µs per-op DE state-reload cost
 *    turning into per-QPainter-call overhead.
 *  - doLock() returns a QImage over the WC-mapped BO.  Qt's raster engine
 *    will software-render into it.  After doUnlock() we call etna2d_finish()
 *    so the GPU sees any CPU writes before the next GPU op on this surface.
 *  - drawPixmap: if the source is NOT an EtnaBlittable (e.g. a PNG loaded
 *    into a QRasterPlatformPixmap) we return without doing anything — Qt will
 *    automatically fall back to the raster engine via lock().
 *
 * Reference: src/plugins/platforms/directfb/qdirectfbblitter.cpp
 */
#include "qetnaviv2dblitter.h"

#include <QDebug>
#include <QtGui/private/qpixmap_blitter_p.h>
#include <qpa/qplatformpixmap.h>

QT_BEGIN_NAMESPACE

/* -----------------------------------------------------------------------
 * Logging — respects QT_ETNAVIV2D_DEBUG env var (0/1).
 * ----------------------------------------------------------------------- */
static bool debugEnabled()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
    return v > 0;
}

/* When set to 1, source-pixmap unlock reverts to the conservative path:
 * etna2d_finish() is called on every drawPixmap source surface before the
 * blit op is queued.  This serialises all GPU ops and eliminates batching.
 * Default 0 (batch mode on): finish is deferred to frame boundary. */
static bool noBatch()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_NO_BATCH");
    return v > 0;
}

#define ETNA_DBG if (debugEnabled()) qDebug() << "[etnaviv2d|blitter]"
#define ETNA_WARN qWarning() << "[etnaviv2d|blitter]"

/* -----------------------------------------------------------------------
 * EtnaBlittablePlatformPixmap — statics
 * ----------------------------------------------------------------------- */
etna2d_context *EtnaBlittablePlatformPixmap::s_ctx = nullptr;
etna2d_device  *EtnaBlittablePlatformPixmap::s_dev = nullptr;

EtnaBlittablePlatformPixmap::EtnaBlittablePlatformPixmap()
    : QBlittablePlatformPixmap()
{
}

void EtnaBlittablePlatformPixmap::setGlobalContext(etna2d_context *ctx,
                                                    etna2d_device  *dev)
{
    s_ctx = ctx;
    s_dev = dev;
    ETNA_DBG << "global context set: ctx=" << ctx << "dev=" << dev;
}

QBlittable *EtnaBlittablePlatformPixmap::createBlittable(const QSize &size,
                                                          bool /*alpha*/) const
{
    if (!s_ctx || !s_dev) {
        ETNA_WARN << "createBlittable called before setGlobalContext — returning nullptr";
        return nullptr;
    }
    etna2d_surface *surf = nullptr;
    int rc = etna2d_surface_create(s_dev, size.width(), size.height(), &surf);
    if (rc != ETNA2D_OK || !surf) {
        ETNA_WARN << "etna2d_surface_create failed rc=" << rc
                  << "size=" << size;
        return nullptr;
    }
    ETNA_DBG << "createBlittable" << size << "surf=" << surf;
    return new EtnaBlittable(s_ctx, surf);
}

/* -----------------------------------------------------------------------
 * EtnaBlittable — construction / destruction
 * ----------------------------------------------------------------------- */
EtnaBlittable::EtnaBlittable(etna2d_context *ctx, etna2d_surface *surf)
    : QBlittable(QSize(etna2d_surface_width(surf), etna2d_surface_height(surf)),
                 Capabilities(SolidRectCapability
                              | AlphaFillRectCapability
                              | SourcePixmapCapability
                              | SourceOverPixmapCapability
                              | SourceOverScaledPixmapCapability
                              | OpacityPixmapCapability))
    , m_ctx(ctx)
    , m_surf(surf)
{
    ETNA_DBG << "EtnaBlittable created:"
             << etna2d_surface_width(surf) << "x" << etna2d_surface_height(surf)
             << "surf=" << surf;
}

EtnaBlittable::~EtnaBlittable()
{
    ETNA_DBG << "EtnaBlittable destroyed surf=" << m_surf;
    unlock(); /* ensure CPU map released before surface destroy */
    etna2d_surface_destroy(m_surf);
    m_surf = nullptr;
}

/* -----------------------------------------------------------------------
 * unlockForGpuRead — CPU→GPU barrier without a GPU fence stall.
 *
 * When a pixmap is used as a blit SOURCE the GPU only reads it; we don't
 * need to wait for any GPU fence.  We only need to flush the WC write
 * buffer (cpu_fini) so the GPU sees any CPU writes.
 *
 * We set m_skipFinishOnUnlock so that the doUnlock() called by the base
 * QBlittable::unlock() skips etna2d_finish().  This avoids one ~2.5–3µs
 * GPU pipeline-drain stall per source sprite per frame.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::unlockForGpuRead()
{
    if (!isLocked())
        return;
    m_skipFinishOnUnlock = true;
    unlock(); /* → doUnlock() which checks m_skipFinishOnUnlock */
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

etna2d_surface *EtnaBlittable::surfaceFromPixmap(const QPixmap &pixmap)
{
    QPlatformPixmap *ppm = pixmap.handle();
    if (!ppm || ppm->classId() != QPlatformPixmap::BlitterClass)
        return nullptr;
    auto *bpm = static_cast<EtnaBlittablePlatformPixmap *>(ppm);
    EtnaBlittable *blitter = bpm->etnaBlittable();
    if (!blitter)
        return nullptr;
    /* Ensure the source blitter is not CPU-locked; its GPU ops must be
     * visible before we feed it into etna2d_blit/stretch.
     * Use unlockForGpuRead() — cpu_fini only, no etna2d_finish stall.
     * The GPU will see the barrier naturally through command ordering:
     * the source surface's last GPU write is already fenced inside the
     * same context's command stream, so no cross-context sync is needed.
     *
     * QT_ETNAVIV2D_NO_BATCH=1 falls back to the conservative full unlock
     * (cpu_fini + etna2d_finish) for debugging or single-context comparison. */
    if (noBatch())
        blitter->unlock();
    else
        blitter->unlockForGpuRead();
    return blitter->m_surf;
}

/* -----------------------------------------------------------------------
 * fillRect — SolidRectCapability
 * ----------------------------------------------------------------------- */
void EtnaBlittable::fillRect(const QRectF &rect, const QColor &color)
{
    /* CompositionMode_Source: write opaque color, ignore destination alpha. */
    QRect r = rect.toRect();
    if (r.isEmpty())
        return;
    /* For a source-mode fill use straight alpha (overwrite dest). */
    uint32_t argb = (uint32_t(color.alpha()) << 24)
                  | (uint32_t(color.red())   << 16)
                  | (uint32_t(color.green())  <<  8)
                  | uint32_t(color.blue());
    ETNA_DBG << "fillRect" << r << Qt::hex << argb;
    etna2d_fill(m_ctx, m_surf, r.x(), r.y(), r.width(), r.height(), argb);
    /* Note: NOT flushed here — deferred to doUnlock()/backingstore flush(). */
}

/* -----------------------------------------------------------------------
 * alphaFillRect — AlphaFillRectCapability
 * ----------------------------------------------------------------------- */
void EtnaBlittable::alphaFillRect(const QRectF &rect, const QColor &color,
                                   QPainter::CompositionMode cmode)
{
    QRect r = rect.toRect();
    if (r.isEmpty())
        return;

    if (cmode == QPainter::CompositionMode_Source || color.alpha() == 255) {
        /* Opaque source fill — same as fillRect. */
        fillRect(rect, color);
        return;
    }

    if (cmode == QPainter::CompositionMode_SourceOver && color.alpha() == 0)
        return; /* fully transparent: no-op */

    if (cmode != QPainter::CompositionMode_SourceOver) {
        /* Unsupported composition mode — fall back to raster. */
        ETNA_DBG << "alphaFillRect: unsupported cmode" << cmode << "— raster fallback";
        QBlittable::alphaFillRect(rect, color, cmode);
        return;
    }

    /* SourceOver with translucent color: libetna2d has no direct
     * "fill with SRC_OVER" operation; the raster fallback handles it
     * correctly via doLock(). Translucent fills are rare in typical
     * widget UIs (most fills are opaque), so this is not a hot path. */
    ETNA_DBG << "alphaFillRect SourceOver translucent: raster fallback";
    QBlittable::alphaFillRect(rect, color, cmode);
}

/* -----------------------------------------------------------------------
 * drawPixmap — SourcePixmapCapability / SourceOverScaledPixmapCapability
 * Qt calls this with CompositionMode_SourceOver semantics for blitting.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::drawPixmap(const QRectF &rect, const QPixmap &pixmap,
                                const QRectF &subrect)
{
    drawPixmapOpacity(rect, pixmap, subrect,
                      QPainter::CompositionMode_SourceOver, 1.0);
}

/* -----------------------------------------------------------------------
 * drawPixmapOpacity — SourceOverPixmapCapability / OpacityPixmapCapability
 * ----------------------------------------------------------------------- */
void EtnaBlittable::drawPixmapOpacity(const QRectF &rect, const QPixmap &pixmap,
                                       const QRectF &subrect,
                                       QPainter::CompositionMode cmode,
                                       qreal opacity)
{
    QRect dstR = rect.toRect();
    QRect srcR = subrect.toRect();

    if (dstR.isEmpty() || srcR.isEmpty())
        return;

    /* Round small rects up to at least 1 pixel (mirrors directfb logic). */
    if (srcR.width()  < 1) srcR.setWidth(1);
    if (srcR.height() < 1) srcR.setHeight(1);

    etna2d_surface *srcSurf = surfaceFromPixmap(pixmap);
    if (!srcSurf) {
        /* Source is a raster pixmap — let Qt fall back to raster engine. */
        ETNA_DBG << "drawPixmapOpacity: source is raster — fallback";
        return;
    }

    /* GC520 PE only supports SRC and SRC_OVER.  For anything else, fall back. */
    if (cmode != QPainter::CompositionMode_SourceOver &&
        cmode != QPainter::CompositionMode_Source) {
        ETNA_DBG << "drawPixmapOpacity: unsupported cmode" << cmode << "— fallback";
        return;
    }

    bool isStretch = (dstR.width()  != srcR.width() ||
                      dstR.height() != srcR.height());
    bool isOver    = (cmode == QPainter::CompositionMode_SourceOver);
    bool hasOpacity = (opacity < 0.9999);

    ETNA_DBG << "drawPixmapOpacity"
             << "src" << srcR << "dst" << dstR
             << "over=" << isOver
             << "stretch=" << isStretch
             << "opacity=" << opacity;

    if (hasOpacity) {
        /* Global opacity: premultiply source alpha by opacity.
         * libetna2d has no direct opacity arg, so we'd need a temporary
         * surface.  For v1 fall back to raster — opacity blits are rare. */
        ETNA_DBG << "drawPixmapOpacity: opacity < 1.0 — raster fallback";
        return;
    }

    if (!isStretch) {
        /* Pixel-for-pixel blit. */
        enum etna2d_blend blend = isOver ? ETNA2D_BLEND_SRC_OVER
                                         : ETNA2D_BLEND_NONE;
        etna2d_blit(m_ctx,
                    srcSurf, srcR.x(), srcR.y(),
                    m_surf,  dstR.x(), dstR.y(),
                    srcR.width(), srcR.height(),
                    blend, ETNA2D_ROP_COPY);
    } else {
        /* Stretch blit.
         * etna2d_stretch() always scales the ENTIRE source surface into the
         * destination rect (it has no source sub-rect param).
         *
         * If Qt is asking us to stretch a sub-rect of the source (srcR !=
         * full source), we fall back to the CPU raster engine — it's an
         * uncommon case (drawPixmap with non-trivial srcRect + scale).
         * The common widget case (drawPixmap(dstRect, pm, pm.rect())) uses
         * the full source so this fast path covers it. */
        int sw = etna2d_surface_width(srcSurf);
        int sh = etna2d_surface_height(srcSurf);
        bool fullSrc = (srcR.x() == 0 && srcR.y() == 0 &&
                        srcR.width() == sw && srcR.height() == sh);
        if (fullSrc) {
            etna2d_stretch(m_ctx, srcSurf, m_surf,
                           dstR.x(), dstR.y(), dstR.width(), dstR.height());
        } else {
            ETNA_DBG << "drawPixmapOpacity: stretch with sub-srcRect — raster fallback";
            /* Returning without doing anything triggers Qt's raster fallback
             * via doLock() automatically — no explicit call needed here. */
            return;
        }
    }
    /* Deferred flush — see class-level design note. */
}

/* -----------------------------------------------------------------------
 * doLock — expose BO memory as a QImage for the CPU raster fallback.
 * Called by Qt's QBlittablePlatformPixmap::buffer() and
 * QBlitterPaintEngine::begin() when a CPU op is needed.
 * ----------------------------------------------------------------------- */
QImage *EtnaBlittable::doLock()
{
    /* Finish any pending GPU work so CPU reads see up-to-date pixels. */
    etna2d_finish(m_ctx);

    void *ptr = etna2d_surface_map(m_surf);
    if (!ptr) {
        ETNA_WARN << "doLock: etna2d_surface_map failed";
        return &m_lockImage; /* return empty — Qt handles null/empty image */
    }

    int w      = etna2d_surface_width(m_surf);
    int h      = etna2d_surface_height(m_surf);
    int stride = static_cast<int>(etna2d_surface_stride(m_surf));

    ETNA_DBG << "doLock" << w << "x" << h << "stride=" << stride;

    /* Format_ARGB32_Premultiplied matches the GC520 premult blend model.
     * Stride must be the real BO stride (NOT w*4 — the DE may pad rows). */
    m_lockImage = QImage(static_cast<uchar *>(ptr), w, h, stride,
                         QImage::Format_ARGB32_Premultiplied);
    return &m_lockImage;
}

/* -----------------------------------------------------------------------
 * doUnlock — release CPU map and flush GPU work.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::doUnlock()
{
    ETNA_DBG << "doUnlock —"
             << (m_skipFinishOnUnlock ? "cpu_fini only (read-only src)"
                                      : "cpu_fini + finish");
    /* WC drain barrier: ensures CPU writes are visible before GPU reads. */
    etna2d_surface_cpu_fini(m_surf);
    if (!m_skipFinishOnUnlock) {
        /* Full flush: wait for all queued GPU ops (used for dst surfaces
         * and at frame boundary). */
        etna2d_finish(m_ctx);
    }
    m_skipFinishOnUnlock = false; /* reset for next lock cycle */
    m_lockImage = QImage(); /* release the dangling pointer */
}

QT_END_NAMESPACE
