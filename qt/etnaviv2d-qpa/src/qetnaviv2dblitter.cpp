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
#include "qetnaviv2dformat.h"
#include "qetnaviv2dconfig.h"
#include "qetnaviv2dstats.h"

#include <QDebug>
#include <QMutexLocker>
#include <QtGui/private/qpixmap_blitter_p.h>
#include <qpa/qplatformpixmap.h>
#include <QtGui/private/qtextureglyphcache_p.h>
#include <QtGui/private/qpaintengine_raster_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qtextengine_p.h>
#include <QtGui/private/qstatictext_p.h>

QT_BEGIN_NAMESPACE

/* -----------------------------------------------------------------------
 * Context mutex
 * ----------------------------------------------------------------------- */
/* One GC520 context/queue is shared by every blittable, so GPU calls from several threads
 * (QPainter on a worker-thread pixmap, QT_THREADED_PAINTING) must be serialised.  Recursive:
 * lock() -> doLock() -> etna2d_finish() and drawing helpers call each other. */
QRecursiveMutex &etnaCtxMutex() { static QRecursiveMutex m; return m; }

#define ETNA_DBG  ETNA_LOG_DBG("blitter")
#define ETNA_WARN ETNA_LOG_WARN("blitter")

/* -----------------------------------------------------------------------
 * EtnaBlittablePlatformPixmap — statics
 * ----------------------------------------------------------------------- */
etna2d_context *EtnaBlittablePlatformPixmap::s_ctx = nullptr;
etna2d_device  *EtnaBlittablePlatformPixmap::s_dev = nullptr;

EtnaBlittablePlatformPixmap::EtnaBlittablePlatformPixmap()
    : QBlittablePlatformPixmap()
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
                                                          bool alpha) const
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
    if (!s_ctx || !s_dev) {
        ETNA_WARN << "createBlittable called before setGlobalContext — returning nullptr";
        return nullptr;
    }
    etna2d_surface *surf = nullptr;
    /* Surfaces the CPU rasterises into are cacheable; QT_ETNA2D_CACHED=1 opts in (default write-combine). */
    const bool cached = etnaConfig().cached;
    int rc = etna2d_surface_create_ex(s_dev, size.width(), size.height(),
                                      etnaStorageFmt(alpha).etna,
                                      cached ? ETNA2D_SURF_CACHED : 0, &surf);
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
                              | OpacityPixmapCapability
                              | (etnaConfig().gpuText ? DrawScaledCachedGlyphsCapability : Capability(0))))
    , m_ctx(ctx)
    , m_surf(surf)
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
    ETNA_DBG << "EtnaBlittable created:"
             << etna2d_surface_width(surf) << "x" << etna2d_surface_height(surf)
             << "surf=" << surf;
}

