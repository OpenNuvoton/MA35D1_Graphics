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

    /* --- QBlittable pure virtuals --- */
    void fillRect(const QRectF &rect, const QColor &color) override;
    void drawPixmap(const QRectF &rect, const QPixmap &pixmap,
                    const QRectF &subrect) override;

    /* --- QBlittable optional virtuals (advertised in Capabilities) --- */
    void alphaFillRect(const QRectF &rect, const QColor &color,
                       QPainter::CompositionMode cmode) override;
    void drawPixmapOpacity(const QRectF &rect, const QPixmap &pixmap,
                           const QRectF &subrect,
                           QPainter::CompositionMode cmode,
                           qreal opacity) override;

protected:
    /* CPU map/unmap for the raster fallback path. */
    QImage *doLock() override;
    void    doUnlock() override;

private:
    /* Extract the etna2d_surface* from a pixmap that is backed by an
     * EtnaBlittable. Returns nullptr if the pixmap is a raster pixmap
     * (in which case drawPixmap/drawPixmapOpacity must fall back). */
    static etna2d_surface *surfaceFromPixmap(const QPixmap &pixmap);

    /* Premultiply a QColor and return it as a packed ARGB32 uint32_t. */

    etna2d_context *m_ctx;   /* borrowed — owned by integration */
    etna2d_surface *m_surf;  /* owned by this blittable */
    QImage          m_lockImage; /* scratch; valid only between doLock/doUnlock */
    bool            m_skipFinishOnUnlock = false; /* set by unlockForGpuRead() */
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

    QBlittable *createBlittable(const QSize &size, bool alpha) const override;

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
