/*
 * qetnaviv2dwaylandtransport.h — Wayland client transport for the etnaviv2d QPA.
 *
 * Fills the plane=wayland path (isStub() seam in QEtnaviv2dScreen).
 *
 *  - Owns all libwayland-client state: display, registry, compositor,
 *    xdg_wm_base, xdg_surface, xdg_toplevel, zwp_linux_dmabuf_v1.
 *  - Allocates a 3-deep ARGB8888/LINEAR BO ring whose BOs are:
 *      (a) rendered into by the GC520 blitter (EtnaBlittable / etna2d_blit),
 *      (b) exported as dmabuf via etna_bo_dmabuf(),
 *      (c) wrapped as persistent wl_buffer objects (created once, reused).
 *  - Frame lifecycle:
 *      BackingStore::flush() blits into the current free ring BO, then calls
 *      commitFrame().  commitFrame() marks the surface dirty; the actual
 *      wl_surface_attach + commit happens in the wl_surface.frame callback
 *      (vsync-aligned, coalesced).  First frame bootstraps the callback chain.
 *  - Event-loop integration: single-threaded QSocketNotifier on
 *    wl_display_get_fd(); uses the prepare_read/read_events/dispatch_pending
 *    sequence to avoid the Wayland fd-starvation deadlock; flushes on
 *    QAbstractEventDispatcher::aboutToBlock.
 *  - wl_buffer.release drives the ring back-pressure (pickFree() discipline).
 *
 * wl_seat input bridge:
 *  - Binds wl_seat from the registry.
 *  - On wl_seat.capabilities: creates wl_pointer, wl_touch, wl_keyboard as
 *    advertised.
 *  - Translates Wayland input events → QWindowSystemInterface calls on the
 *    single fullscreen QWindow (m_window, set by setWindow()).
 *  - wl_pointer: motion/enter/leave → handleMouseEvent; button → handleMouseEvent;
 *    axis → handleWheelEvent.
 *  - wl_touch: down/up/motion/frame/cancel → handleTouchEvent.  Weston delivers
 *    pre-calibrated surface-local coordinates (libinput matrix already applied).
 *  - wl_keyboard: key → handleKeyEvent via a compact linux→Qt keycode table.
 *    No xkbcommon dependency (embedded kiosk; raw keycodes suffice).
 *
 * ivi-shell auto-detection:
 *  - In registryGlobal, binds ivi_application when advertised (ivi-shell).
 *  - In connect(), detects shell at runtime:
 *      ivi-shell  → ivi_application_surface_create(IVI_ID_QT, wl_surface)
 *      kiosk-shell → xdg_wm_base + xdg_toplevel (existing path)
 *  - IVI surface ID defaults to 2000; override with QT_IVI_SURFACE_ID env var.
 *  - No rebuild required to switch shells — only weston.ini changes.
 *
 * Thread safety: all methods must be called from the Qt GUI thread.
 */
#pragma once

#include <QObject>
#include <QRect>
#include <QSocketNotifier>
#include <QWindow>
#include <QPointF>
#include <QVector>
#include <functional>
#include <atomic>
#include <cstddef>

// Forward-declare libwayland types to avoid pulling C headers into every TU
// that includes this header.
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_buffer;
struct wl_callback;
struct wl_seat;
struct wl_pointer;
struct wl_touch;
struct wl_keyboard;
struct wl_shm;
struct wl_shm_pool;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct ivi_application;
struct ivi_surface;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_buffer_params_v1;

// libetna2d / etnaviv forward declarations
struct etna2d_device;
struct etna2d_surface;
struct etna_bo;

QT_BEGIN_NAMESPACE

/* Number of ARGB ring buffers.  3 gives the scheduler room: one on screen,
 * one committed (in-flight), one being painted by the blitter. */
static constexpr int WL_RING_SIZE = 3;

/* -----------------------------------------------------------------------
 * ArgbBo — one slot in the ARGB ring
 * ----------------------------------------------------------------------- */
struct ArgbBo {
    etna2d_surface  *surf     = nullptr; // GC520 render target (blitter writes here)
    int              dmabuf_fd = -1;     // dmabuf fd (owned; closed on destroy)
    wl_buffer       *wl_buf   = nullptr; // persistent wl_buffer (created once)
    std::atomic<bool> in_use  { false }; // true while Weston holds a reference
};

/* -----------------------------------------------------------------------
 * QEtnaviv2dWaylandTransport
 * ----------------------------------------------------------------------- */
class QEtnaviv2dWaylandTransport : public QObject
{
    Q_OBJECT

public:
    explicit QEtnaviv2dWaylandTransport(etna2d_device *etnaDev,
                                        int            cardFd,
                                        QObject       *parent = nullptr);
    ~QEtnaviv2dWaylandTransport() override;

    /* connect(): bind globals, create surface role.
     * Auto-detects shell: if ivi_application is advertised uses IVI role,
     * otherwise falls back to xdg_toplevel (kiosk-shell).
     * Pumps the display until the configure round-trip completes
     * so the caller can read outputGeometry() immediately after. */
    bool connect();

