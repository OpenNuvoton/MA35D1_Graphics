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

/* Scratch surface for source-over stretches (see drawPixmapOpacity).  etna2d_stretch()
 * is a point-sampled copy with no blending, so a translucent source is stretched into
 * this surface first and then blended onto the destination.  Grown on demand, shared
 * by all blittables (single GUI thread), never shrunk. */
static etna2d_surface *s_scratch = nullptr;
static int s_scratchW = 0, s_scratchH = 0;
static const qint64 kScratchMaxPixels = 4 * 1024 * 1024;

static etna2d_surface *scratchSurface(etna2d_context *ctx, etna2d_device *dev, int w, int h)
{
    if (s_scratch && s_scratchW >= w && s_scratchH >= h)
        return s_scratch;
    if (qint64(w) * h > kScratchMaxPixels)
        return nullptr;
    const int nw = qMax(w, s_scratchW), nh = qMax(h, s_scratchH);
    etna2d_surface *ns = nullptr;
    if (etna2d_surface_create(dev, nw, nh, &ns) != ETNA2D_OK || !ns)
        return nullptr;
    if (s_scratch) {
        etna2d_finish(ctx);               /* queued ops may still use the old one */
        etna2d_surface_destroy(s_scratch);
    }
    s_scratch = ns; s_scratchW = nw; s_scratchH = nh;
    return s_scratch;
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
static const qint64 kOpaqueScanMaxPixels = 4 * 1024 * 1024;

bool EtnaBlittable::isOpaque()
{
    if (m_opaqueKnown)
        return m_opaque;
    const int w = etna2d_surface_width(m_surf), h = etna2d_surface_height(m_surf);
    bool opaque = false;
    if (qint64(w) * h <= kOpaqueScanMaxPixels) {
        QImage *img = lock();               /* finishes pending GPU writes, maps for CPU */
        if (img && !img->isNull()) {
            opaque = true;
            for (int y = 0; y < h && opaque; ++y) {
                const quint32 *px = reinterpret_cast<const quint32 *>(img->constScanLine(y));
                for (int x = 0; x < w; ++x)
                    if ((px[x] >> 24) != 0xff) { opaque = false; break; }
            }
        }
        unlockForGpuRead();
    }
    m_opaque = opaque;
    m_opaqueKnown = true;                  /* after lock(): doLock() clears the flag */
    return m_opaque;
}

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
    m_opaqueKnown = false;
    ETNA_DBG << "fillRect" << r << Qt::hex << argb;
    etna2d_fill(m_ctx, m_surf, r.x(), r.y(), r.width(), r.height(), argb);
    /* Note: NOT flushed here — deferred to doUnlock()/backingstore flush(). */
}

/* -----------------------------------------------------------------------
 * CPU fallbacks.
 *
 * Qt's QBlitterPaintEngine decides GPU-or-raster BEFORE it calls a blittable
 * method (from the declared Capabilities and the painter state).  Once one of
 * our methods has been called Qt assumes the result is drawn, so a bare
 * "return" or QBlittable::alphaFillRect() (a stub that only warns) leaves the
 * pixels untouched.  Every case we cannot do on the GPU must therefore draw
 * itself, through the CPU raster engine on the locked BO memory.
 * Rects arrive already mapped to device space and clipped by Qt.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::cpuFillRect(const QRectF &rect, const QColor &color,
                                QPainter::CompositionMode cmode)
{
    QImage *dst = lock();
    if (!dst || dst->isNull())
        return;
    QPainter p(dst);
    p.setCompositionMode(cmode);
    p.fillRect(rect, color);
}

void EtnaBlittable::cpuDrawPixmap(const QRectF &rect, const QPixmap &pixmap,
                                  const QRectF &subrect,
                                  QPainter::CompositionMode cmode, qreal opacity)
{
    /* Read the source first (maps it for the CPU), then lock the destination. */
    const QImage src = pixmap.toImage();
    QImage *dst = lock();
    if (!dst || dst->isNull() || src.isNull())
        return;
    QPainter p(dst);
    p.setCompositionMode(cmode);
    p.setOpacity(opacity);
    p.drawImage(rect, src, subrect);
}

