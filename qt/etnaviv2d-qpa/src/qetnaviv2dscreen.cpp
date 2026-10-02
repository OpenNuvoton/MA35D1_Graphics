/*
 * qetnaviv2dscreen.cpp — KMS display screen implementation.
 *
 * KMS operations are serialised by m_flipMutex so pageFlip() is safe to call
 * from a render thread (QT_THREADED_PAINTING=1) as well as the GUI thread.
 *
 * Initialization sequence (atomic path, default):
 *  1. Open card node, set DRM_CLIENT_CAP_ATOMIC + UNIVERSAL_PLANES.
 *  2. Enumerate connector → CRTC → plane (by type: primary or overlay).
 *  3. Resolve all atomic property IDs by name per object.
 *  4. Allocate two dumb BOs → drmModeAddFB2 → PRIME-export → etna2d import.
 *  5. Initial atomic modeset:
 *       - Primary plane: ALLOW_MODESET commit (sets CRTC ACTIVE, MODE_ID,
 *         connector CRTC_ID, plane FB_ID + geometry).  Requires DRM master.
 *       - Overlay plane: plane-only commit (FB_ID + geometry, flags=0).
 *         The CRTC is already active; no master needed.
 *       - Composite: ALLOW_MODESET with primary ARGB placeholder, then
 *         overlay added as plane-only in the same commit.
 *  6. Fill QFbScreen geometry fields.
 *
 * Page-flip sequence (non-composite):
 *  1. etna2d_finish() already called by the backing store.
 *  2. drmModeAtomicCommit(FB_ID only, NONBLOCK | PAGE_FLIP_EVENT).
 *  3. select() + drmHandleEvent() drain loop until flip_pending clears.
 *  4. Swap front/back indices.
 *
 * Page-flip sequence (composite):
 *  1. Build one drmModeAtomicReq with overlay FB_ID (back buffer) AND
 *     staged primary FB_ID (last-write-wins, from attachPrimaryFb()).
 *  2. One PAGE_FLIP_EVENT drains both: overlay swaps, primary on_screen
 *     bit cleared via release callback.
 *
 * Cleanup: destroy the mode blob; free BOs and close the fd.  No teardown
 * commit is issued — the kernel releases all KMS state on fd close.
 */
#include "qetnaviv2dscreen.h"

#include <QDebug>
#include <QCoreApplication>
#include <qpa/qwindowsysteminterface.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <cstring>

#include <QMutexLocker>

#include <drm_fourcc.h>

QT_BEGIN_NAMESPACE

/* -----------------------------------------------------------------------
 * Logging
 * ----------------------------------------------------------------------- */
static bool screenDebug()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
    return v > 0;
}

#define SCR_DBG  if (screenDebug()) qDebug() << "[etnaviv2d|screen]"
#define SCR_WARN qWarning() << "[etnaviv2d|screen]"
#define SCR_INFO qInfo()    << "[etnaviv2d|screen]"

/* -----------------------------------------------------------------------
 * Vblank / page-flip event handler
 * ----------------------------------------------------------------------- */
static void flipHandler(int /*fd*/, unsigned int /*seq*/,
                        unsigned int /*tvSec*/, unsigned int /*tvUsec*/,
                        void *user)
{
    *static_cast<volatile int *>(user) = 0;
}

/* -----------------------------------------------------------------------
 * Helper: find a DRM plane of a given type for m_crtcIndex.
 * Returns the plane id, or 0 on failure.
 * ----------------------------------------------------------------------- */
