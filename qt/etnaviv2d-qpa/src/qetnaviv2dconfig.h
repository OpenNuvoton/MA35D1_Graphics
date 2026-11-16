/*
 * qetnaviv2dconfig.h — every runtime switch of the plugin, read once.
 *
 * The defaults are the recommended behaviour; a switch only turns a feature off, opts in to an
 * experiment, or selects a value.  "unset" means the variable is not in the environment.
 *
 *  Variable                  Default (unset)          Effect
 *  ------------------------  -----------------------  ---------------------------------------------
 * Behaviour
 *  QT_ETNA2D_FMT             argb8888                 storage / scanout / ring pixel format (format.h)
 *  QT_ETNA2D_CACHED          0  write-combined BOs    1 = cacheable BOs
 *  QT_ETNA2D_GPU_TEXT        1  glyphs on the GPU     0 = glyphs on the CPU raster engine
 *  QT_ETNA2D_FULL_FLUSH      0  damage tracking ON    1 = blit the whole frame on every flush
 *  QT_ETNA2D_SYNC_FLIP       0  event-driven flip     1 = KMS pageFlip() blocks until vblank
 *  QT_ETNAVIV2D_NO_BATCH     0  batching ON           1 = finish the GPU on every source unlock
 * Placement
 *  QT_ETNAVIV2D_PLANE        auto: "wayland" if WAYLAND_DISPLAY is set, else "primary"
 *                            (also primary | overlay | wayland; the platform argument plane=... wins)
 *  QT_IVI_GEOMETRY=WxH       unset: size follows window / wl_output; set: pinned (Wayland)
 *  QT_IVI_SURFACE_ID         2000                     ivi-shell surface id (Wayland)
 * Debug (no effect on what is drawn)
 *  QT_ETNAVIV2D_DEBUG        0  off                   1 = log; 2 = log + per-second timing report
 * Test hooks (off unless set)
 *  QT_ETNA2D_VERIFY_FLUSH=1  compare every flushed target with the backing store
 *  QT_ETNA2D_TEST_DROP=N     pretend the target is busy on every Nth flush
 *  QT_ETNA2D_TEST_OUTPUT=WxH Wayland: fake a wl_output mode change 1 s after the first frame
 */
#ifndef QETNAVIV2DCONFIG_H
#define QETNAVIV2DCONFIG_H

#include <QByteArray>
#include <QDebug>
#include <QSize>
#include <QtGlobal>

struct EtnaConfig {
    int   debug;
    bool  cached, gpuText, fullFlush, syncFlip, noBatch, iviPinned;
    bool  verifyFlush;
    int   testDrop;
    QSize testOutput;     /* invalid = off */
};

inline const EtnaConfig &etnaConfig()
{
    static const EtnaConfig c = []() {
        EtnaConfig k;
        auto on  = [](const char *n) { return qEnvironmentVariableIntValue(n) != 0; };
        auto def = [](const char *n) { return qEnvironmentVariableIsEmpty(n) || qEnvironmentVariableIntValue(n) != 0; };
        k.debug       = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
        k.cached      = on("QT_ETNA2D_CACHED");
        k.gpuText     = def("QT_ETNA2D_GPU_TEXT");
        k.fullFlush   = on("QT_ETNA2D_FULL_FLUSH");
        k.syncFlip    = on("QT_ETNA2D_SYNC_FLIP");
        k.noBatch     = on("QT_ETNAVIV2D_NO_BATCH");
        k.iviPinned   = !qEnvironmentVariableIsEmpty("QT_IVI_GEOMETRY");
        k.verifyFlush = on("QT_ETNA2D_VERIFY_FLUSH");
        k.testDrop    = qEnvironmentVariableIntValue("QT_ETNA2D_TEST_DROP");
        const QList<QByteArray> p = qgetenv("QT_ETNA2D_TEST_OUTPUT").split('x');
        k.testOutput  = (p.size() == 2 && p[0].toInt() > 0 && p[1].toInt() > 0)
                            ? QSize(p[0].toInt(), p[1].toInt()) : QSize();
        return k;
    }();
    return c;
}

/* One logging scheme for all files: ETNA_LOG_DBG("screen") << ...  (debug >= 1) */
#define ETNA_LOG_DBG(tag)  if (etnaConfig().debug > 0) qDebug() << "[etnaviv2d|" tag "]"
#define ETNA_LOG_WARN(tag) qWarning() << "[etnaviv2d|" tag "]"
#define ETNA_LOG_INFO(tag) qInfo() << "[etnaviv2d|" tag "]"

#endif
