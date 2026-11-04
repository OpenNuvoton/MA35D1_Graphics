/*
 * qetnaviv2dformat.h — pixel formats the QPA can use for the Wayland ring, the
 * backing store and pixmaps (the formats both the GC520 DE and the display
 * controller support; see etna2d.h ETNA2D_FMT_*).
 *
 *   QT_ETNA2D_FMT       the one setting: format of the backing store, opaque pixmaps and the
 *                       ring (default argb8888 = unchanged behaviour)
 *   QT_ETNA2D_RING_FMT  debug / A-B only: overrides the ring format. A ring that differs from
 *                       the backing store costs a converting copy per frame.
 *
 * Names: argb8888 xrgb8888 rgb565 argb4444 argb1555.  argb1555 has no QImage
 * equivalent, so it is ring-only (backing store / pixmaps fall back to argb8888).
 * Pixmaps with an alpha channel stay ARGB8888 when the chosen format has no alpha.
 */
#ifndef QETNAVIV2DFORMAT_H
#define QETNAVIV2DFORMAT_H

#include <QByteArray>
#include <QImage>
#include <drm_fourcc.h>
extern "C" {
#include "etna2d.h"
}

struct EtnaFmt {
    const char *name;
    uint32_t drm;                  /* DRM_FORMAT_* */
    int etna;                      /* ETNA2D_FMT_* */
    int bpp;
    bool alpha;
    QImage::Format qfmt;           /* Format_Invalid = no CPU-raster equivalent */
};

inline const EtnaFmt *etnaFormats(int *n)
{
    static const EtnaFmt t[] = {
        { "argb8888", DRM_FORMAT_ARGB8888, ETNA2D_FMT_ARGB8888, 32, true,  QImage::Format_ARGB32_Premultiplied },
        { "xrgb8888", DRM_FORMAT_XRGB8888, ETNA2D_FMT_XRGB8888, 32, false, QImage::Format_RGB32 },
        { "rgb565",   DRM_FORMAT_RGB565,   ETNA2D_FMT_RGB565,   16, false, QImage::Format_RGB16 },
        { "argb4444", DRM_FORMAT_ARGB4444, ETNA2D_FMT_ARGB4444, 16, true,  QImage::Format_ARGB4444_Premultiplied },
        { "argb1555", DRM_FORMAT_ARGB1555, ETNA2D_FMT_ARGB1555, 16, true,  QImage::Format_Invalid },
    };
    *n = int(sizeof(t) / sizeof(t[0]));
    return t;
}

inline const EtnaFmt &etnaFmtByName(const QByteArray &name, bool *known = nullptr)
{
    int n; const EtnaFmt *t = etnaFormats(&n);
    for (int i = 0; i < n; ++i)
        if (name.toLower() == t[i].name) { if (known) *known = true; return t[i]; }
    if (known) *known = false;
    return t[0];
}

inline const EtnaFmt &etnaFmtByEtna(int etna)
{
    int n; const EtnaFmt *t = etnaFormats(&n);
    for (int i = 0; i < n; ++i)
        if (t[i].etna == etna) return t[i];
    return t[0];
}

/* Format for the backing store and pixmaps (QT_ETNA2D_FMT); alpha pixmaps need an alpha format. */
inline const EtnaFmt &etnaStorageFmt(bool alpha)
{
    static const EtnaFmt &base = etnaFmtByName(qgetenv("QT_ETNA2D_FMT"));
    if (base.qfmt == QImage::Format_Invalid) return etnaFmtByName("argb8888");
    if (alpha && !base.alpha) return etnaFmtByName("argb8888");
    return base;
}

/* Ring format: QT_ETNA2D_RING_FMT, else QT_ETNA2D_FMT, else argb8888. */
inline const EtnaFmt &etnaRingFmt()
{
    const QByteArray r = qgetenv("QT_ETNA2D_RING_FMT");
    if (!r.isEmpty()) return etnaFmtByName(r);
    return etnaFmtByName(qgetenv("QT_ETNA2D_FMT"));
}

#endif