uint32_t QEtnaviv2dScreen::findPlaneByType(uint32_t planeType)
{
    uint32_t found = 0;
    drmModePlaneRes *pres = drmModeGetPlaneResources(m_kfd);
    if (!pres)
        return 0;
    for (uint32_t i = 0; i < pres->count_planes && found == 0; ++i) {
        drmModePlane *pl = drmModeGetPlane(m_kfd, pres->planes[i]);
        if (!pl)
            continue;
        if (pl->possible_crtcs & (1u << m_crtcIndex)) {
            uint32_t tid = propId(m_kfd, pl->plane_id,
                                  DRM_MODE_OBJECT_PLANE, "type");
            if (tid) {
                drmModeObjectProperties *pp = drmModeObjectGetProperties(
                    m_kfd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
                if (pp) {
                    for (uint32_t k = 0; k < pp->count_props; ++k) {
                        if (pp->props[k] == tid &&
                            pp->prop_values[k] == planeType) {
                            found = pl->plane_id;
                            break;
                        }
                    }
                    drmModeFreeObjectProperties(pp);
                }
            }
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    return found;
}

/* -----------------------------------------------------------------------
 * Helper: resolve all atomic plane property IDs into dst.
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::resolvePlaneProps(uint32_t planeId, EtnaPlaneProps &dst)
{
    dst.fb_id   = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "FB_ID");
    dst.crtc_id = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    dst.src_x   = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "SRC_X");
    dst.src_y   = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    dst.src_w   = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "SRC_W");
    dst.src_h   = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "SRC_H");
    dst.crtc_x  = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    dst.crtc_y  = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    dst.crtc_w  = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    dst.crtc_h  = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    dst.zpos    = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "zpos");
    dst.blend_mode        = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "porter-duff-blend-mode");
    dst.source_alpha_mode = propId(m_kfd, planeId, DRM_MODE_OBJECT_PLANE, "source-alpha-mode");
}

/* -----------------------------------------------------------------------
 * propId() — resolve a property ID by name on a DRM object.
 *
 * DRM core assigns property IDs per object, so the same named property has
 * different IDs on different planes.  Never hardcode; always resolve by name.
 * Returns 0 if the object does not expose the property (caller must check).
 * ----------------------------------------------------------------------- */
uint32_t QEtnaviv2dScreen::propId(int fd, uint32_t objId, uint32_t objType,
                                  const char *name)
{
    uint32_t id = 0;
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, objId, objType);
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && id == 0; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, name) == 0)
            id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* -----------------------------------------------------------------------
 * QEtnaviv2dScreen — construction / destruction
 * ----------------------------------------------------------------------- */
QEtnaviv2dScreen::QEtnaviv2dScreen(const QString &cardPath,
                                   PlaneMode      planeMode,
                                   etna2d_context *etnaCtx,
                                   etna2d_device  *etnaDev,
                                   int             sharedKfd)
    : QFbScreen()
    , m_cardPath(cardPath)
    , m_planeMode(planeMode)
    , m_etnaCtx(etnaCtx)
    , m_etnadev(etnaDev)
    , m_ownsKfd(sharedKfd < 0)
{
    if (sharedKfd >= 0)
        m_kfd = sharedKfd;
    for (auto &fb : m_fb) fb = EtnaKmsFb{};
    memset(&m_modeInfo, 0, sizeof(m_modeInfo));
    m_dmabuf[0] = m_dmabuf[1] = -1;
    m_scanoutSurf[0] = m_scanoutSurf[1] = nullptr;
}

QEtnaviv2dScreen::~QEtnaviv2dScreen()
{
    cleanup();
}

/* -----------------------------------------------------------------------
 * initialize() — open KMS, allocate BOs, modeset
 * ----------------------------------------------------------------------- */
bool QEtnaviv2dScreen::initialize()
{
    /* --- Wayland handoff stub: no DRM master, no modeset.
     *     The QPA's direct-KMS transport is disabled; the Wayland client
     *     path switches only this transport, leaving libetna2d/
     *     EtnaBlittable untouched. --- */
    if (m_planeMode == PlaneMode::Wayland) {
        SCR_INFO << "plane=wayland: handoff stub — no KMS ops";
        mGeometry     = QRect(0, 0, 1920, 1080); /* nominal until Weston provides geometry */
        mDepth        = 32;
        mFormat       = QImage::Format_ARGB32_Premultiplied;
        mPhysicalSize = QSizeF(527, 296);         /* nominal 96 dpi for 1920x1080 */
        return true;
    }

    /* --- 1. Open card0 (skip if a shared fd was provided at construction) --- */
    if (m_ownsKfd) {
        m_kfd = open(m_cardPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
        if (m_kfd < 0) {
            SCR_WARN << "open(" << m_cardPath << "):" << strerror(errno);
            return false;
        }
    }
    drmSetClientCap(m_kfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
#ifndef ETNAVIV2D_LEGACY_KMS
    if (drmSetClientCap(m_kfd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        SCR_WARN << "DRM_CLIENT_CAP_ATOMIC:" << strerror(errno);
        cleanup();
        return false;
    }
#endif

    /* --- 2. Enumerate resources, pick connector and CRTC --- */
    drmModeRes *res = drmModeGetResources(m_kfd);
    if (!res) {
        SCR_WARN << "drmModeGetResources failed";
        cleanup();
        return false;
    }

    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; ++i) {
        conn = drmModeGetConnector(m_kfd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            break;
        if (conn) { drmModeFreeConnector(conn); conn = nullptr; }
    }
    if (!conn) {
        SCR_WARN << "no connected connector with modes";
        drmModeFreeResources(res);
        cleanup();
        return false;
    }
    m_connId = conn->connector_id;

    /* Preferred mode, else first (highest) mode. */
    m_modeInfo = conn->modes[0];
    for (int i = 0; i < conn->count_modes; ++i) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            m_modeInfo = conn->modes[i];
            break;
        }
    }

    /* Read physical size while we still hold the connector. */
    int mmW = conn->mmWidth;
    int mmH = conn->mmHeight;

    /* Find a CRTC for this connector. */
    for (int i = 0; i < conn->count_encoders && m_crtcId == 0; ++i) {
        drmModeEncoder *enc = drmModeGetEncoder(m_kfd, conn->encoders[i]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs; ++c) {
            if (enc->possible_crtcs & (1u << c)) {
                m_crtcId    = res->crtcs[c];
                m_crtcIndex = c;
                break;
            }
        }
        drmModeFreeEncoder(enc);
    }
    if (m_crtcId == 0) {
        SCR_WARN << "no usable CRTC for connector" << m_connId;
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        cleanup();
        return false;
    }
#ifdef ETNAVIV2D_LEGACY_KMS
    m_savedCrtc = drmModeGetCrtc(m_kfd, m_crtcId);
#endif
    SCR_INFO << "KMS: conn=" << m_connId << "crtc=" << m_crtcId
             << "mode=" << m_modeInfo.name
             << m_modeInfo.hdisplay << "x" << m_modeInfo.vdisplay
             << "@" << m_modeInfo.vrefresh << "Hz";

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    int w = m_modeInfo.hdisplay;
    int h = m_modeInfo.vdisplay;

#ifndef ETNAVIV2D_LEGACY_KMS
    /* --- 2b. Atomic: pick plane(s) based on m_planeMode --- */
    if (m_crtcIndex < 0) {
        SCR_WARN << "no CRTC index captured";
        cleanup();
        return false;
    }

    /* Resolve CRTC and connector props by name before checking planes. */
    m_crtcProps.active  = propId(m_kfd, m_crtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    m_crtcProps.mode_id = propId(m_kfd, m_crtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    m_connProps.crtc_id = propId(m_kfd, m_connId, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    if (m_planeMode == PlaneMode::Composite) {
        /* Composite: enumerate BOTH primary and overlay planes. */
        m_primaryPlaneId = findPlaneByType(DRM_PLANE_TYPE_PRIMARY);
        m_planeId        = findPlaneByType(DRM_PLANE_TYPE_OVERLAY);
        if (!m_primaryPlaneId || !m_planeId) {
            SCR_WARN << "composite: primary plane=" << m_primaryPlaneId
                     << "overlay plane=" << m_planeId;
            cleanup();
            return false;
        }
        resolvePlaneProps(m_primaryPlaneId, m_primaryPlaneProps);
        resolvePlaneProps(m_planeId,        m_planeProps);
        SCR_INFO << "Composite: primary plane=" << m_primaryPlaneId
                 << "overlay plane=" << m_planeId;
    } else {
        const uint32_t targetType = (m_planeMode == PlaneMode::Overlay)
                                    ? DRM_PLANE_TYPE_OVERLAY
                                    : DRM_PLANE_TYPE_PRIMARY;
        m_planeId = findPlaneByType(targetType);
        if (m_planeId == 0) {
            SCR_WARN << "no" << (targetType == DRM_PLANE_TYPE_OVERLAY ? "OVERLAY" : "PRIMARY")
                     << "plane for CRTC index" << m_crtcIndex;
            cleanup();
            return false;
        }
        resolvePlaneProps(m_planeId, m_planeProps);
        /* Overlay-only mode: also resolve the primary plane so we can add a
         * black placeholder when bringing the CRTC up from a cold/torn-down
         * state.  Most DRM drivers require the primary plane to have a valid
         * FB when ACTIVE is set to 1.  m_primaryPlaneId stays 0 if there is
         * no primary (unusual), in which case we skip the placeholder. */
        if (m_planeMode == PlaneMode::Overlay) {
            m_primaryPlaneId = findPlaneByType(DRM_PLANE_TYPE_PRIMARY);
            if (m_primaryPlaneId)
                resolvePlaneProps(m_primaryPlaneId, m_primaryPlaneProps);
        }
    }

    if (!m_planeProps.fb_id || !m_planeProps.crtc_id ||
        !m_crtcProps.active || !m_crtcProps.mode_id || !m_connProps.crtc_id) {
        SCR_WARN << "required atomic properties missing"
                 << "(plane.fb_id" << m_planeProps.fb_id
                 << "plane.crtc_id" << m_planeProps.crtc_id
                 << "crtc.active" << m_crtcProps.active
                 << "crtc.mode_id" << m_crtcProps.mode_id
                 << "conn.crtc_id" << m_connProps.crtc_id << ")";
        cleanup();
        return false;
    }

    if (m_planeMode == PlaneMode::Composite &&
        (!m_primaryPlaneProps.fb_id || !m_primaryPlaneProps.crtc_id)) {
        SCR_WARN << "composite: primary plane props missing";
        cleanup();
        return false;
    }

    if (drmModeCreatePropertyBlob(m_kfd, &m_modeInfo, sizeof(m_modeInfo),
                                  &m_modeBlobId)) {
        SCR_WARN << "drmModeCreatePropertyBlob:" << strerror(errno);
        cleanup();
        return false;
    }

    SCR_INFO << "Atomic: plane=" << m_planeId
             << (m_planeMode == PlaneMode::Overlay   ? "(overlay)"
              : m_planeMode == PlaneMode::Composite  ? "(overlay/composite)"
              :                                        "(primary)")
             << "props resolved; mode blob=" << m_modeBlobId;
#endif // !ETNAVIV2D_LEGACY_KMS

    /* --- 3. Allocate dumb BOs and DRM FBs --- */
    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        struct drm_mode_create_dumb creq = {};
        creq.width  = w;
        creq.height = h;
        creq.bpp    = 32;
        if (drmIoctl(m_kfd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
            SCR_WARN << "CREATE_DUMB[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        m_fb[i].handle = creq.handle;
        m_fb[i].stride = creq.pitch;
        m_fb[i].size   = creq.size;
        m_fb[i].width  = w;
        m_fb[i].height = h;

        uint32_t handles[4] = {creq.handle};
        uint32_t pitches[4] = {creq.pitch};
        uint32_t offsets[4] = {};
        uint32_t fmt = (m_planeMode == PlaneMode::Overlay ||
                        m_planeMode == PlaneMode::Composite)
                       ? DRM_FORMAT_ARGB8888
                       : DRM_FORMAT_XRGB8888;
        if (drmModeAddFB2(m_kfd, w, h, fmt,
                          handles, pitches, offsets, &m_fb[i].fb_id, 0)) {
            SCR_WARN << "drmModeAddFB2[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        SCR_DBG << "FB[" << i << "] id=" << m_fb[i].fb_id
                << "handle=" << creq.handle
                << "stride=" << creq.pitch;
    }

    /* --- 4. PRIME export + etnaviv import --- */
    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        if (drmPrimeHandleToFD(m_kfd, m_fb[i].handle,
                               DRM_CLOEXEC | DRM_RDWR, &m_dmabuf[i])) {
            SCR_WARN << "drmPrimeHandleToFD[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        int rc = etna2d_surface_from_dmabuf(m_etnadev, m_dmabuf[i],
                                            w, h, m_fb[i].stride,
                                            &m_scanoutSurf[i]);
        if (rc != ETNA2D_OK || !m_scanoutSurf[i]) {
            SCR_WARN << "etna2d_surface_from_dmabuf[" << i << "] rc=" << rc;
            cleanup();
            return false;
        }
        SCR_INFO << "Scanout[" << i << "] imported into etnaviv"
                 << "dmabuf=" << m_dmabuf[i];
    }

    /* --- 5. Initial modeset with the front buffer (fill black first) --- */
    etna2d_fill(m_etnaCtx, m_scanoutSurf[m_frontIdx], 0, 0, w, h, 0xff000000u);
    etna2d_finish(m_etnaCtx);

#ifdef ETNAVIV2D_LEGACY_KMS
    if (drmModeSetCrtc(m_kfd, m_crtcId, m_fb[m_frontIdx].fb_id, 0, 0,
                       &m_connId, 1, &m_modeInfo)) {
        SCR_WARN << "drmModeSetCrtc:" << strerror(errno);
        cleanup();
        return false;
    }
#else
    {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req) {
            SCR_WARN << "drmModeAtomicAlloc (modeset) failed";
            cleanup();
            return false;
        }
        /* ALLOW_MODESET is needed only when we are the one bringing the CRTC
         * up: setting ACTIVE, MODE_ID, and connector CRTC_ID.  This is
         * required when the CRTC is not yet active (first opener, or after a
         * full teardown).  When sharing an fd with an already-active CRTC
         * (e.g. overlay added after primary) we skip these properties and
         * commit with flags=0 — a plane-only commit that does not require
         * master privilege beyond holding the shared fd. */
        uint32_t flags = 0;
        bool crtcAlreadyActive = false;
        {
            drmModeCrtc *crtc = drmModeGetCrtc(m_kfd, m_crtcId);
            if (crtc) {
                crtcAlreadyActive = crtc->mode_valid;
                drmModeFreeCrtc(crtc);
            }
        }
        if (!crtcAlreadyActive) {
            drmModeAtomicAddProperty(req, m_crtcId, m_crtcProps.mode_id, m_modeBlobId);
            drmModeAtomicAddProperty(req, m_crtcId, m_crtcProps.active,  1);
            drmModeAtomicAddProperty(req, m_connId, m_connProps.crtc_id, m_crtcId);
            flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
        }
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.fb_id,   m_fb[m_frontIdx].fb_id);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_id, m_crtcId);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_x,  0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_y,  0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_w,  (uint64_t)w << 16);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_h,  (uint64_t)h << 16);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_x, 0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_y, 0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_w, w);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_h, h);
        /* Overlay blend props: set once at modeset, persist for all flips.
         * porter-duff-blend-mode=3 (SRC_OVER), source-alpha-mode=0 (None = per-pixel). */
        if (m_planeMode == PlaneMode::Overlay || m_planeMode == PlaneMode::Composite) {
            if (m_planeProps.blend_mode)
                drmModeAtomicAddProperty(req, m_planeId,
                                         m_planeProps.blend_mode, 3);
            if (m_planeProps.source_alpha_mode)
                drmModeAtomicAddProperty(req, m_planeId,
                                         m_planeProps.source_alpha_mode, 0);
        }
        /* Composite: also set up the primary plane with a black ARGB placeholder.
         * The VideoThread will call attachPrimaryFb() to replace this with NV12
         * once it has allocated its BO ring. */
        if (m_planeMode == PlaneMode::Composite) {
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.fb_id,   m_fb[m_frontIdx].fb_id);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_id, m_crtcId);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_x,  0);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_y,  0);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_w,  (uint64_t)w << 16);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_h,  (uint64_t)h << 16);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_x, 0);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_y, 0);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_w, w);
            drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_h, h);
            flags |= DRM_MODE_ATOMIC_ALLOW_MODESET; /* primary needs modeset too */
        }
        /* Overlay-only mode: add a black ARGB placeholder on the primary plane
         * whenever the primary is unattached (fb=0 / crtc=null), regardless of
         * whether the CRTC was already active.  This covers two cases:
         *   (a) CRTC inactive (cold start): primary placeholder + ACTIVE=1
         *   (b) CRTC active but primary detached (e.g. Weston exit leaves
         *       planes with crtc=null): plane-only re-attachment of primary.
         * Without a valid primary FB the MA35D1 display engine does not
         * scanout the overlay even if the overlay plane itself is set up. */
        if (m_planeMode == PlaneMode::Overlay
                && m_primaryPlaneId && m_primaryPlaneProps.fb_id) {
            /* Check whether primary is currently attached.  If its CRTC_ID
             * prop is 0 we must (re-)attach it, which requires ALLOW_MODESET. */
            bool primaryDetached = true;
            {
                drmModeObjectProperties *pp = drmModeObjectGetProperties(
                        m_kfd, m_primaryPlaneId, DRM_MODE_OBJECT_PLANE);
                if (pp) {
                    for (uint32_t i = 0; i < pp->count_props; ++i) {
                        if (pp->props[i] == m_primaryPlaneProps.crtc_id) {
                            primaryDetached = (pp->prop_values[i] == 0);
                            break;
                        }
                    }
                    drmModeFreeObjectProperties(pp);
                }
            }
            if (primaryDetached) {
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.fb_id,   m_fb[m_frontIdx].fb_id);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_id, m_crtcId);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_x,  0);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_y,  0);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_w,  (uint64_t)w << 16);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.src_h,  (uint64_t)h << 16);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_x, 0);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_y, 0);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_w, w);
                drmModeAtomicAddProperty(req, m_primaryPlaneId, m_primaryPlaneProps.crtc_h, h);
                flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
                SCR_INFO << "Overlay: primary plane" << m_primaryPlaneId
                         << "detached — seeding black ARGB placeholder (ALLOW_MODESET)";
            }
        }
        int rc = drmModeAtomicCommit(m_kfd, req, flags, nullptr);
        drmModeAtomicFree(req);
        if (rc) {
            SCR_WARN << "atomic modeset commit:" << strerror(errno);
            cleanup();
            return false;
        }
    }
