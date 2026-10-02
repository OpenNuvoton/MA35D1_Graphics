/*
 * qetnaviv2dintegration.cpp — QPlatformIntegration implementation.
 *
 * Initialization order:
 *  1. Open render node (renderD128) and create etna2d context.
 *  2. Publish context to EtnaBlittablePlatformPixmap (static setter).
 *  3. Create QEtnaviv2dScreen — opens card0, allocates BOs, modeset.
 *  4. Register screen with Qt's window system.
 *  5. Set up input (evdev keyboard/mouse/touch) and VT handler.
 *  6. Create input context (on-screen keyboard support).
 *
 * Params parsed from QT_QPA_PLATFORM="etnaviv2d:card=/dev/dri/card0:
 *   render=/dev/dri/renderD128":
 *   - card=<path>    (default /dev/dri/card0)
 *   - render=<path>  (default /dev/dri/renderD128)
 *   - ctx=<nops>     (context op-buffer size, default 512)
 *
 * Reference: qlinuxfbintegration.cpp, qdirectfbintegration.cpp
 */
#include "qetnaviv2dintegration.h"
#include "qetnaviv2dblitter.h"
#include "qetnaviv2dscreen.h"
#include "qetnaviv2dbackingstore.h"
#include "qetnaviv2dwaylandtransport.h"

#include <QtFontDatabaseSupport/private/qgenericunixfontdatabase_p.h>
#include <QtServiceSupport/private/qgenericunixservices_p.h>
#include <QtEventDispatcherSupport/private/qgenericunixeventdispatcher_p.h>
#include <QtFbSupport/private/qfbvthandler_p.h>
#include <QtFbSupport/private/qfbwindow_p.h>

#include <QtGui/private/qpixmap_blitter_p.h>
#include <QtGui/private/qpixmap_raster_p.h>

#include <qpa/qplatforminputcontextfactory_p.h>
#include <qpa/qwindowsysteminterface.h>

#if QT_CONFIG(evdev)
#  include <QtInputSupport/private/qevdevkeyboardmanager_p.h>
#  include <QtInputSupport/private/qevdevmousemanager_p.h>
#  include <QtInputSupport/private/qevdevtouchmanager_p.h>
#endif

#if QT_CONFIG(libinput)
#  include <QtInputSupport/private/qlibinputhandler_p.h>
#endif

#include <QDebug>
#include <QCoreApplication>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

QT_BEGIN_NAMESPACE

#define INTG_DBG  if (qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG") > 0) qDebug() << "[etnaviv2d|integration]"
#define INTG_WARN qWarning() << "[etnaviv2d|integration]"
#define INTG_INFO qInfo()    << "[etnaviv2d|integration]"

/* -----------------------------------------------------------------------
 * Parse plugin parameter string
 * e.g. "etnaviv2d:card=/dev/dri/card0:render=/dev/dri/renderD128:ctx=512"
 * ----------------------------------------------------------------------- */
static QString parseParam(const QStringList &params,
                          const QString &key,
                          const QString &defaultVal)
{
    for (const QString &p : params) {
        if (p.startsWith(key + QLatin1Char('=')))
            return p.mid(key.length() + 1);
    }
    return defaultVal;
}

/* -----------------------------------------------------------------------
 * Construction / two-phase init
 * ----------------------------------------------------------------------- */
QEtnaviv2dIntegration::QEtnaviv2dIntegration(const QStringList &params)
    : m_params(params)
    , m_fontDb(new QGenericUnixFontDatabase)
    , m_services(new QGenericUnixServices)
{
}

