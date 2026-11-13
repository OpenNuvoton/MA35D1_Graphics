/*
 * qetnaviv2dconfig.h — every runtime switch of the plugin, read once.
 *
 * Behaviour switches (default = the recommended behaviour, a switch only turns it off or A/Bs it):
 *   QT_ETNA2D_FMT          storage / scanout / ring pixel format (format.h, read there)
 *   QT_ETNA2D_CACHED=1     cacheable BOs instead of write-combined (opt-in)
 *   QT_ETNA2D_GPU_TEXT=0   glyphs on the CPU raster engine instead of the DE
 *   QT_ETNA2D_FULL_FLUSH=1 blit the whole frame on flush (no damage tracking)
 *   QT_ETNAVIV2D_NO_BATCH=1  finish the GPU on every source unlock (no batching)
 *   QT_IVI_SURFACE_ID, QT_ETNAVIV2D_PLANE   read where used (integration / transport)
 * Debug (no effect on what is drawn):
 *   QT_ETNAVIV2D_DEBUG=1   log; =2 also per-second timing/counter report
 * Test hooks:
 *   QT_ETNA2D_VERIFY_FLUSH=1  compare every flushed target with the backing store
 */
#ifndef QETNAVIV2DCONFIG_H
#define QETNAVIV2DCONFIG_H

#include <QByteArray>
#include <QDebug>
#include <QSize>
#include <QtGlobal>

struct EtnaConfig {
    int   debug;
    bool  cached, gpuText, fullFlush, noBatch;
    bool  verifyFlush;
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
        k.noBatch     = on("QT_ETNAVIV2D_NO_BATCH");
        k.verifyFlush = on("QT_ETNA2D_VERIFY_FLUSH");
        return k;
    }();
    return c;
}

/* One logging scheme for all files: ETNA_LOG_DBG("screen") << ...  (debug >= 1) */
#define ETNA_LOG_DBG(tag)  if (etnaConfig().debug > 0) qDebug() << "[etnaviv2d|" tag "]"
#define ETNA_LOG_WARN(tag) qWarning() << "[etnaviv2d|" tag "]"
#define ETNA_LOG_INFO(tag) qInfo() << "[etnaviv2d|" tag "]"

#endif