#endif
    SCR_INFO << "Initial modeset OK — screen" << w << "x" << h;

    /* --- 6. Fill QFbScreen fields --- */
    mGeometry    = QRect(0, 0, w, h);
    mDepth       = 32;
    mFormat      = QImage::Format_ARGB32_Premultiplied;

    /* Physical size — read from connector above; fall back to 96 dpi. */
    if (mmW <= 0 || mmH <= 0) {
        mmW = w * 25.4 / 96.0;
        mmH = h * 25.4 / 96.0;
    }
    mPhysicalSize = QSizeF(mmW, mmH);

    return true;
}

/* -----------------------------------------------------------------------
 * setWaylandGeometry() — update screen geometry from Wayland configure
 * Called from QEtnaviv2dIntegration after the xdg_surface configure roundtrip.
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::setWaylandGeometry(const QRect &geo)
{
    if (!isStub() || geo.isEmpty())
        return;

    mGeometry     = geo;
    mPhysicalSize = QSizeF(geo.width() * 25.4 / 96.0,
                           geo.height() * 25.4 / 96.0);

    QWindowSystemInterface::handleScreenGeometryChange(this->screen(),
                                                       mGeometry, mGeometry);
    SCR_INFO << "Wayland geometry updated to" << mGeometry;
}

/* -----------------------------------------------------------------------
 * doRedraw() — called on the GUI thread by QFbScreen::event(UpdateRequest).
 *
 * We suppress the QFbScreen software compositor (return empty region), but
 * in composite mode we use this path as the video-present trigger:
 * when attachPrimaryFb() staged a new frame and the UI is static (no pending
 * flush()), scheduleUpdate() delivers an UpdateRequest here, and we call
 * pageFlip() to commit the staged primary FB.
 *
 * If flush() runs first (UI repaint arrived), it calls pageFlip() and clears
 * m_videoPresent — doRedraw() then skips to avoid a double-flip.
 * ----------------------------------------------------------------------- */
