/*
 * etnaviv2d_benchmark.cpp — GPU vs CPU frame-rate benchmark.
 *
 * Mimics a widget application rendering N sprites per frame and measures
 * actual achieved FPS for:
 *   (a) etnaviv2d plugin — GPU fill + blit compositing
 *   (b) linuxfb plugin   — CPU raster baseline
 *
 * Two compositing modes (selected by QT_ETNAVIV2D_BENCH_OPAQUE):
 *   0 (default) — alpha=200 translucent sprites, Format_ARGB32_Premultiplied.
 *                 Both platforms do a full SRC_OVER alpha blend.
 *                 This is the fair apples-to-apples comparison.
 *   1           — alpha=255 fully-opaque sprites.
 *                 linuxfb can use a plain memcpy; etnaviv2d uses ETNA2D_BLEND_NONE.
 *                 Measures raw blit throughput without blending overhead.
 *
 * Run modes:
 *   QT_QPA_PLATFORM=etnaviv2d  ./etnaviv2d_benchmark [nframes] [nsprites]
 *   QT_QPA_PLATFORM=linuxfb   ./etnaviv2d_benchmark [nframes] [nsprites]
 *   QT_ETNAVIV2D_BENCH_OPAQUE=1  QT_QPA_PLATFORM=etnaviv2d ./etnaviv2d_benchmark ...
 *
 * Output:
 *   platform=X  frames=N  sprites=M  mode=alpha|opaque  elapsed=X.XXXs  fps=YY.Y  ms/frame=Z.ZZ
 *
 * Defaults: nframes=120, nsprites=32.
 *
 * The benchmark uses QPainter exclusively (no direct libetna2d calls) so
 * it exercises exactly the QPA path a real application uses.
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QDebug>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/* -----------------------------------------------------------------------
 * CPU utilisation helpers — reads /proc/stat for aggregate + per-core.
 * ----------------------------------------------------------------------- */
struct CpuStat { unsigned long long idle, total; };

/* Parse one "cpu..." line already positioned in the FILE. */
static CpuStat parseCpuLine(FILE *f)
{
    CpuStat s{0, 0};
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(f, " %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle,
               &iowait, &irq, &softirq, &steal) == 8) {
        s.idle  = idle + iowait;
        s.total = user + nice + system + idle + iowait + irq + softirq + steal;
    }
    /* consume remainder of line */
    int c; while ((c = fgetc(f)) != '\n' && c != EOF) {}
    return s;
}

/* Snapshot: aggregate + per-core (up to MAX_CORES). */
static constexpr int MAX_CORES = 8;
struct AllCpuStat {
    CpuStat total;
    CpuStat core[MAX_CORES];
    int     ncores = 0;
};

static AllCpuStat readAllCpuStat()
{
    AllCpuStat r{};
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return r;
    char tag[16];
    while (fscanf(f, "%15s", tag) == 1) {
        if (strcmp(tag, "cpu") == 0) {
            r.total = parseCpuLine(f);
        } else if (strncmp(tag, "cpu", 3) == 0 && tag[3] >= '0' && tag[3] <= '9') {
            int idx = atoi(tag + 3);
            CpuStat cs = parseCpuLine(f);
            if (idx < MAX_CORES) {
                r.core[idx] = cs;
                if (idx + 1 > r.ncores) r.ncores = idx + 1;
            }
        } else {
            /* skip rest of line */
            int c; while ((c = fgetc(f)) != '\n' && c != EOF) {}
        }
    }
    fclose(f);
    return r;
}

/* Returns CPU busy percentage in [0,100] between two CpuStat snapshots. */
static double cpuUtil(const CpuStat &a, const CpuStat &b)
{
    unsigned long long dTotal = b.total - a.total;
    unsigned long long dIdle  = b.idle  - a.idle;
    if (dTotal == 0) return 0.0;
    return 100.0 * (1.0 - double(dIdle) / double(dTotal));
}

/* -----------------------------------------------------------------------
 * Benchmark widget
 * ----------------------------------------------------------------------- */
class BenchWidget : public QWidget
{
public:
    BenchWidget(int nframes, int nsprites, bool opaque, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_totalFrames(nframes)
        , m_nsprites(nsprites)
        , m_opaque(opaque)
        , m_fbFd(-1)
    {
        setWindowTitle(QStringLiteral("etnaviv2d benchmark"));

        /* Open /dev/fb0 for FBIO_WAITFORVSYNC when running under linuxfb.
         * etnaviv2d already syncs to vblank via drmModePageFlip — no fd needed. */
        if (QGuiApplication::platformName() == QLatin1String("linuxfb")) {
            m_fbFd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
            if (m_fbFd < 0)
                perror("open /dev/fb0 (vsync unavailable)");
        }

        /* Build sprite pixmap.
         * Always use Format_ARGB32_Premultiplied so both etnaviv2d and linuxfb
         * go through the same alpha-compositing code path (SRC_OVER blend).
         * With opaque=true, alpha=255 so the blend degenerates to a plain copy
         * on both platforms — measuring raw blit throughput. */
        const int alpha = opaque ? 255 : 200;
        QImage img(96, 96, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&img);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect( 0,  0, 48, 48, QColor(255,  64,  64, alpha));
            p.fillRect(48,  0, 48, 48, QColor( 64, 255,  64, alpha));
            p.fillRect( 0, 48, 48, 48, QColor( 64,  64, 255, alpha));
            p.fillRect(48, 48, 48, 48, QColor(255, 255,  64, alpha));
        }
        m_sprite = QPixmap::fromImage(img);

        /* Build sprite positions (deterministic, wrapping). */
        m_positions.reserve(nsprites);
        for (int i = 0; i < nsprites; ++i)
            m_positions.append(QPoint((i * 37) % 900, (i * 53) % 550));
    }