EtnaBlittable::~EtnaBlittable()
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
    if (m_opaqueKnown)
        return m_opaque;
    const int w = etna2d_surface_width(m_surf), h = etna2d_surface_height(m_surf);
    bool opaque = false;
    if (!etnaFmtByEtna(etna2d_surface_format(m_surf)).alpha) {
        m_opaque = true; m_opaqueKnown = true;    /* no alpha channel: always opaque */
        return true;
    }
    const bool px16 = etnaFmtByEtna(etna2d_surface_format(m_surf)).bpp == 16;
    if (qint64(w) * h <= kOpaqueScanMaxPixels) {
        EtnaStats::Scope _t(EtnaStats::OpaqueScan);
        QImage *img = lock();               /* finishes pending GPU writes, maps for CPU */
        if (img && !img->isNull()) {
            opaque = true;
            for (int y = 0; y < h && opaque; ++y) {
                if (px16) {                       /* ARGB4444: alpha = top nibble */
                    const quint16 *px = reinterpret_cast<const quint16 *>(img->constScanLine(y));
                    for (int x = 0; x < w; ++x)
                        if ((px[x] >> 12) != 0xf) { opaque = false; break; }
                } else {
                    const quint32 *px = reinterpret_cast<const quint32 *>(img->constScanLine(y));
                    for (int x = 0; x < w; ++x)
                        if ((px[x] >> 24) != 0xff) { opaque = false; break; }
                }
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
    if (etnaConfig().noBatch)
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
    EtnaStats::add(EtnaStats::GpuFill);
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
    EtnaStats::Scope _t(EtnaStats::CpuFill);
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
    /* Read the source first (maps it for the CPU), then lock the destination. */
    EtnaStats::Scope _t(EtnaStats::CpuPixmap);
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
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
        /* Global-opacity blit (probed on the GC520): source-over of a premultiplied source
         * scaled by the painter opacity.  Scaled and non-source-over variants stay on the CPU. */
        if (!isStretch && isOver) {
            EtnaStats::add(EtnaStats::GpuBlit);
            etna2d_blit_opacity(m_ctx, srcSurf, srcR.x(), srcR.y(), m_surf, dstR.x(), dstR.y(),
                                srcR.width(), srcR.height(), unsigned(qBound(0, qRound(opacity * 255), 255)));
            return;
        }
        ETNA_DBG << "drawPixmapOpacity: opacity < 1.0 — CPU draw";
        cpuDrawPixmap(rect, pixmap, subrect, cmode, opacity);
        return;
    }

    if (!isStretch) {
        /* Pixel-for-pixel blit. */
        enum etna2d_blend blend = isOver ? ETNA2D_BLEND_SRC_OVER
                                         : ETNA2D_BLEND_NONE;
        EtnaStats::add(EtnaStats::GpuBlit);
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
                EtnaStats::add(EtnaStats::GpuStretch);
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
                EtnaStats::add(EtnaStats::GpuStretch);
                EtnaStats::add(EtnaStats::GpuBlit);
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
    QMutexLocker _etnaLock(&etnaCtxMutex());
    m_opaqueKnown = false;   /* CPU may write */
    /* Finish any pending GPU work so CPU reads see up-to-date pixels. */
    {
        EtnaStats::Scope _t(EtnaStats::LockFinish);
        if (etna2d_surface_is_cached(m_surf)) {
            /* Submit queued ops, wait only for GPU work on this surface, invalidate the CPU cache. */
            if (etna2d_surface_cpu_begin(m_ctx, m_surf) != ETNA2D_OK)
                etna2d_finish(m_ctx);
        } else {
            etna2d_finish(m_ctx);
        }
    }

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
    QImage::Format qf = etnaFmtByEtna(etna2d_surface_format(m_surf)).qfmt;
    if (qf == QImage::Format_Invalid) qf = QImage::Format_ARGB32_Premultiplied;
    m_lockImage = QImage(static_cast<uchar *>(ptr), w, h, stride, qf);
    return &m_lockImage;
}

/* -----------------------------------------------------------------------
 * doUnlock — release CPU map and flush GPU work.
 * ----------------------------------------------------------------------- */
void EtnaBlittable::doUnlock()
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
    ETNA_DBG << "doUnlock —"
             << (m_skipFinishOnUnlock ? "cpu_fini only (read-only src)"
                                      : "cpu_fini + finish");
    /* WC drain barrier: ensures CPU writes are visible before GPU reads. */
    etna2d_surface_cpu_fini(m_surf);
    /* A cached surface needs no finish here: cpu_fini already cleaned the CPU cache
     * synchronously and the next GPU submit is ordered after it. */
    if (!m_skipFinishOnUnlock && !etna2d_surface_is_cached(m_surf)) {
        /* Full flush: wait for all queued GPU ops (used for dst surfaces
         * and at frame boundary). */
        EtnaStats::Scope _t(EtnaStats::UnlockFinish);
        etna2d_finish(m_ctx);
    }
    m_skipFinishOnUnlock = false; /* reset for next lock cycle */
    m_lockImage = QImage(); /* release the dangling pointer */
}


/* -----------------------------------------------------------------------
 * Text on the GPU.
 *
 * QImageTextureGlyphCache keeps the rasterised glyphs in a QImage atlas (Format_Alpha8 for
 * antialiased text).  We subclass it only to mirror that atlas into an A8 GPU surface (dirty
 * rectangles are uploaded when new glyphs were added) and then draw every glyph with
 * etna2d_mask_blit(): pen colour (straight ARGB) blended through the coverage mask.
 * Same structure as src/plugins/platforms/directfb/qdirectfbblitter.cpp.
 * Not handled (Qt still routes them to the raster engine, or they are ignored here): glyph formats
 * other than A8 (mono bitmap fonts: drawn as nothing), composition modes other than SourceOver,
 * gradient pens.  Complex clips are routed to the raster engine by Qt (no ComplexClipCapability).
 * ----------------------------------------------------------------------- */
namespace {
const char s_glyphCacheKey = 0;   /* address = key for QFontEngine::glyphCache() */

class EtnaGlyphCache : public QImageTextureGlyphCache
{
public:
    EtnaGlyphCache(QFontEngine::GlyphFormat f, const QTransform &m) : QImageTextureGlyphCache(f, m) {}
    ~EtnaGlyphCache() override { dropSurface(); }

    void createTextureData(int w, int h) override
    {
        dropSurface();
        QImageTextureGlyphCache::createTextureData(w, h);
        m_dirty = QRect(0, 0, w, h);
    }
    void resizeTextureData(int w, int h) override
    {
        dropSurface();
        QImageTextureGlyphCache::resizeTextureData(w, h);
        m_dirty = QRect(0, 0, w, h);
    }
    void fillTexture(const Coord &c, glyph_t g, QFixed sp) override
    {
        QImageTextureGlyphCache::fillTexture(c, g, sp);
        m_dirty |= QRect(c.x, c.y, c.w, c.h);
    }

    /* The A8 surface holding the atlas, up to date; nullptr if not usable. */
    etna2d_surface *sync(etna2d_context *ctx, etna2d_device *dev)
    {
        const QImage &im = image();
        if (im.isNull() || im.format() != QImage::Format_Alpha8)
            return nullptr;
        if (!m_surf) {
            if (etna2d_surface_create_fmt(dev, im.width(), im.height(), ETNA2D_FMT_A8, &m_surf) != ETNA2D_OK)
                { m_surf = nullptr; return nullptr; }
            m_dirty = QRect(0, 0, im.width(), im.height());
        }
        if (m_dirty.isEmpty())
            return m_surf;
        m_dirty &= QRect(0, 0, im.width(), im.height());
        /* Queued glyph blits may still read the old atlas content: cpu_begin submits them and waits
         * for the GPU work on this surface only. */
        if (etna2d_surface_cpu_begin(ctx, m_surf) != ETNA2D_OK)
            return nullptr;
        uchar *base = static_cast<uchar *>(etna2d_surface_map(m_surf));
        const uint stride = etna2d_surface_stride(m_surf);
        for (int y = m_dirty.top(); y <= m_dirty.bottom(); ++y)
            memcpy(base + y * stride + m_dirty.left(), im.constScanLine(y) + m_dirty.left(), m_dirty.width());
        etna2d_surface_cpu_fini(m_surf);
        m_dirty = QRect();
        return m_surf;
    }

private:
    void dropSurface()
    {
        if (!m_surf)
            return;
        etna2d_context *ctx = EtnaBlittablePlatformPixmap::context();
        if (ctx) {                            /* null after the integration was torn down: the device is gone */
            etna2d_finish(ctx);               /* queued blits may still use it */
            etna2d_surface_destroy(m_surf);
        }
        m_surf = nullptr;
    }
    etna2d_surface *m_surf = nullptr;
    QRect m_dirty;
};
} // namespace

bool EtnaBlittable::drawCachedGlyphs(const QPaintEngineState *state, QFontEngine::GlyphFormat glyphFormat,
                                     int numGlyphs, const glyph_t *glyphs, const QFixedPoint *positions,
                                     QFontEngine *fontEngine)
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
    etna2d_device *dev = EtnaBlittablePlatformPixmap::device();
    if (!dev)
        return false;
    unlock();     /* CPU raster writes (if any) must be released before the GPU reads/writes the surface */
    void *key = const_cast<char *>(&s_glyphCacheKey);
    auto *cache = static_cast<EtnaGlyphCache *>(fontEngine->glyphCache(key, glyphFormat, state->transform()));
    if (!cache) {
        cache = new EtnaGlyphCache(glyphFormat, state->transform());
        fontEngine->setGlyphCache(key, cache);
    }
    cache->populate(fontEngine, numGlyphs, glyphs, positions);
    cache->fillInPendingGlyphs();
    etna2d_surface *atlas = cache->sync(m_ctx, dev);
    if (!atlas) {
        ETNA_WARN << "glyph atlas not usable (format" << int(glyphFormat) << ") — text skipped";
        return false;
    }

    const QColor pen = state->pen().color();
    const int alpha = qBound(0, qRound(pen.alpha() * state->opacity()), 255);
    if (alpha == 0)
        return true;
    const uint32_t argb = (uint32_t(alpha) << 24) | (uint32_t(pen.red()) << 16)
                        | (uint32_t(pen.green()) << 8) | uint32_t(pen.blue());

    QRect clipR(0, 0, etna2d_surface_width(m_surf), etna2d_surface_height(m_surf));
    const QRasterPaintEngineState *rs = static_cast<const QRasterPaintEngineState *>(state);
    if (rs->clip && rs->clip->enabled)
        clipR &= rs->clip->clipRect;
    if (clipR.isEmpty())
        return true;

    const int margin = fontEngine->glyphMargin(glyphFormat);
    m_opaqueKnown = false;

    /* The mask blit is a source-over blend.  Other composition modes (rare) are drawn by the CPU
     * raster engine from the same atlas: one premultiplied coloured image per glyph. */
    const QPainter::CompositionMode cmode = state->compositionMode();
    QImage *cpuDst = nullptr;
    QScopedPointer<QPainter> cpuPainter;
    if (cmode != QPainter::CompositionMode_SourceOver) {
        cpuDst = lock();
        if (!cpuDst || cpuDst->isNull())
            return false;
        cpuPainter.reset(new QPainter(cpuDst));
        cpuPainter->setCompositionMode(cmode);
        cpuPainter->setClipRect(clipR);
    }

    QVarLengthArray<etna2d_mask_rect, 128> rects;
    for (int i = 0; i < numGlyphs; ++i) {
        const QFixed sub = fontEngine->subPixelPositionForX(positions[i].x);
        const QTextureGlyphCache::GlyphAndSubPixelPosition g(glyphs[i], sub);
        const QTextureGlyphCache::Coord &c = cache->coords[g];
        if (c.isNull())
            continue;
        const int x = qFloor(positions[i].x) + c.baseLineX - margin;
        const int y = qRound(positions[i].y) - c.baseLineY - margin;
        const QRect r = QRect(x, y, c.w, c.h) & clipR;
        if (r.isEmpty())
            continue;
        if (cpuPainter) {
            const QImage &atlasImg = cache->image();
            QImage tmp(c.w, c.h, QImage::Format_ARGB32_Premultiplied);
            for (int yy = 0; yy < c.h; ++yy) {
                const uchar *m = atlasImg.constScanLine(c.y + yy) + c.x;
                QRgb *d = reinterpret_cast<QRgb *>(tmp.scanLine(yy));
                for (int xx = 0; xx < c.w; ++xx) {
                    const int a = (m[xx] * alpha + 127) / 255;
                    d[xx] = qPremultiply(qRgba(pen.red(), pen.green(), pen.blue(), a));
                }
            }
            cpuPainter->drawImage(x, y, tmp);
            continue;
        }
        rects.append({ c.x + (r.x() - x), c.y + (r.y() - y), r.x(), r.y(), r.width(), r.height() });
    }
    if (!rects.isEmpty()) {
        EtnaStats::add(EtnaStats::GpuMask, 0, rects.size());
        etna2d_mask_blit_n(m_ctx, atlas, m_surf, argb, rects.constData(), int(rects.size()));
    }
    return true;
}


