/*
 * qetnaviv2dscreen.h — KMS display screen for the etnaviv2d QPA plugin.
 *
 * Owns the DRM/KMS display side (card0, dumb BOs, page-flip). Reuses
 * QFbScreen for the window-stack and damage-tracking scaffolding;
 * overrides doRedraw() to suppress QFbScreen's software compositing (our
 * backing-store flush() drives all scanout updates directly).
 *
 * Double-buffer state: two dumb BOs on card0 → PRIME-exported → imported
 * into etnaviv as etna2d_surface*.  The backing store renders into an
 * independent offscreen surface then etna2d_blit()s into the back scanout
 * BO before flipping.
 *
 * Thread safety: all KMS calls are serialised by m_flipMutex.  pageFlip()
 * may be called from a render thread (QT_THREADED_PAINTING=1); the mutex
 * ensures only one flip is in-flight at a time.  m_shuttingDown is set by
 * cleanup() so that a late flush() arriving during teardown is silently
 * dropped instead of racing with a partially-freed KMS state.
 *
 * Composite mode (plane=composite):
 *   The screen enumerates both the primary and overlay planes.  The overlay
 *   carries the Qt UI (ARGB8888, SRC_OVER) via the normal backing-store path.
 *   The primary carries video (NV12) via an external producer that calls
 *   attachPrimaryFb() to stage a pre-allocated NV12 FB.  Both plane FB_IDs
 *   are folded into a single drmModeAtomicCommit so they land on the same
 *   vblank.  The producer owns the NV12 BO ring (3 buffers + on_screen bit);
 *   the screen fires setPrimaryReleaseCallback() when a committed buffer is
 *   retired so the producer can clear the on_screen flag.
 */
#ifndef QETNAVIV2DSCREEN_H
#define QETNAVIV2DSCREEN_H

#include <QtFbSupport/private/qfbscreen_p.h>
#include <QMutex>
#include <functional>
#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include "etna2d.h"
}

QT_BEGIN_NAMESPACE

static constexpr int KMS_NUM_BUFS = 2; /* double buffer */

/* Which DRM plane this screen instance scans out on.
 * Resolved once at integration init (arg > QT_ETNAVIV2D_PLANE env >
 * WAYLAND_DISPLAY probe > Primary fallback) and passed to the screen. */
enum class PlaneMode {
    Primary,   /* PRIMARY plane, full-screen opaque (default)                    */
    Overlay,   /* OVERLAY plane, ARGB8888, SRC_OVER blend                        */
    Composite, /* Single-committer dual-plane: overlay UI + primary NV12 video   */
    Wayland,   /* handoff stub — no DRM master, no modeset                       */
};

/* Minimal scanout framebuffer descriptor (mirrors kms_display's kms_fb). */
struct EtnaKmsFb {
    uint32_t fb_id  = 0;
    uint32_t handle = 0;
    uint32_t stride = 0;
    uint64_t size   = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

/*
 * Atomic-KMS property-ID caches.  All IDs are resolved once in initialize()
 * *by name* (drm core assigns IDs per object, so they differ per plane); the
 * app is responsible for checking presence before use.  A 0 ID means "the
 * object does not expose that property" — legal for e.g. the blend props on
 * the primary plane.
 */
struct EtnaPlaneProps {
    uint32_t fb_id  = 0, crtc_id = 0;
    uint32_t src_x  = 0, src_y = 0, src_w = 0, src_h = 0;
    uint32_t crtc_x = 0, crtc_y = 0, crtc_w = 0, crtc_h = 0;
    uint32_t zpos   = 0;
    /* overlay-only custom props (0 on the primary plane) */
    uint32_t blend_mode        = 0;
    uint32_t source_alpha_mode = 0;
};
struct EtnaCrtcProps { uint32_t active = 0, mode_id = 0; };
struct EtnaConnProps { uint32_t crtc_id = 0; };

/*
 * Descriptor for an externally-allocated primary (NV12) framebuffer that the
 * caller stages via attachPrimaryFb().  The screen only uses fbId for the
 * atomic commit; all other fields are for the caller's bookkeeping (e.g. to
 * identify which buffer was retired in the release callback).
 */
struct PrimaryFbInfo {
    uint32_t fbId   = 0;   /* DRM FB id, from drmModeAddFB2 */
    void    *cookie = nullptr; /* caller-defined (e.g. buffer index / pointer) */
};

class QEtnaviv2dScreen : public QFbScreen
{
    Q_OBJECT
public:
    /*
     * cardPath    e.g. "/dev/dri/card0"
     * etnaCtx     shared etna2d context (owned by integration)
     * etnaDev     shared etna2d device  (owned by integration)
     */
    /* sharedKfd: if >= 0 the screen uses this pre-opened DRM master fd and
     * does NOT close it on destruction (caller owns the fd lifetime).
     * Pass -1 (default) to let the screen open card0 itself. */
    QEtnaviv2dScreen(const QString &cardPath,
                     PlaneMode      planeMode,
                     etna2d_context *etnaCtx,
                     etna2d_device  *etnaDev,
                     int             sharedKfd = -1);
    ~QEtnaviv2dScreen() override;

