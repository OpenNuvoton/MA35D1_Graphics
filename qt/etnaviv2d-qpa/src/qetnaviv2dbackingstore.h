/*
 * qetnaviv2dbackingstore.h — QPlatformBackingStore for the etnaviv2d plugin.
 *
 *  - paintDevice() returns a QPixmap backed by an EtnaBlittablePlatformPixmap
 *    (an offscreen GPU surface).  Qt paints into it via QBlitterPaintEngine.
 *  - flush() blits the offscreen backing BO into the current target back-buffer
 *    via etna2d_blit(), then either:
 *      (a) KMS modes: calls screen->pageFlip().
 *      (b) Wayland mode: calls transport->commitFrame() which drives the
 *          wl_surface.frame-callback paced commit to Weston.
 */
#ifndef QETNAVIV2DBACKINGSTORE_H
#define QETNAVIV2DBACKINGSTORE_H

#include <qpa/qplatformbackingstore.h>
#include <QtGui/private/qpixmap_blitter_p.h>

#include "qetnaviv2dblitter.h"
#include "qetnaviv2dscreen.h"
#include <QHash>
#include <QRegion>

QT_BEGIN_NAMESPACE

class QEtnaviv2dWaylandTransport;

class QEtnaviv2dBackingStore : public QPlatformBackingStore
{
public:
    explicit QEtnaviv2dBackingStore(QWindow *window,
                                    QEtnaviv2dScreen *screen,
                                    QEtnaviv2dWaylandTransport *transport = nullptr);
    ~QEtnaviv2dBackingStore() override;

    /* Qt paints into this device. */
    QPaintDevice *paintDevice() override;

    /* Blit backing surface → scanout/ring back-buffer, then page-flip or commit. */
    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override;

    /* Reallocate the backing surface when the window is resized. */
    void resize(const QSize &size, const QRegion &staticContents) override;

    /* For screenshots / accessibility. */
    QImage toImage() const override;

    /* Scroll acceleration: GPU blit within the backing surface. */
    bool scroll(const QRegion &area, int dx, int dy) override;

private:
    /* Damage tracking.  The backing store is the
     * truth; every target buffer (ring slot / scanout buffer) lags behind it
     * by the regions flushed since that buffer was last written.  Returns the
     * region of the backing store that must be copied into 'target' now and
     * marks that buffer up to date.  A buffer seen for the first time (or
     * after a resize) is fully stale. */
    void    accumulateStale(const QRegion &flushed, const QRect &bounds);
    QRegion takeDamage(etna2d_surface *target, const QRegion &flushed, const QRect &bounds);
    void    blitRegion(etna2d_context *ctx, etna2d_surface *src, etna2d_surface *dst,
                       const QRegion &r);

    /* Shared by the Wayland and KMS present paths. */
    bool    blitToTarget(EtnaBlittable *src, etna2d_surface *target, const QRegion &region,
                         const QRect &bounds, const char *what, QRegion *copied);  /* damage -> blit -> finish -> verify */

    QHash<etna2d_surface *, QRegion> m_stale;  /* per target buffer */
    bool                         m_fullFlush = false; /* QT_ETNA2D_FULL_FLUSH=1 (A/B) */

    QEtnaviv2dScreen            *m_screen;     /* borrowed */
    QEtnaviv2dWaylandTransport  *m_transport;  /* borrowed; non-null in wayland mode */
    QScopedPointer<QPixmap>      m_pixmap;     /* wraps the blittable */
    EtnaBlittablePlatformPixmap *m_pmdata = nullptr; /* raw ptr into m_pixmap */
};

QT_END_NAMESPACE

#endif // QETNAVIV2DBACKINGSTORE_H

