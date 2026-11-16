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
#include "qetnaviv2dstats.h"
#include "qetnaviv2dconfig.h"

#include <QDebug>
#include <QTimer>
#include <QMutexLocker>
#include "qetnaviv2dblitter.h"
#include <string.h>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

#define BS_DBG  ETNA_LOG_DBG("backingstore")
#define BS_WARN ETNA_LOG_WARN("backingstore")

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
    m_fullFlush = etnaConfig().fullFlush;
    if (m_transport) {
        m_transport->setReleaseHook([this]() { presentPending(); });
    } else if (m_screen && m_screen->asyncFlip()) {
        m_screen->armFlipNotifier();
        m_screen->setFlipDoneHook([this]() { presentPending(); });
    }
    BS_DBG << "created for window" << window
           << (transport ? "(wayland mode)" : "(KMS mode)");
    /* Allocate initial backing surface sized to the window. */
    resize(window->size(), QRegion());
}

QEtnaviv2dBackingStore::~QEtnaviv2dBackingStore()
{
    if (m_transport)
        m_transport->setReleaseHook(nullptr);
    else if (m_screen)
        m_screen->setFlipDoneHook(nullptr);
    if (EtnaStats::enabled())
        fprintf(stderr, "ETNAFINAL pending_drop=%d (must be 0: a dropped frame was never presented otherwise)\n", int(m_dropped));
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
    m_stale.clear();   /* every target buffer is stale after a resize */

    /* Create a new EtnaBlittablePlatformPixmap; it owns an offscreen BO. */
    auto *pmdata = new EtnaBlittablePlatformPixmap();
    pmdata->resize(size.width(), size.height());

    m_pixmap.reset(new QPixmap(pmdata)); /* QPixmap takes ownership of pmdata */
    m_pmdata = pmdata;

    BS_DBG << "resize done: backing surf=" << (m_pmdata->etnaBlittable()
            ? m_pmdata->etnaBlittable()->surface() : nullptr);
}

/* QT_ETNA2D_VERIFY_FLUSH=1 (debug): after the blit, the target buffer must equal the
 * backing store everywhere, not only inside the damage region.  Slow (reads WC memory). */
static void verifyTarget(etna2d_surface *src, etna2d_surface *dst, const char *what)
{
    const bool en = etnaConfig().verifyFlush;
    if (!en || etna2d_surface_format(src) != etna2d_surface_format(dst))
        return;
    const uchar *a = static_cast<const uchar *>(etna2d_surface_map(src));
    const uchar *b = static_cast<const uchar *>(etna2d_surface_map(dst));
    if (!a || !b) return;
    const int w = qMin(etna2d_surface_width(src), etna2d_surface_width(dst));
    const int h = qMin(etna2d_surface_height(src), etna2d_surface_height(dst));
    const int bpp = int(etna2d_surface_stride(src)) / etna2d_surface_width(src);
    static long long frames = 0, bad = 0;
    ++frames;
    for (int y = 0; y < h; ++y) {
        const uchar *ra = a + size_t(y) * etna2d_surface_stride(src);
        const uchar *rb = b + size_t(y) * etna2d_surface_stride(dst);
        if (memcmp(ra, rb, size_t(w) * bpp)) {
            int x = 0;
            while (!memcmp(ra + x * bpp, rb + x * bpp, bpp)) ++x;
            ++bad;
            BS_WARN << "VERIFY FAIL" << what << "frame" << frames << "first diff at x=" << x << "y=" << y
                    << "(bad frames so far" << bad << ")";
            return;
        }
    }
    if (frames % 100 == 0)
        fprintf(stderr, "VERIFYFLUSH frames=%lld bad=%lld\n", frames, bad);
}

/* -----------------------------------------------------------------------
 * Damage tracking helpers
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::accumulateStale(const QRegion &flushed, const QRect &bounds)
{
    const QRegion clipped = m_fullFlush ? QRegion(bounds) : (flushed & bounds);
    for (auto it = m_stale.begin(); it != m_stale.end(); ++it)
        it.value() += clipped;
}

QRegion QEtnaviv2dBackingStore::takeDamage(etna2d_surface *target,
                                           const QRegion &flushed,
                                           const QRect &bounds)
{
    accumulateStale(flushed, bounds);
    auto it = m_stale.find(target);
    if (it == m_stale.end())
        it = m_stale.insert(target, QRegion(bounds));   /* first use: fully stale */
    const QRegion out = it.value() & bounds;
    it.value() = QRegion();
    return out;
}