    /* Output geometry delivered by the xdg_surface configure callback. */
    QRect outputGeometry() const { return m_outputGeometry; }

    /* Allocate the ARGB ring using the output geometry.
     * Must be called after connect() (needs m_outputGeometry).
     * Returns false if BO allocation or dmabuf export fails. */
    bool allocateRing();

    /* Called from BackingStore::flush() after the blitter has rendered into
     * the current free ring BO.  Marks the surface dirty; the frame callback
     * fires the actual attach+commit.  On the very first call (no pending
     * callback) the commit is issued immediately to bootstrap the chain.
     *
     * bo must be the surface returned by currentBo() — this is how the
     * transport knows which slot to attach. */
    void commitFrame(etna2d_surface *bo);

    /* Returns the ring BO the blitter should render into for the next frame.
     * Returns nullptr if all slots are in_use (back-pressure). */
    etna2d_surface *currentBo();

    /* Disconnect and free all Wayland and BO resources. */
    void disconnect();

    /* Is the transport connected and operational? */
    bool isConnected() const { return m_connected; }

    /* Register the Qt window that receives translated input events.
     * Must be called after the QWindow has been created (typically from
     * QEtnaviv2dIntegration::createWindow) so the transport can target
     * QWindowSystemInterface calls at it. */
    void setWindow(QWindow *w) { m_window = w; }
    QWindow *window() const   { return m_window; }

private slots:
    void onWaylandReadable();
    void onAboutToBlock();

private:
    /* Wayland registry listener callbacks (static trampolines) */
    static void registryGlobal(void *data, wl_registry *reg,
                               uint32_t name, const char *iface,
                               uint32_t version);
    static void registryGlobalRemove(void *data, wl_registry *reg,
                                     uint32_t name);

    /* xdg_wm_base ping */
    static void xdgWmBasePing(void *data, xdg_wm_base *base, uint32_t serial);

    /* xdg_surface configure */
    static void xdgSurfaceConfigure(void *data, xdg_surface *surf,
                                    uint32_t serial);

    /* xdg_toplevel configure + close */
    static void xdgToplevelConfigure(void *data, xdg_toplevel *tl,
                                     int32_t w, int32_t h,
                                     struct wl_array *states);
    static void xdgToplevelClose(void *data, xdg_toplevel *tl);

    /* ivi_surface configure */
    static void iviSurfaceConfigure(void *data, ivi_surface *surf,
                                    int32_t width, int32_t height);

    /* zwp_linux_dmabuf_v1 format/modifier (ignored — we use LINEAR) */
    static void dmabufFormat(void *data, zwp_linux_dmabuf_v1 *dmabuf,
                             uint32_t format);
    static void dmabufModifier(void *data, zwp_linux_dmabuf_v1 *dmabuf,
                               uint32_t format,
                               uint32_t modifier_hi, uint32_t modifier_lo);

    /* zwp_linux_buffer_params_v1 created callback */
    static void bufParamsCreated(void *data,
                                 zwp_linux_buffer_params_v1 *params,
                                 wl_buffer *buf);
    static void bufParamsFailed(void *data,
                                zwp_linux_buffer_params_v1 *params);

    /* wl_buffer release callback */
    static void bufferRelease(void *data, wl_buffer *buf);

    /* wl_surface frame callback */
    static void frameCallback(void *data, wl_callback *cb, uint32_t time);

    /* ---- wl_seat / wl_pointer / wl_touch / wl_keyboard ---- */

