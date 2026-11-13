/*
 * qetnaviv2dblitter.h — EtnaBlittable: QBlittable HW hook over libetna2d.
 *
 * Qt's QBlitterPaintEngine calls into this class for all accelerated 2D ops.
 * Any op not covered by our declared Capabilities falls back to Qt's
 * QRasterPaintEngine which software-renders directly into the lock()-ed BO
 * memory — so a CPU raster fallback is automatic and free.
 *
 * Surface format is always ARGB32_Premultiplied, matching the GC520 PE blend
 * model (premultiplied source-over).  The BO stride is the real hardware
 * stride from etna2d_surface_stride(), NOT w*4.
 *
 * Reference: src/plugins/platforms/directfb/qdirectfbblitter.h
 */
#ifndef QETNAVIV2DBLITTER_H
#define QETNAVIV2DBLITTER_H

#include <QtGui/private/qblittable_p.h>
#include <QtGui/private/qpixmap_blitter_p.h>
#include <QtGui/private/qpaintengine_blitter_p.h>

extern "C" {
#include "etna2d.h"
}

QT_BEGIN_NAMESPACE

/* -----------------------------------------------------------------------
 * EtnaBlittable
 * One instance per GPU surface (offscreen pixmap or backing-store buffer).
 * The blitter does NOT own the etna2d_context — it is shared across all
 * blittables and lives in the integration singleton.
 * ----------------------------------------------------------------------- */
QT_BEGIN_NAMESPACE
class QRecursiveMutex;
QT_END_NAMESPACE
QRecursiveMutex &etnaCtxMutex();

class EtnaBlittable : public QBlittable
{
public:
    /* Takes ownership of surf; ctx is borrowed (not owned). */
    EtnaBlittable(etna2d_context *ctx, etna2d_surface *surf);
    ~EtnaBlittable() override;

    /* Access the underlying GPU surface (used by backing store for flush). */
    etna2d_surface *surface() const { return m_surf; }
    etna2d_context *context() const { return m_ctx; }

    /* Lightweight CPU→GPU barrier for read-only source surfaces.
     * Drains WC write buffer (cpu_fini) but does NOT submit or wait for a
     * GPU fence.  Use this when the surface will only be READ by the GPU
     * (e.g. as a blit source) so we avoid an etna2d_finish() stall per op.
     * If the surface is not CPU-locked this is a no-op. */
    void unlockForGpuRead();

    /* True if every pixel has alpha 0xff.  Scanned once per content version
     * (cached; invalidated by any CPU lock or GPU write to this surface), and
     * only for surfaces up to kOpaqueScanMaxPixels - bigger ones report false. */
    bool isOpaque();

    /* --- QBlittable pure virtuals --- */
    void fillRect(const QRectF &rect, const QColor &color) override;
    void drawPixmap(const QRectF &rect, const QPixmap &pixmap,
                    const QRectF &subrect) override;

    /* --- QBlittable optional virtuals (advertised in Capabilities) ---
     * AlphaFillRectCapability is deliberately NOT declared (the GC520 has no
     * blended fill), so Qt rasters translucent fills itself.  OpacityPixmap IS
     * declared so Qt passes the composition mode (CompositionMode_Source needs
     * it); painter opacity < 1 is then drawn by cpuDrawPixmap(). */
    void alphaFillRect(const QRectF &rect, const QColor &color,
                       QPainter::CompositionMode cmode) override;
    void drawPixmapOpacity(const QRectF &rect, const QPixmap &pixmap,
                           const QRectF &subrect,
                           QPainter::CompositionMode cmode,
                           qreal opacity) override;