void QEtnaviv2dBackingStore::blitRegion(etna2d_context *ctx, etna2d_surface *src,
                                        etna2d_surface *dst, const QRegion &r)
{
    /* Cost model(measured: full 1024x600 blit 8.7 ms = ~14 ns/px; per-op overhead
     * ~30 us = ~2100 px).  One bounding blit beats n rects when the pixels it adds cost less
     * than the (n-1) ops it saves. */
    QVector<QRect> rects(r.begin(), r.end());
    if (rects.size() > 1) {
        qint64 sum = 0;
        for (const QRect &q : qAsConst(rects)) sum += qint64(q.width()) * q.height();
        const QRect bb = r.boundingRect();
        const qint64 extra = qint64(bb.width()) * bb.height() - sum;
        if (extra < qint64(rects.size() - 1) * 2100 || rects.size() > 128)
            rects = { bb };
    }
    const int bpp = etna2d_surface_stride(src) / qMax(1, etna2d_surface_width(src));
    for (const QRect &q : qAsConst(rects)) {
        if (q.isEmpty()) continue;
        etna2d_blit(ctx, src, q.x(), q.y(), dst, q.x(), q.y(), q.width(), q.height(),
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
        EtnaStats::add(EtnaStats::BytesCopied, 0, (long long)q.width() * q.height() * bpp);
    }
}

/* -----------------------------------------------------------------------
 * Present helpers shared by the Wayland and KMS paths
 * ----------------------------------------------------------------------- */

/* The target buffer is not available (ring full / flip pending): the backing store already holds
 * the pixels, so remember the region as stale for every target and present it from the next
 * release / flip-done event.  retryFromLoop: no event will come (QT_ETNA2D_TEST_DROP), so retry
 * from the event loop. */
void QEtnaviv2dBackingStore::deferFrame(const QRegion &region, const QRect &bounds, bool retryFromLoop)
{
    EtnaStats::add(EtnaStats::DroppedFrames);
    accumulateStale(region, bounds);
    m_dropped = true;
    if (retryFromLoop && !m_retryQueued) {
        m_retryQueued = true;
        QTimer::singleShot(0, [this]() { m_retryQueued = false; presentPending(); });
    }
    BS_DBG << "flush: target busy, frame deferred";
}

/* Copy what 'target' is missing (damage tracking) from the backing surface and wait for the GPU.
 * *copied gets the copied region (may be empty); false if the GPU wait failed. */
bool QEtnaviv2dBackingStore::blitToTarget(EtnaBlittable *src, etna2d_surface *target,
                                          const QRegion &region, const QRect &bounds,
                                          const char *what, QRegion *copied)
{
    etna2d_context *ctx         = src->context();
    etna2d_surface *backingSurf = src->surface();

    /* Drain CPU writes + GPU fence for any raster-fallback ops. */
    src->unlock();

    /* Only what this target is missing: it lags the backing store by the regions flushed since it
     * was last written, so no ghosting. */
    const QRegion damage = takeDamage(target, region, bounds);
    BS_DBG << "flush(" << what << "): blit" << damage.boundingRect() << "(" << damage.rectCount()
           << "rects) ->" << target;
    {
        EtnaStats::Scope _t(EtnaStats::FlushBlit);
        blitRegion(ctx, backingSurf, target, damage);
    }
    {
        EtnaStats::Scope _tf(EtnaStats::FlushFinish);
        if (etna2d_finish(ctx) != ETNA2D_OK) {
            BS_WARN << "flush(" << what << "): etna2d_finish failed";
            m_stale.remove(target);      /* contents unknown: fully stale again */
            return false;
        }
    }
    verifyTarget(backingSurf, target, what);
    *copied = damage;
    return true;
}

/* -----------------------------------------------------------------------
 * flush — blit backing BO → target buffer, then commit / page-flip
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::flush(QWindow * /*window*/,
                                   const QRegion &region,
                                   const QPoint  &offset)
{
    QMutexLocker _etnaLock(&etnaCtxMutex());   /* shared GC520 queue */
    Q_UNUSED(offset); /* always (0,0): the backing store is the whole window */

    if (!m_pmdata || !m_pmdata->etnaBlittable()) {
        BS_WARN << "flush: no backing blittable";
        return;
    }

    EtnaBlittable *blitter      = m_pmdata->etnaBlittable();
    etna2d_surface *backingSurf = blitter->surface();
    struct FrameEnd { ~FrameEnd() { if (EtnaStats::enabled()) EtnaStats::report(); } } _fe;
    EtnaStats::add(EtnaStats::Frames);

    /* QT_ETNA2D_TEST_DROP=N (test): behave as if the target were busy on every Nth flush. */
    static int flushCount = 0;
    const int testDrop = etnaConfig().testDrop;
    const bool fakeBusy = testDrop > 0 && ++flushCount % testDrop == 0;

    /* ---- Wayland mode: blit into the ring BO, hand off to transport ---- */
    if (m_transport) {
        /* the ring follows the top-level window size (resize(), showFullScreen(), output
         * mode change) unless QT_IVI_GEOMETRY pins it.  Done here, at present time, so a window
         * that is resized several times before painting costs one reallocation. */
        const QSize want(m_pmdata->width(), m_pmdata->height());
        if (!etnaConfig().iviPinned && window() && window()->type() == Qt::Window
                && !want.isEmpty() && m_transport->ringSize() != want) {
            m_stale.clear();   /* new ring surfaces: every target is fully stale */
            if (!m_transport->resizeRing(want)) {
                BS_WARN << "flush(wayland): ring resize to" << want << "failed";
                return;
            }
        }
        const QRect bounds(0, 0, etna2d_surface_width(backingSurf), etna2d_surface_height(backingSurf));
        etna2d_surface *ringBo = fakeBusy ? nullptr : m_transport->currentBo();
        if (!ringBo) {
            /* All ring slots are held by Weston: present from wl_buffer.release */
            deferFrame(region, bounds, /*retryFromLoop=*/!m_transport->ringBusy());
            return;
        }
        QRegion damage;
        if (!blitToTarget(blitter, ringBo, region, bounds, "wayland", &damage))
            return;
        m_dropped = false;
        m_transport->commitFrame(ringBo, damage);   /* frame callback commits */
        return;
    }

    /* ---- KMS: blit into the back scanout buffer, flip ---- */
    etna2d_surface *scanoutBack = m_screen->backSurface();
    if (!scanoutBack) {
        BS_WARN << "flush: no scanout back surface";
        return;
    }

    /* Fixed screen: the scanout is the connector mode; a larger window is clipped to
     * it, a smaller one leaves the (black-cleared) rest untouched. */
    const QRect bounds = QRect(0, 0, etna2d_surface_width(backingSurf), etna2d_surface_height(backingSurf))
                             .intersected(QRect(0, 0, etna2d_surface_width(scanoutBack),
                                                      etna2d_surface_height(scanoutBack)));

    /* Non-blocking flip: while the previous flip is in flight the back buffer is not ours;
     * present from the flip-done hook. */
    if (m_screen->asyncFlip() && (fakeBusy || m_screen->flipBusy())) {
        deferFrame(region, bounds, /*retryFromLoop=*/fakeBusy);
        return;
    }

    QRegion damage;
    if (!blitToTarget(blitter, scanoutBack, region, bounds, "kms", &damage))
        return;

    /* Blocks until vblank only with QT_ETNA2D_SYNC_FLIP=1. */
    m_dropped = false;
    EtnaStats::Scope _tp(EtnaStats::FlipWait);
    if (m_screen->pageFlip() != 0)
        BS_WARN << "flush(KMS): pageFlip failed";
}

void QEtnaviv2dBackingStore::presentPending()
{
    if (!m_dropped || !window())
        return;
    EtnaStats::add(EtnaStats::RelPresent);
    BS_DBG << "presenting frame dropped earlier (ring slot available)";
    flush(window(), QRegion(), QPoint());
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