QRegion QEtnaviv2dScreen::doRedraw()
{
    if (m_planeMode == PlaneMode::Composite) {
        bool doFlip = false;
        {
            QMutexLocker lock(&m_flipMutex);
            if (m_videoPresent) {
                m_videoPresent = false;
                doFlip = true;
            }
        }
        if (doFlip) {
            SCR_DBG << "doRedraw triggering pageFlip (video-present path)";
            pageFlip(/*videoOnly=*/true);
        }
    }
    return QRegion();
}

/* -----------------------------------------------------------------------
 * pageFlip() — flip back buffer to display, block for vblank.
 *
 * Serialised by m_flipMutex so render-thread and GUI-thread flushes cannot
 * race.  Returns immediately with -1 if teardown is already in progress
 * (m_shuttingDown), avoiding an EBUSY from a late Ctrl-C flush.
 * ----------------------------------------------------------------------- */
int QEtnaviv2dScreen::pageFlip(bool videoOnly)
{
    QMutexLocker lock(&m_flipMutex);

    if (m_shuttingDown)
        return -1;

    /* Clear the video-present flag: whichever path reaches pageFlip()
     * first (flush() from a UI repaint, or doRedraw() from scheduleUpdate())
     * owns this commit.  The other will see m_videoPresent=false in doRedraw()
     * and skip, preventing a double-flip. */
    m_videoPresent = false;

    m_flipPending = 1;
#ifdef ETNAVIV2D_LEGACY_KMS
    if (drmModePageFlip(m_kfd, m_crtcId, m_fb[m_backIdx].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT,
                        static_cast<void *>(const_cast<int *>(&m_flipPending)))) {
        SCR_WARN << "drmModePageFlip:" << strerror(errno);
        m_flipPending = 0;
        return -1;
    }
#else
    {
        /* Geometry is fixed after the initial modeset; only FB_ID changes
         * per flip.  Non-blocking, event-driven — the drain loop below waits
         * on the same DRM_MODE_PAGE_FLIP_EVENT the legacy path used. */
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req) {
            SCR_WARN << "drmModeAtomicAlloc (flip) failed";
            m_flipPending = 0;
            return -1;
        }

        if (m_planeMode == PlaneMode::Composite) {
            /* Overlay (UI): video-only path re-commits the front buffer (the
             * one currently on screen) so the UI stays frozen without flutter.
             * UI path commits the back buffer (just painted by the backing store). */
            int overlayIdx = videoOnly ? m_frontIdx : m_backIdx;
            drmModeAtomicAddProperty(req, m_planeId, m_planeProps.fb_id,
                                     m_fb[overlayIdx].fb_id);
            /* Primary (video) flip: advance staged → on-screen.
             * Snapshot the buffer that is currently on-screen BEFORE replacing
             * it — that is the one the flip event will retire, so it is the
             * one the release callback must free. */
            if (m_hasStagedPrimary) {
                m_retiredPrimary   = m_onScreenPrimary; /* what the DC is about to stop scanning */
                m_onScreenPrimary  = m_stagedPrimary;
                m_hasStagedPrimary = false;
                SCR_DBG << "pageFlip retiring fbId=" << m_retiredPrimary.fbId
                        << "committing fbId=" << m_onScreenPrimary.fbId;
            } else {
                /* No new frame — re-committing the same buffer; nothing to retire. */
                m_retiredPrimary = PrimaryFbInfo{};
            }
            if (m_onScreenPrimary.fbId) {
                drmModeAtomicAddProperty(req, m_primaryPlaneId,
                                         m_primaryPlaneProps.fb_id,
                                         m_onScreenPrimary.fbId);
            }
        } else {
            drmModeAtomicAddProperty(req, m_planeId, m_planeProps.fb_id,
                                     m_fb[m_backIdx].fb_id);
        }

        int rc = drmModeAtomicCommit(m_kfd, req,
                     DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                     static_cast<void *>(const_cast<int *>(&m_flipPending)));
        drmModeAtomicFree(req);
        if (rc) {
            SCR_WARN << "atomic flip commit:" << strerror(errno);
            m_flipPending = 0;
            return -1;
        }
    }