/* -----------------------------------------------------------------------
 * alphaFillRect — only reached if AlphaFillRectCapability is declared.
 * It is NOT declared (the GC520 has no blended fill), so Qt draws translucent
 * fills itself.  Kept as a safe implementation in case it is ever called.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::alphaFillRect(const QRectF &rect, const QColor &color,
                                   QPainter::CompositionMode cmode)
{
    QRect r = rect.toRect();
    if (r.isEmpty())
        return;
    if (cmode == QPainter::CompositionMode_Source || color.alpha() == 255) {
        fillRect(rect, color);
        return;
    }
    cpuFillRect(rect, color, cmode);
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

    m_opaqueKnown = false;   /* this surface is about to be written */
    etna2d_surface *srcSurf = surfaceFromPixmap(pixmap);
    if (!srcSurf) {
        ETNA_DBG << "drawPixmapOpacity: source is raster — CPU draw";
        cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
        return;
    }

    /* GC520 PE only supports SRC and SRC_OVER.  For anything else, fall back. */
    if (cmode != QPainter::CompositionMode_SourceOver &&
        cmode != QPainter::CompositionMode_Source) {
        ETNA_DBG << "drawPixmapOpacity: unsupported cmode" << cmode << "— CPU draw";
        cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
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
        /* libetna2d has no global-opacity blend. */
        ETNA_DBG << "drawPixmapOpacity: opacity < 1.0 — CPU draw";
        cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
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
         * full source), we draw it with the CPU raster engine — it's an
         * uncommon case (drawPixmap with non-trivial srcRect + scale).
         * The common widget case (drawPixmap(dstRect, pm, pm.rect())) uses
         * the full source so this fast path covers it.
         * etna2d_stretch() copies without blending, so source-over goes
         * through a scratch surface (see scratchSurface()). */
        int sw = etna2d_surface_width(srcSurf);
        int sh = etna2d_surface_height(srcSurf);
        bool fullSrc = (srcR.x() == 0 && srcR.y() == 0 &&
                        srcR.width() == sw && srcR.height() == sh);
        if (fullSrc) {
            EtnaBlittable *srcBl = static_cast<EtnaBlittablePlatformPixmap *>(pixmap.handle())->etnaBlittable();
            if (!isOver || srcBl->isOpaque()) {
                /* No alpha to blend: plain stretch (copy) is exact. */
                etna2d_stretch(m_ctx, srcSurf, m_surf,
                               dstR.x(), dstR.y(), dstR.width(), dstR.height());
            } else {
                /* Source-over: stretch alone would overwrite the destination
                 * (no alpha).  Stretch into the scratch surface, then blend. */
                etna2d_surface *tmp = nullptr;
                const QRect surfR(0, 0, etna2d_surface_width(m_surf), etna2d_surface_height(m_surf));
                const QRect vis = dstR.intersected(surfR);
                if (vis.isEmpty())
                    return;
                if (dstR.width() > 0 && dstR.height() > 0)
                    tmp = scratchSurface(m_ctx, EtnaBlittablePlatformPixmap::device(), dstR.width(), dstR.height());
                if (!tmp) {
                    ETNA_DBG << "drawPixmapOpacity: no scratch surface — CPU draw";
                    cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
                    return;
                }
                etna2d_stretch(m_ctx, srcSurf, tmp, 0, 0, dstR.width(), dstR.height());
                etna2d_blit(m_ctx,
                            tmp, vis.x() - dstR.x(), vis.y() - dstR.y(),
                            m_surf, vis.x(), vis.y(),
                            vis.width(), vis.height(),
                            ETNA2D_BLEND_SRC_OVER, ETNA2D_ROP_COPY);
            }
        } else {
            ETNA_DBG << "drawPixmapOpacity: stretch with sub-srcRect — CPU draw";
            cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
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
    m_opaqueKnown = false;   /* CPU may write */
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