bool QEtnaviv2dIntegration::initializeHardware()
{
    QString cardPath   = parseParam(m_params, QStringLiteral("card"),
                                    QStringLiteral("/dev/dri/card0"));
    QString renderPath = parseParam(m_params, QStringLiteral("render"),
                                    QStringLiteral("/dev/dri/renderD128"));
    int ctxOps = parseParam(m_params, QStringLiteral("ctx"),
                             QStringLiteral("512")).toInt();
    if (ctxOps < 64) ctxOps = 512;

    /* Resolve plane mode: arg > QT_ETNAVIV2D_PLANE env > WAYLAND_DISPLAY
     * probe > primary fallback. */
    QString planeStr = parseParam(m_params, QStringLiteral("plane"), QString());
    if (planeStr.isEmpty())
        planeStr = qEnvironmentVariable("QT_ETNAVIV2D_PLANE");
    if (planeStr.isEmpty() && !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty())
        planeStr = QStringLiteral("wayland");
    if (planeStr.isEmpty() || planeStr == QLatin1String("auto"))
        planeStr = QStringLiteral("primary");

    PlaneMode planeMode = PlaneMode::Primary;
    if (planeStr == QLatin1String("overlay"))
        planeMode = PlaneMode::Overlay;
    else if (planeStr == QLatin1String("composite"))
        planeMode = PlaneMode::Composite;
    else if (planeStr == QLatin1String("wayland"))
        planeMode = PlaneMode::Wayland;
    else if (planeStr != QLatin1String("primary"))
        INTG_WARN << "unknown plane=" << planeStr << "— using primary";

    INTG_INFO << "initializing: card=" << cardPath
              << "render=" << renderPath << "ctx_ops=" << ctxOps
              << "plane=" << planeStr;

    /* 1. Open GPU render node. */
    if (etna2d_device_open(renderPath.toLocal8Bit().constData(),
                            &m_etnadev) != ETNA2D_OK || !m_etnadev) {
        INTG_WARN << "etna2d_device_open(" << renderPath << ") failed";
        return false;
    }
    if (etna2d_context_create(m_etnadev, ctxOps, &m_etnaCtx) != ETNA2D_OK
            || !m_etnaCtx) {
        INTG_WARN << "etna2d_context_create failed";
        return false;
    }
    INTG_INFO << "GPU context ready: dev=" << m_etnadev
              << "ctx=" << m_etnaCtx;

    /* 2. Publish context so EtnaBlittablePlatformPixmap can allocate BOs. */
    EtnaBlittablePlatformPixmap::setGlobalContext(m_etnaCtx, m_etnadev);

    /* 3. Create screen (opens card0 for KMS modes, allocates scanout BOs). */
    m_screen = new QEtnaviv2dScreen(cardPath, planeMode, m_etnaCtx, m_etnadev);
    if (!m_screen->initialize()) {
        INTG_WARN << "QEtnaviv2dScreen::initialize() failed";
        delete m_screen;
        m_screen = nullptr;
        return false;
    }

    /* 3b. In wayland mode: create and connect the Wayland transport.
     *     The transport opens a separate wl_display connection and drives the
     *     frame lifecycle.  The screen's KMS path stays dormant (isStub()).
     *     We need the card fd from the screen to allocate dumb BOs for the ring. */
    if (planeMode == PlaneMode::Wayland) {
        /* Open a DRM fd for BO allocation (transport doesn't hold DRM master) */
        int ringCardFd = open(cardPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
        if (ringCardFd < 0) {
            INTG_WARN << "wayland: open(" << cardPath << ") for ring alloc:" << strerror(errno);
            delete m_screen; m_screen = nullptr;
            return false;
        }

        m_waylandTransport = new QEtnaviv2dWaylandTransport(m_etnadev, ringCardFd);
        if (!m_waylandTransport->connect()) {
            INTG_WARN << "QEtnaviv2dWaylandTransport::connect() failed";
            close(ringCardFd);
            delete m_waylandTransport; m_waylandTransport = nullptr;
            delete m_screen; m_screen = nullptr;
            return false;
        }

        /* Update screen geometry to match what Weston reported */
        QRect geo = m_waylandTransport->outputGeometry();
        m_screen->setWaylandGeometry(geo);

        if (!m_waylandTransport->allocateRing()) {
            INTG_WARN << "QEtnaviv2dWaylandTransport::allocateRing() failed";
            m_waylandTransport->disconnect();
            delete m_waylandTransport; m_waylandTransport = nullptr;
            close(ringCardFd);
            delete m_screen; m_screen = nullptr;
            return false;
        }

        INTG_INFO << "Wayland transport ready:"
                  << geo.width() << "x" << geo.height();
    }

    /* 4. Register screen. */
    QWindowSystemInterface::handleScreenAdded(m_screen);
    INTG_INFO << "screen registered:"
              << m_screen->geometry().width() << "x"
              << m_screen->geometry().height();

    /* 5. Input handlers and VT.
     *
     * In wayland mode the QPA must NOT open /dev/input directly — Weston is
     * the libinput master (it owns touchscreen calibration etc).  Input will
     * arrive over wl_seat instead. Skip evdev/libinput entirely here. */
    m_vtHandler.reset(new QFbVtHandler);

    if (planeMode != PlaneMode::Wayland &&
        !qEnvironmentVariableIntValue("QT_QPA_ETNAVIV2D_DISABLE_INPUT"))
        createInputHandlers();

    /* 6. Wire SIGINT/SIGTERM to a clean Qt shutdown so Ctrl-C works even
     *    when the VT handler has the terminal in raw mode. */
    struct sigaction sa{};
    sa.sa_handler = [](int) { QCoreApplication::quit(); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* do NOT use SA_RESTART — we want select() to return EINTR */
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    return true;
}

QEtnaviv2dIntegration::~QEtnaviv2dIntegration()
{
    if (m_waylandTransport) {
        m_waylandTransport->disconnect();
        delete m_waylandTransport;
        m_waylandTransport = nullptr;
    }

    if (m_screen)
        QWindowSystemInterface::handleScreenRemoved(m_screen);
    /* m_screen deleted by Qt after handleScreenRemoved */

    if (m_etnaCtx) {
        etna2d_context_destroy(m_etnaCtx);
        m_etnaCtx = nullptr;
    }
    if (m_etnadev) {
        etna2d_device_close(m_etnadev);
        m_etnadev = nullptr;
    }
}

/* -----------------------------------------------------------------------
 * QPlatformIntegration::initialize() — called by Qt after construction.
 * Hardware init is already done in initializeHardware() (called by factory).
 * This is where Qt expects input context setup.
 * ----------------------------------------------------------------------- */
void QEtnaviv2dIntegration::initialize()
{
    /* Input context (on-screen keyboard) — must be created after Qt is ready. */
    m_inputContext = QPlatformInputContextFactory::create();
}

/* -----------------------------------------------------------------------
 * Capabilities
 * ----------------------------------------------------------------------- */
bool QEtnaviv2dIntegration::hasCapability(Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps:   return true;  /* blittables are GPU-backed but lock() serializes */
    case WindowManagement:  return false; /* single fullscreen window */
    case MultipleWindows:   return false;
    case OpenGL:            return false;
    default:                return QPlatformIntegration::hasCapability(cap);
    }
}

/* -----------------------------------------------------------------------
 * Factory methods
 * ----------------------------------------------------------------------- */
QPlatformPixmap *QEtnaviv2dIntegration::createPlatformPixmap(
        QPlatformPixmap::PixelType type) const
{
    if (type == QPlatformPixmap::BitmapType)
        return new QRasterPlatformPixmap(type); /* monochrome bitmaps: CPU */
    return new EtnaBlittablePlatformPixmap();   /* colour pixmaps: GPU */
}

QPlatformBackingStore *QEtnaviv2dIntegration::createPlatformBackingStore(
        QWindow *window) const
{
    return new QEtnaviv2dBackingStore(window, m_screen, m_waylandTransport);
}

QPlatformWindow *QEtnaviv2dIntegration::createPlatformWindow(
        QWindow *window) const
{
    /* Hand the first top-level window to the transport so it
     * can target QWindowSystemInterface input events at it. */
    if (m_waylandTransport && !m_waylandTransport->window())
        m_waylandTransport->setWindow(window);

    return new QFbWindow(window); /* QFbScreen manages window stack */
}

QAbstractEventDispatcher *QEtnaviv2dIntegration::createEventDispatcher() const
{
    return createUnixEventDispatcher();
}

/* -----------------------------------------------------------------------
 * Accessors
 * ----------------------------------------------------------------------- */
QPlatformFontDatabase *QEtnaviv2dIntegration::fontDatabase() const
{
    return m_fontDb.data();
}

QPlatformServices *QEtnaviv2dIntegration::services() const
{
    return m_services.data();
}

QPlatformNativeInterface *QEtnaviv2dIntegration::nativeInterface() const
{
    return const_cast<QEtnaviv2dIntegration *>(this);
}

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */
void QEtnaviv2dIntegration::createInputHandlers()
{
#if QT_CONFIG(libinput)
    if (!qEnvironmentVariableIntValue("QT_QPA_ETNAVIV2D_NO_LIBINPUT")) {
        new QLibInputHandler(QLatin1String("libinput"), QString());
        return;
    }
#endif

#if QT_CONFIG(evdev)
    new QEvdevKeyboardManager(QLatin1String("EvdevKeyboard"), QString(), this);
    new QEvdevMouseManager(QLatin1String("EvdevMouse"), QString(), this);
    new QEvdevTouchManager(QLatin1String("EvdevTouch"), QString(), this);
#endif
}

QT_END_NAMESPACE