#endif

    /* Snapshot the retired primary buffer — the one the DC was scanning before
     * this commit.  After the flip event it is safe to release back to the
     * producer.  m_retiredPrimary was set above before advancing m_onScreenPrimary. */
    PrimaryFbInfo committedPrimary = m_retiredPrimary;

    /* Wait for the flip event via select + drmHandleEvent.
     * The mutex is held throughout (original behaviour) so that other threads
     * see m_flipPending atomically with the lock.  attachPrimaryFb() just
     * stages under the same mutex and returns; it will succeed as soon as
     * this flip completes (within one vblank, ~16 ms). */
    drmEventContext ev = {};
    ev.version           = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = flipHandler;

    while (m_flipPending) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_kfd, &fds);
        int r = select(m_kfd + 1, &fds, nullptr, nullptr, nullptr);
        if (r < 0) {
            if (errno == EINTR) {
                /* Signal received (e.g. SIGINT → QCoreApplication::quit()).
                 * Stop waiting — the flip may never complete if the app is
                 * shutting down, and blocking here would prevent cleanup. */
                m_flipPending = 0;
                return -1;
            }
            SCR_WARN << "select:" << strerror(errno);
            return -1;
        }
        if (FD_ISSET(m_kfd, &fds))
            drmHandleEvent(m_kfd, &ev);
    }

    /* Swap front/back — but only for UI-driven flips where the back buffer
     * was just painted.  Video-only flips re-committed the front buffer;
     * no swap needed (and swapping would corrupt the next UI flip). */
    if (!m_shuttingDown && !videoOnly)
        std::swap(m_frontIdx, m_backIdx);

    /* Fire the primary release callback.  The just-completed commit retired
     * the previous on-screen primary buffer; the producer can now clear its
     * on_screen bit.  We snapshot the callback under the lock then call it
     * after releasing to avoid holding the lock during an unknown callback. */
    std::function<void(const PrimaryFbInfo &)> releaseCb;
    if (m_planeMode == PlaneMode::Composite && committedPrimary.fbId)
        releaseCb = m_primaryReleaseCb;

    lock.unlock();

    if (releaseCb && committedPrimary.fbId)
        releaseCb(committedPrimary);

    SCR_DBG << "pageFlip done — front=" << m_frontIdx
            << "back=" << m_backIdx;
    return 0;
}