    /* QPlatformScreen */
    QRect geometry()        const override { return mGeometry; }
    int   depth()           const override { return mDepth; }
    QImage::Format format() const override { return mFormat; }
    QSizeF physicalSize()   const override { return mPhysicalSize; }

    /* QFbScreen — suppress the software compositor; backing store drives flip.
     * In composite mode, also called from the scheduleUpdate() path when the
     * video thread stages a new primary FB: drives pageFlip() so the
     * video advances even when the Qt UI is completely static. */
    QRegion doRedraw() override;

    /* Initialize KMS: open card, allocate BOs, modeset. Returns false on error. */
    bool initialize() override;

    /* ---- Display API used by the backing store ---- */

    /* The current back-buffer surface (render target for the backing store). */
    etna2d_surface *backSurface() const { return m_scanoutSurf[m_backIdx]; }

    /* Flip back buffer to display; waits for vblank.
     * videoOnly=true: video-driven commit — overlay uses the
     *   front buffer (already on screen) so the UI stays frozen with no
     *   flutter; front/back indices are NOT swapped.
     * videoOnly=false (default): UI-driven commit (flush() path) — overlay
     *   uses the back buffer (just painted); front/back swap as normal.
     * Returns 0 on success, -1 on error or if shutting down.
     * Thread-safe: serialised by m_flipMutex. */
    int pageFlip(bool videoOnly = false);

    /* Raw DRM fd (for drmHandleEvent poll if needed outside the screen). */
    int drmFd() const { return m_kfd; }

    /* etna2d handles (for constructing a sibling screen with shared context). */
    etna2d_context *etnaCtx()  const { return m_etnaCtx;  }
    etna2d_device  *etnadev()  const { return m_etnadev;  }

    /* True when running in wayland handoff stub mode (no KMS, no scanout
     * surfaces).  The backing store uses this to suppress the flush path
     * silently instead of spamming "no scanout back surface" warnings. */
    bool isStub() const { return m_planeMode == PlaneMode::Wayland; }

    /* Update screen geometry after the Wayland configure round-trip.
     * Only meaningful in plane=wayland mode; updates mGeometry + mPhysicalSize
     * and notifies Qt so the window system sees the correct output size. */
    void setWaylandGeometry(const QRect &geo);

    /* ---- Composite-mode video API (plane=composite only) ---- */

    /*
     * Stage an externally-produced NV12 primary framebuffer for inclusion in
     * the next atomic commit.  Last-write-wins; the producer never blocks.
     * Thread-safe (protected by m_flipMutex).
     *
     * After staging, drives a coalesced GUI-thread repaint so the video
     * advances even when the Qt UI is static: posts a QEvent to the
     * first top-level window.  The repaint is skipped if a flip is already
     * in-flight.
     *
     * The screen does NOT own the buffer.  The producer retains ownership and
     * must not recycle the buffer until the release callback fires with its
     * cookie.
     */
    void attachPrimaryFb(const PrimaryFbInfo &info);