    ~BenchWidget()
    {
        if (m_fbFd >= 0)
            ::close(m_fbFd);
    }

    void startBenchmark()
    {
        m_frame = 0;
        m_cpuStatStart = readAllCpuStat();
        m_elapsed.start();
        QTimer::singleShot(0, this, &BenchWidget::nextFrame);
    }

    int             totalFrames()    const { return m_totalFrames; }
    qint64          elapsedMs()      const { return m_elapsed.elapsed(); }
    bool            isOpaque()       const { return m_opaque; }
    AllCpuStat      cpuStatStart()   const { return m_cpuStatStart; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        int w = width(), h = height();

        /* Background fill. */
        p.fillRect(0, 0, w, h, QColor(24, 24, 24));

        /* Animate sprites: offset by frame number. */
        int step = m_frame % 200;
        for (int i = 0; i < m_nsprites; ++i) {
            int x = (m_positions[i].x() + step) % w;
            int y = (m_positions[i].y() + step / 2) % h;
            x = qMin(x, w - 96);
            y = qMin(y, h - 96);
            p.drawPixmap(x, y, m_sprite);
        }

        /* Frame counter label (raster fallback on both platforms). */
        p.setPen(Qt::white);
        p.drawText(10, 20, QStringLiteral("Frame %1 / %2")
                            .arg(m_frame).arg(m_totalFrames));
        p.end(); /* flush QPainter before vsync wait */

        /* linuxfb vsync: block until the next vertical blanking interval so
         * the just-rendered frame lands tear-free, matching etnaviv2d's
         * drmModePageFlip vblank synchronisation. */
        if (m_fbFd >= 0) {
            unsigned int dummy = 0;
            ::ioctl(m_fbFd, FBIO_WAITFORVSYNC, &dummy);
        }
    }

private slots:
    void nextFrame()
    {
        update();
        ++m_frame;
        if (m_frame < m_totalFrames)
            QTimer::singleShot(0, this, &BenchWidget::nextFrame);
        else
            QApplication::quit();
    }

private:
    int             m_totalFrames;
    int             m_nsprites;
    bool            m_opaque;
    int             m_frame = 0;
    int             m_fbFd;          /* /dev/fb0 fd for FBIO_WAITFORVSYNC, or -1 */
    QPixmap         m_sprite;
    QVector<QPoint> m_positions;
    QElapsedTimer   m_elapsed;
    AllCpuStat      m_cpuStatStart{};
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int nframes  = (argc > 1) ? atoi(argv[1]) : 120;
    int nsprites = (argc > 2) ? atoi(argv[2]) : 32;
    if (nframes  < 1) nframes  = 120;
    if (nsprites < 1) nsprites = 32;

    bool opaque = qEnvironmentVariableIntValue("QT_ETNAVIV2D_BENCH_OPAQUE") != 0;

    QApplication app(argc, argv);

    printf("platform=%s  frames=%d  sprites=%d  mode=%s\n",
           qPrintable(QGuiApplication::platformName()),
           nframes, nsprites,
           opaque ? "opaque" : "alpha");

    BenchWidget w(nframes, nsprites, opaque);
    w.showFullScreen();
    w.startBenchmark();

    int rc = app.exec();

    qint64 ms  = w.elapsedMs();
    double fps  = (ms > 0) ? (w.totalFrames() * 1000.0 / ms) : 0.0;
    double mspf = (w.totalFrames() > 0) ? double(ms) / w.totalFrames() : 0.0;

    AllCpuStat cpuEnd   = readAllCpuStat();
    AllCpuStat cpuStart = w.cpuStatStart();
    double cpuTotal = cpuUtil(cpuStart.total, cpuEnd.total);

    /* Build per-core string: "cpu0=XX.X%  cpu1=XX.X%" */
    char coreBuf[128] = {};
    int  pos = 0;
    int  nc  = cpuEnd.ncores;
    for (int i = 0; i < nc && pos < (int)sizeof(coreBuf) - 20; ++i) {
        double u = cpuUtil(cpuStart.core[i], cpuEnd.core[i]);
        pos += snprintf(coreBuf + pos, sizeof(coreBuf) - pos,
                        "  cpu%d=%.1f%%", i, u);
    }

    printf("frames=%d  sprites=%d  elapsed=%.3fs  fps=%.1f  ms/frame=%.2f"
           "  cpu_total=%.1f%%%s\n",
           w.totalFrames(), nsprites, ms / 1000.0, fps, mspf,
           cpuTotal, coreBuf);

    return rc;
}