/* -----------------------------------------------------------------------
 * cleanup()
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::cleanup()
{
    {
        /* Signal any concurrent pageFlip() to abort, then wait for it to
         * release the mutex before we tear down KMS state under it. */
        QMutexLocker lock(&m_flipMutex);
        m_shuttingDown = true;
    }

#ifdef ETNAVIV2D_LEGACY_KMS
    /* Restore saved CRTC (legacy). */
    if (m_savedCrtc && m_kfd >= 0) {
        drmModeSetCrtc(m_kfd, m_savedCrtc->crtc_id,
                       m_savedCrtc->buffer_id,
                       m_savedCrtc->x, m_savedCrtc->y,
                       &m_connId, 1, &m_savedCrtc->mode);
        drmModeFreeCrtc(m_savedCrtc);
        m_savedCrtc = nullptr;
    }
#else
    /* Atomic path: no teardown commit needed.  The kernel releases all KMS
     * state (plane FBs, CRTC, connector binding) when the DRM fd is closed.
     * Issuing an explicit disable commit here caused EINVAL because
     * detaching the connector while ACTIVE=1 is an invalid atomic transition,
     * and setting ACTIVE=0 triggers atomic_disable → drm_crtc_vblank_off
     * which would break the next session's vsync.  Just let the fd close. */
    if (m_modeBlobId && m_kfd >= 0) {
        drmModeDestroyPropertyBlob(m_kfd, m_modeBlobId);
        m_modeBlobId = 0;
    }
    if (m_savedCrtc) {
        drmModeFreeCrtc(m_savedCrtc);
        m_savedCrtc = nullptr;
    }
