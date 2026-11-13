/*
 * qetnaviv2dstats.h — optional per-second cost counters (QT_ETNAVIV2D_DEBUG=2).
 *
 * Measure where time goes before changing anything.  Disabled (one cached bool test per site) unless the env var is >= 2.
 * Output: one "ETNASTATS" line per second on stderr, all times in ms per second.
 */
#ifndef QETNAVIV2DSTATS_H
#define QETNAVIV2DSTATS_H

#include "qetnaviv2dconfig.h"
#include <QtCore/qglobal.h>
#include <atomic>
#include <time.h>
#include <stdio.h>

namespace EtnaStats {

enum Id {
    LockFinish,   /* etna2d_finish() inside doLock() (CPU waits for GPU)   */
    UnlockFinish, /* etna2d_finish() inside doUnlock()                     */
    CpuFill,      /* cpuFillRect (raster on WC memory)                     */
    CpuPixmap,    /* cpuDrawPixmap                                         */
    FlushFinish,  /* etna2d_finish() in backing store flush/scroll         */
    FlushBlit,    /* queueing the backing->scanout/ring blit               */
    FlipWait,     /* pageFlip() blocking wait                              */
    OpaqueScan,   /* isOpaque() pixel scan                                 */
    GpuFill, GpuBlit, GpuStretch, GpuMask,  /* count only (GpuMask = glyph mask blits) */
    BytesCopied,  /* flush copy bytes (count = bytes)                      */
    Frames,       /* flush() calls                                         */
    DroppedFrames,/* ring full, frame dropped                              */
    RelPresent,   /* dropped frame presented from the release hook          */
    N
};

struct Slot { std::atomic<long long> ns{0}; std::atomic<long long> n{0}; };
inline Slot *statSlots() { static Slot s[N]; return s; }
inline bool enabled() { return etnaConfig().debug >= 2; }
inline long long nowNs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }

inline void add(Id i, long long ns = 0, long long n = 1)
{
    Slot &s = statSlots()[i];
    s.ns.fetch_add(ns, std::memory_order_relaxed);
    s.n.fetch_add(n, std::memory_order_relaxed);
}

inline void report()
{
    static std::atomic<long long> last{nowNs()};
    long long t = nowNs(), l = last.load();
    if (t - l < 1000000000LL || !last.compare_exchange_strong(l, t)) return;
    double ms[N]; long long c[N];
    for (int i = 0; i < N; ++i) {
        ms[i] = statSlots()[i].ns.exchange(0) / 1e6;
        c[i]  = statSlots()[i].n.exchange(0);
    }
    fprintf(stderr,
        "ETNASTATS dt=%.2f frames=%lld dropped=%lld relpresent=%lld | ms/s lockfin=%.1f(%lld) unlockfin=%.1f(%lld) "
        "cpufill=%.1f(%lld) cpupix=%.1f(%lld) flushfin=%.1f(%lld) flushblit=%.1f(%lld) copyMB=%.1f "
        "flipwait=%.1f(%lld) opaquescan=%.1f(%lld) | gpuops fill=%lld blit=%lld stretch=%lld mask=%lld\n",
        (t - l) / 1e9, c[Frames], c[DroppedFrames], c[RelPresent],
        ms[LockFinish], c[LockFinish], ms[UnlockFinish], c[UnlockFinish],
        ms[CpuFill], c[CpuFill], ms[CpuPixmap], c[CpuPixmap],
        ms[FlushFinish], c[FlushFinish], ms[FlushBlit], c[FlushBlit], c[BytesCopied] / 1048576.0,
        ms[FlipWait], c[FlipWait], ms[OpaqueScan], c[OpaqueScan],
        c[GpuFill], c[GpuBlit], c[GpuStretch], c[GpuMask]);
}

/* RAII timer: accumulates elapsed ns into slot id (no-op when disabled). */
struct Scope {
    Id id; long long t0;
    explicit Scope(Id i) : id(i), t0(enabled() ? nowNs() : 0) {}
    ~Scope() { if (t0) add(id, nowNs() - t0); }
};

} // namespace EtnaStats
#endif