/* -----------------------------------------------------------------------
 * EtnaPaintEngine
 * ----------------------------------------------------------------------- */
QPaintEngine *EtnaBlittablePlatformPixmap::paintEngine() const
{
    if (!m_engine) {
        auto *that = const_cast<EtnaBlittablePlatformPixmap *>(this);
        that->m_engine.reset(new EtnaPaintEngine(that));
    }
    return m_engine.data();
}

bool EtnaPaintEngine::end()
{
    QMutexLocker _etnaLock(&etnaCtxMutex());
    const bool ok = QBlitterPaintEngine::end();
    /* Queued ops otherwise sit in the command stream until something needs them (next lock of a
     * surface they touch, frame flush).  Submitting here is asynchronous (no wait) and lets the GPU
     * work overlap the CPU work that follows.  A no-op when nothing is queued. */
    etna2d_flush(EtnaBlittablePlatformPixmap::context());
    return ok;
}

bool EtnaPaintEngine::gpuGlyphs(int numGlyphs, const glyph_t *glyphs, const QFixedPoint *positions,
                                QFontEngine *fe)
{
    if (!etnaConfig().gpuText)
        return false;
    QRasterPaintEngineState *s = state();
    if (!s || s->matrix.type() > QTransform::TxScale)
        return false;
    if (s->clip && s->clip->enabled && !s->clip->hasRectClip)
        return false;                                   /* complex clip: raster engine */
    auto *rd = static_cast<QRasterPaintEnginePrivate *>(d_ptr.data());
    const QFontEngine::GlyphFormat fmt = fe->glyphFormat != QFontEngine::Format_None ? fe->glyphFormat
                                                                                      : rd->glyphCacheFormat;
    if (fmt != QFontEngine::Format_A8)
        return false;
    auto *bl = static_cast<EtnaBlittable *>(m_pm->blittable());
    return bl && bl->drawCachedGlyphs(s, fmt, numGlyphs, glyphs, positions, fe);
}