    /* wl_seat */
    static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps);
    static void seatName(void *data, wl_seat *seat, const char *name);

    /* wl_pointer */
    static void pointerEnter(void *data, wl_pointer *p, uint32_t serial,
                             wl_surface *surf, int32_t sx, int32_t sy);
    static void pointerLeave(void *data, wl_pointer *p, uint32_t serial,
                             wl_surface *surf);
    static void pointerMotion(void *data, wl_pointer *p, uint32_t time,
                              int32_t sx, int32_t sy);
    static void pointerButton(void *data, wl_pointer *p, uint32_t serial,
                              uint32_t time, uint32_t button, uint32_t state);
    static void pointerAxis(void *data, wl_pointer *p, uint32_t time,
                            uint32_t axis, int32_t value);
    static void pointerFrame(void *data, wl_pointer *p);
    static void pointerAxisSource(void *data, wl_pointer *p, uint32_t source);
    static void pointerAxisStop(void *data, wl_pointer *p, uint32_t time,
                                uint32_t axis);
    static void pointerAxisDiscrete(void *data, wl_pointer *p, uint32_t axis,
                                    int32_t discrete);

    /* wl_touch */
    static void touchDown(void *data, wl_touch *t, uint32_t serial,
                          uint32_t time, wl_surface *surf,
                          int32_t id, int32_t x, int32_t y);
    static void touchUp(void *data, wl_touch *t, uint32_t serial,
                        uint32_t time, int32_t id);
    static void touchMotion(void *data, wl_touch *t, uint32_t time,
                            int32_t id, int32_t x, int32_t y);
    static void touchFrame(void *data, wl_touch *t);
    static void touchCancel(void *data, wl_touch *t);

    /* wl_keyboard */
    static void keyboardKeymap(void *data, wl_keyboard *kb, uint32_t format,
                               int32_t fd, uint32_t size);
    static void keyboardEnter(void *data, wl_keyboard *kb, uint32_t serial,
                              wl_surface *surf, struct wl_array *keys);
    static void keyboardLeave(void *data, wl_keyboard *kb, uint32_t serial,
                              wl_surface *surf);
    static void keyboardKey(void *data, wl_keyboard *kb, uint32_t serial,
                            uint32_t time, uint32_t key, uint32_t state);
    static void keyboardModifiers(void *data, wl_keyboard *kb, uint32_t serial,
                                  uint32_t mods_depressed, uint32_t mods_latched,
                                  uint32_t mods_locked, uint32_t group);
    static void keyboardRepeatInfo(void *data, wl_keyboard *kb,
                                   int32_t rate, int32_t delay);

    /* Key translation helper: linux evdev keycode → Qt::Key */
    static Qt::Key linuxKeyToQt(uint32_t linux_key);

    /* wl_shm format (informational only) */
    static void shmFormat(void *data, wl_shm *shm, uint32_t format);

    /* Hardware cursor: build a 32x32 ARGB8888 wl_shm cursor surface lazily,
     * on first pointer enter.  Returns false on any allocation failure
     * (memfd/mmap/wl_shm_pool) — cursor is then simply not shown, no crash. */
    bool createCursorBuffer();

    /* Helpers */
    void armNotifier();           // create QSocketNotifier lazily on first commitFrame()
    void doCommit(int slot);      // attach buf[slot], damage, commit, arm next frame cb
    void armFrameCallback();          // request wl_surface.frame
    int  pickFree() const;            // return first non-in_use slot, or -1

    /* ---- libwayland objects ---- */
    wl_display          *m_display    = nullptr;
    wl_registry         *m_registry   = nullptr;
    wl_compositor       *m_compositor = nullptr;
    wl_surface          *m_surface    = nullptr;
    /* xdg-shell role (kiosk-shell) */
    xdg_wm_base         *m_xdgWmBase  = nullptr;
    xdg_surface         *m_xdgSurface = nullptr;
    xdg_toplevel        *m_xdgToplevel = nullptr;
    /* ivi-shell role */
    ivi_application     *m_iviApp     = nullptr;
    ivi_surface         *m_iviSurface = nullptr;

    zwp_linux_dmabuf_v1 *m_dmabuf     = nullptr;
    wl_callback         *m_frameCallback = nullptr;

    /* ---- input objects ---- */
    wl_seat             *m_seat       = nullptr;
    wl_pointer          *m_pointer    = nullptr;
    wl_touch            *m_touch      = nullptr;
    wl_keyboard         *m_keyboard   = nullptr;

    /* ---- Hardware cursor (wl_shm + dedicated cursor wl_surface) ---- */
    wl_shm              *m_shm             = nullptr;
    wl_surface          *m_cursorSurface   = nullptr;
    wl_buffer           *m_cursorBuffer    = nullptr;
    void                *m_cursorShmData   = nullptr; // mmap'd memfd, size m_cursorShmSize
    int                  m_cursorShmFd     = -1;
    size_t               m_cursorShmSize   = 0;
    bool                 m_cursorBufferReady = false; // createCursorBuffer() succeeded

    /* Target Qt window for input event delivery (set by setWindow()) */
    QWindow             *m_window     = nullptr;

    /* Pointer state accumulated between wl_pointer events */
    QPointF              m_pointerPos;
    Qt::MouseButtons     m_pointerButtons { Qt::NoButton };

    /* Touch point accumulator: flushed to Qt on touchFrame() */
    struct TouchPoint {
        int     id;
        QPointF pos;
        Qt::TouchPointState state;
    };
    QVector<TouchPoint>  m_touchPoints;

    /* Keyboard modifier state (raw bitmask from wl_keyboard.modifiers) */
    uint32_t             m_keyModsDepressed = 0;
    uint32_t             m_keyModsLatched   = 0;
    uint32_t             m_keyModsLocked    = 0;

    /* ---- BO ring ---- */
    ArgbBo   m_ring[WL_RING_SIZE];
    int      m_currentSlot = 0;   // slot last handed to the blitter

    /* ---- State ---- */
    etna2d_device *m_etnadev   = nullptr;
    int            m_cardFd    = -1;  // DRM card fd for etna2d_surface allocation
    QRect          m_outputGeometry;
    bool           m_connected         = false;
    bool           m_configured        = false; // xdg_surface configure received
    bool           m_dirty             = false; // commitFrame() called, waiting for frame cb
    int            m_pendingCommitSlot = -1;    // slot to attach on next frame cb

    QSocketNotifier *m_notifier = nullptr;
};

QT_END_NAMESPACE