#endif

    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        if (m_scanoutSurf[i]) {
            etna2d_surface_destroy(m_scanoutSurf[i]);
            m_scanoutSurf[i] = nullptr;
        }
        if (m_dmabuf[i] >= 0) {
            close(m_dmabuf[i]);
            m_dmabuf[i] = -1;
        }
        if (m_fb[i].fb_id && m_kfd >= 0) {
            drmModeRmFB(m_kfd, m_fb[i].fb_id);
            m_fb[i].fb_id = 0;
        }
        if (m_fb[i].handle && m_kfd >= 0) {
            struct drm_mode_destroy_dumb dreq = { .handle = m_fb[i].handle };
            drmIoctl(m_kfd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
            m_fb[i].handle = 0;
        }
    }

    if (m_kfd >= 0 && m_ownsKfd) {
        close(m_kfd);
        m_kfd = -1;
    }
}

/* -----------------------------------------------------------------------
 * attachPrimaryFb() — stage an externally-produced NV12 FB (composite only)
 *
 * Called from the video producer thread.  Stages info under m_flipMutex
 * (last-write-wins) then posts a coalesced repaint request to the GUI thread
 * so the video advances even when the Qt UI is completely static.
 *
 * The caller must not recycle the buffer until the release callback fires.
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::attachPrimaryFb(const PrimaryFbInfo &info)
{
    {
        QMutexLocker lock(&m_flipMutex);
        if (m_shuttingDown)
            return;
        m_stagedPrimary    = info;
        m_hasStagedPrimary = true;
        m_videoPresent     = true;
    }

    /* Drive a coalesced GUI-thread commit via QFbScreen::scheduleUpdate().
     * scheduleUpdate() posts QEvent::UpdateRequest to *this* (the screen
     * QObject) and is coalesced (only one pending request at a time).  The
     * event is delivered on the GUI thread → QFbScreen::event() → doRedraw()
     * → pageFlip().  If a UI repaint arrives first, flush() calls pageFlip()
     * and clears m_videoPresent; doRedraw() then skips to avoid a double-flip.
     */
    SCR_DBG << "attachPrimaryFb fbId=" << info.fbId
            << "calling scheduleUpdate()";
    scheduleUpdate();
}

/* -----------------------------------------------------------------------
 * setPrimaryReleaseCallback() — register the on-screen buffer release hook
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::setPrimaryReleaseCallback(
        std::function<void(const PrimaryFbInfo &)> cb)
{
    QMutexLocker lock(&m_flipMutex);
    m_primaryReleaseCb = std::move(cb);
}

QT_END_NAMESPACE