    /* Text on the GPU.  Glyphs come from Qt's texture glyph cache (A8 coverage atlas),
     * mirrored into an ETNA2D_FMT_A8 surface; each glyph is a coloured-mask blit. */
    bool drawCachedGlyphs(const QPaintEngineState *state, QFontEngine::GlyphFormat glyphFormat,
                          int numGlyphs, const glyph_t *glyphs, const QFixedPoint *positions,
                          QFontEngine *fontEngine) override;

protected:
    /* CPU map/unmap for the raster fallback path. */
    QImage *doLock() override;
    void    doUnlock() override;

private:
    bool m_opaqueKnown = false;
    bool m_opaque = false;
    /* Extract the etna2d_surface* from a pixmap that is backed by an
     * EtnaBlittable. Returns nullptr if the pixmap is a raster pixmap
     * (in which case drawPixmap/drawPixmapOpacity must fall back). */
    static etna2d_surface *surfaceFromPixmap(const QPixmap &pixmap);

    /* CPU draws into the locked BO.  Qt does NOT fall back for us once a
     * blittable method is entered, so unsupported cases must call these. */
    void cpuFillRect(const QRectF &rect, const QColor &color,
                     QPainter::CompositionMode cmode);
    void cpuDrawPixmap(const QRectF &rect, const QPixmap &pixmap,
                       const QRectF &subrect, QPainter::CompositionMode cmode,
                       qreal opacity);

    /* Premultiply a QColor and return it as a packed ARGB32 uint32_t. */

    etna2d_context *m_ctx;   /* borrowed — owned by integration */
    etna2d_surface *m_surf;  /* owned by this blittable */
    QImage          m_lockImage; /* scratch; valid only between doLock/doUnlock */
    bool            m_skipFinishOnUnlock = false; /* set by unlockForGpuRead() */
};

/* -----------------------------------------------------------------------
 * EtnaPaintEngine — QBlitterPaintEngine that keeps GPU text off the CPU path.
 * QBlitterPaintEngine::drawTextItem/drawStaticTextItem lock the pixmap (a whole-context GPU
 * drain) before the text is drawn and again after the blittable's drawCachedGlyphs().  When the
 * text can be drawn by the GPU (A8 glyphs, scale/translate transform, rect clip, SourceOver is
 * handled inside the blittable) we call the blittable directly and never touch the CPU mapping.
 * Everything else goes to the stock QBlitterPaintEngine implementation.
 * ----------------------------------------------------------------------- */
class EtnaPaintEngine : public QBlitterPaintEngine
{
public:
    explicit EtnaPaintEngine(QBlittablePlatformPixmap *p) : QBlitterPaintEngine(p), m_pm(p) {}
    void drawTextItem(const QPointF &p, const QTextItem &textItem) override;
    void drawStaticTextItem(QStaticTextItem *item) override;
private:
    bool gpuGlyphs(int numGlyphs, const glyph_t *glyphs, const QFixedPoint *positions, QFontEngine *fe);
    QBlittablePlatformPixmap *m_pm;
};

/* -----------------------------------------------------------------------
 * EtnaBlittablePlatformPixmap
 * Factory: creates an EtnaBlittable for each new QPixmap size.
 * Qt's base class supplies paintEngine(), buffer(), fromImage(), classId().
 * ----------------------------------------------------------------------- */
class EtnaBlittablePlatformPixmap : public QBlittablePlatformPixmap
{
public:
    /* ctx/dev are set by the integration after construction. */
    EtnaBlittablePlatformPixmap();

    /* Must be called once before any pixmap is allocated. */
    static void setGlobalContext(etna2d_context *ctx, etna2d_device *dev);
    static etna2d_device *device() { return s_dev; }
    static etna2d_context *context() { return s_ctx; }

    QBlittable *createBlittable(const QSize &size, bool alpha) const override;
    QPaintEngine *paintEngine() const override;   /* EtnaPaintEngine */

    /* Convenience downcast — safe because classId() == BlitterClass. */
    EtnaBlittable *etnaBlittable() const
    {
        return static_cast<EtnaBlittable *>(blittable());
    }

private:
    static etna2d_context *s_ctx;
    static etna2d_device  *s_dev;
};

QT_END_NAMESPACE

#endif // QETNAVIV2DBLITTER_H