    /*
     * Register a callback that fires on the flip-event drain thread when a
     * previously-committed primary buffer has been retired by the display
     * controller.  The callback receives the PrimaryFbInfo that was passed to
     * attachPrimaryFb().  The producer should use this to clear the on_screen
     * bit for the retired buffer.
     *
     * Only one callback is active at a time (the last one registered wins).
     * Pass a default-constructed std::function to unregister.
     */
    void setPrimaryReleaseCallback(std::function<void(const PrimaryFbInfo &)> cb);

private:
    void cleanup();

    /* Resolve a property ID by name on a given DRM object.  Returns 0 if the
     * object does not expose that property (callers must check before use). */
    static uint32_t propId(int fd, uint32_t objId, uint32_t objType,
                           const char *name);

    /* Find a plane of the given DRM type for m_crtcIndex.  Returns 0 on failure. */
    uint32_t findPlaneByType(uint32_t planeType);

    /* Resolve all atomic plane property IDs into dst. */
    void resolvePlaneProps(uint32_t planeId, EtnaPlaneProps &dst);

    QString         m_cardPath;
    PlaneMode       m_planeMode  = PlaneMode::Primary;
    etna2d_context *m_etnaCtx;
    etna2d_device  *m_etnadev;
    bool            m_ownsKfd    = true;  /* false when sharedKfd was provided */

    /* KMS state (replicated from kms_display internals for Qt integration). */
    int             m_kfd         = -1;
    uint32_t        m_connId      = 0;
    uint32_t        m_crtcId      = 0;
    int             m_crtcIndex   = -1;   /* index of m_crtcId in res->crtcs */
    uint32_t        m_planeId     = 0;    /* the plane we scan out on (overlay in composite) */
    uint32_t        m_primaryPlaneId = 0; /* primary plane id (composite mode only) */
    drmModeCrtc    *m_savedCrtc   = nullptr;

    /* Atomic property-ID caches (resolved by name in initialize()). */
    EtnaPlaneProps  m_planeProps;          /* overlay (or primary in non-composite modes) */
    EtnaPlaneProps  m_primaryPlaneProps;   /* primary plane props (composite mode only) */
    EtnaCrtcProps   m_crtcProps;
    EtnaConnProps   m_connProps;
    uint32_t        m_modeBlobId  = 0;    /* MODE_ID blob for atomic modeset */

    /* Double-buffer scanout resources (overlay / primary in non-composite). */
    struct EtnaKmsFb m_fb[KMS_NUM_BUFS];
    etna2d_surface *m_scanoutSurf[KMS_NUM_BUFS] = {};
    int             m_dmabuf[KMS_NUM_BUFS]       = {-1, -1};
    int             m_frontIdx   = 0;
    int             m_backIdx    = 1;
    volatile int    m_flipPending = 0;

    QMutex          m_flipMutex;                /* serialises pageFlip() callers */
    volatile bool   m_shuttingDown = false;     /* set in cleanup(); drops late flushes */

    /* Composite mode: staged primary NV12 FB (last-write-wins staging slot). */
    PrimaryFbInfo   m_stagedPrimary;            /* protected by m_flipMutex */
    PrimaryFbInfo   m_onScreenPrimary;          /* buffer currently scanned by DC */
    PrimaryFbInfo   m_retiredPrimary;           /* buffer retired by the last commit (for release cb) */
    bool            m_hasStagedPrimary = false; /* true when m_stagedPrimary is valid */
    bool            m_videoPresent     = false; /* set by attachPrimaryFb, cleared by doRedraw/flush */
    std::function<void(const PrimaryFbInfo &)> m_primaryReleaseCb; /* protected by m_flipMutex */

    /* Saved mode info for geometry. */
    drmModeModeInfo m_modeInfo   = {};
};

QT_END_NAMESPACE

#endif // QETNAVIV2DSCREEN_H