void EtnaPaintEngine::drawTextItem(const QPointF &p, const QTextItem &textItem)
{
    const QTextItemInt &ti = static_cast<const QTextItemInt &>(textItem);
    if (ti.glyphs.numGlyphs == 0)
        return;
    QTransform matrix = state()->matrix;
    if (shouldDrawCachedGlyphs(ti.fontEngine, matrix)) {
        QVarLengthArray<QFixedPoint> positions;
        QVarLengthArray<glyph_t> glyphs;
        matrix.translate(p.x(), p.y());
        ti.fontEngine->getGlyphPositions(ti.glyphs, matrix, ti.flags, glyphs, positions);
        if (gpuGlyphs(glyphs.size(), glyphs.constData(), positions.constData(), ti.fontEngine))
            return;
    }
    QBlitterPaintEngine::drawTextItem(p, textItem);
}

void EtnaPaintEngine::drawStaticTextItem(QStaticTextItem *item)
{
    if (item->numGlyphs == 0)
        return;
    if (shouldDrawCachedGlyphs(item->fontEngine(), state()->matrix)
        && gpuGlyphs(item->numGlyphs, item->glyphs, item->glyphPositions, item->fontEngine()))
        return;
    QBlitterPaintEngine::drawStaticTextItem(item);
}

QT_END_NAMESPACE
