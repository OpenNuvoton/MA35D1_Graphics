/*
 * etnaviv2d_benchmark.cpp - Qt 2D benchmark shared by the presentation clip and the ops table.
 *
 * Same binary, different platform plugin:
 *   QT_QPA_PLATFORM=linuxfb                  CPU raster baseline
 *   QT_QPA_PLATFORM=etnaviv2d:plane=primary  GC520 through our QPA (KMS primary)
 *
 * Modes
 *   --clip            Bouncing translucent sprites, sprite count ramps every stage,
 *                     HUD (platform, fps, CPU, Mpixel/s). Window + vsync.
 *   --ops --display   Every op/size shown in a window with vsync, for visual check.
 *   --ops             (default ops mode) Offscreen, no vsync: draws into a QPixmap,
 *                     no display output, finds raw capability. One RESULT line per op/size.
 *
 * Ops: fill, alphafill, blit, alpha (SRC_OVER pixmap), scale (2x upscale pixmap).
 * Sizes: 64x64, 256x256, full (screen size).
 *
 * Shared helpers: CpuMeter, Sprites/OpSet, drawPass(), Hud.
 * cpu_sys = all cores together, cpu_app = % of one core (see cpuPct).
 * Output lines start with CLIP / STAGE / SAMPLE / RESULT / VERIFY for easy grep.
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPixmap>
#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QScreen>
#include <QVector>
#include <QStringList>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/* ------------------------------------------------------------------ CPU meter */
struct CpuSnap { unsigned long long idle = 0, total = 0, proc = 0; qint64 wallUs = 0; };

static CpuSnap cpuSnap()
{
    CpuSnap s;
    if (FILE *f = fopen("/proc/stat", "r")) {
        unsigned long long u, n, sy, id, io, irq, sirq, st;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq, &st) == 8) {
            s.idle = id + io;
            s.total = u + n + sy + id + io + irq + sirq + st;
        }
        fclose(f);
    }
    if (FILE *f = fopen("/proc/self/stat", "r")) {
        char buf[1024]; size_t len = fread(buf, 1, sizeof(buf) - 1, f); buf[len] = 0; fclose(f);
        char *p = strrchr(buf, ')');
        unsigned long long ut = 0, st = 0;
        if (p) sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
        s.proc = ut + st;
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    s.wallUs = qint64(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    return s;
}

/* sys = whole-system CPU busy %, all cores together (2 cores: one fully busy thread = 50 %).
 * app = this process, as % of ONE core (a single-threaded app maxes at 100). */
static void cpuPct(const CpuSnap &a, const CpuSnap &b, double &sys, double &app)
{
    unsigned long long dt = b.total - a.total, di = b.idle - a.idle;
    sys = dt ? 100.0 * (1.0 - double(di) / double(dt)) : 0.0;
    /* jiffies of the process vs jiffies of all cores over the same interval */
    static const int ncpu = int(sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1);
    app = dt ? 100.0 * ncpu * double(b.proc - a.proc) / double(dt) : 0.0;
}

/* ------------------------------------------------------------------ ops / sizes */
enum Op { Fill, AlphaFill, Blit, Alpha, Scale, NOPS };
static const char *opName[NOPS] = { "fill", "alphafill", "blit", "alpha", "scale" };

struct SizeSpec { QString label; int dw, dh, k; };       /* dest rect, ops per pass */

static QVector<SizeSpec> sizeSpecs(int W, int H)
{
    QVector<SizeSpec> v;
    auto mk = [&](int s) { SizeSpec z; z.label = QString("%1x%1").arg(s); z.dw = z.dh = s;
                           z.k = qMax(1, int(std::lround(2.0 * W * H / (s * s)))); return z; };
    v << mk(64) << mk(256);
    SizeSpec f; f.label = "full"; f.dw = W; f.dh = H; f.k = 2; v << f;
    return v;
}

/* Source pixmaps for one size, built once (GPU surfaces on etnaviv2d). */
struct OpSet {
    SizeSpec sz;
    QPixmap opaque, alpha, half;
    QVector<QPoint> pos;
    static QPixmap mkOpaque(int w, int h)
    {
        QImage im(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            QRgb *l = reinterpret_cast<QRgb *>(im.scanLine(y));
            for (int x = 0; x < w; ++x)
                l[x] = qRgb(40 + (x * 200 / w), 60 + (y * 180 / h), 220 - (x * 150 / w));
        }
        return QPixmap::fromImage(im);
    }
    static QPixmap mkAlpha(int w, int h, int a)
    {
        QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&im);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(0, 0, w / 2, h / 2, QColor(255, 64, 64, a));
        p.fillRect(w / 2, 0, w - w / 2, h / 2, QColor(64, 255, 64, a));
        p.fillRect(0, h / 2, w / 2, h - h / 2, QColor(64, 64, 255, a));
        p.fillRect(w / 2, h / 2, w - w / 2, h - h / 2, QColor(255, 255, 64, a));
        p.end();
        return QPixmap::fromImage(im);
    }
    void build(const SizeSpec &s, int W, int H)
    {
        sz = s;
        opaque = mkOpaque(s.dw, s.dh);
        alpha  = mkAlpha(s.dw, s.dh, 200);
        half   = mkAlpha(qMax(2, s.dw / 2), qMax(2, s.dh / 2), 255); /* 4 flat colour blocks: 2x upscale is visible, unlike the smooth gradient */
        unsigned r = 12345;
        pos.clear();
        for (int i = 0; i < s.k; ++i) {
            r = r * 1103515245u + 12345u; int x = (r >> 8) % qMax(1, W - s.dw + 1);
            r = r * 1103515245u + 12345u; int y = (r >> 8) % qMax(1, H - s.dh + 1);
            pos << QPoint(x, y);
        }
    }
};

/* One "pass" = k ops of one kind, the same code in display and offscreen mode. */
static void drawPass(QPainter &p, Op op, const OpSet &s, int W, int H, int phase)
{
    static const QColor col[8] = { QColor(230, 60, 60), QColor(60, 200, 80), QColor(70, 90, 230), QColor(240, 200, 50),
                                   QColor(200, 80, 200), QColor(60, 200, 200), QColor(250, 140, 40), QColor(160, 160, 160) };
    const int dw = s.sz.dw, dh = s.sz.dh;
    /* Qt's blitter engine only sends fillRect() to the GPU when the painter state is "clean": a default painter has
     * brush = NoBrush (pattern bit set), so fillRect(rect, color) stays on the CPU raster path. A solid brush makes it
     * qualify Fill uses the GPU path; alphafill is left in the default state on purpose. */
    if (op == Fill) p.setBrush(QColor(0, 0, 0));
    for (int i = 0; i < s.sz.k; ++i) {
        int x = (s.pos[i].x() + phase * 5) % qMax(1, W - dw + 1);
        int y = (s.pos[i].y() + phase * 3) % qMax(1, H - dh + 1);
        switch (op) {
        case Fill:      p.fillRect(x, y, dw, dh, col[i & 7]); break;
        case AlphaFill: { QColor c = col[i & 7]; c.setAlpha(128); p.fillRect(x, y, dw, dh, c); break; }
        case Blit:      p.drawPixmap(x, y, s.opaque); break;
        case Alpha:     p.drawPixmap(x, y, s.alpha); break;
        case Scale:     p.drawPixmap(QRect(x, y, dw, dh), s.half); break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ HUD */
static QPixmap makeHud(const QStringList &lines, int w = 400)
{
    QFont f; f.setPixelSize(15); f.setBold(true);
    QFontMetrics fm(f);
    int lh = fm.height() + 2, h = lh * lines.size() + 12;
    QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
    im.fill(QColor(0, 0, 0, 255));
    QPainter p(&im);
    p.setFont(f); p.setPen(QColor(120, 255, 120));
    for (int i = 0; i < lines.size(); ++i) p.drawText(8, 6 + (i + 1) * lh - 4, lines[i]);
    p.end();
    return QPixmap::fromImage(im);
}

/* ------------------------------------------------------------------ verify (offscreen) */
static bool near(const QColor &c, int r, int g, int b, int tol = 4)
{ return abs(c.red() - r) <= tol && abs(c.green() - g) <= tol && abs(c.blue() - b) <= tol; }

static void verifyOps()
{
    QPixmap d(64, 64);
    { QPainter p(&d); p.fillRect(d.rect(), QColor(0, 0, 255)); p.fillRect(8, 8, 16, 16, QColor(255, 0, 0)); }
    QImage a = d.toImage();
    printf("VERIFY fill       %s\n", near(a.pixelColor(10, 10), 255, 0, 0) && near(a.pixelColor(40, 40), 0, 0, 255) ? "ok" : "FAIL");
    QPixmap src = OpSet::mkAlpha(16, 16, 200);
    { QPainter p(&d); p.fillRect(d.rect(), QColor(0, 0, 255)); p.drawPixmap(0, 0, src); }
    a = d.toImage();
    /* src (255,64,64,a200) premult = (200,50,50) over blue: r200 g50 b=50+55 */
    { QColor c = a.pixelColor(2, 2);
      printf("VERIFY alpha      %s (got %d,%d,%d, expect ~200,50,105)\n", near(c, 200, 50, 105, 6) ? "ok" : "FAIL", c.red(), c.green(), c.blue()); }
    QPixmap sm = OpSet::mkAlpha(8, 8, 255);
    { QPainter p(&d); p.fillRect(d.rect(), QColor(0, 0, 0)); p.drawPixmap(QRect(0, 0, 16, 16), sm); }
    a = d.toImage();
    printf("VERIFY scale      %s\n", near(a.pixelColor(2, 2), 255, 64, 64) && near(a.pixelColor(13, 13), 255, 255, 64) ? "ok" : "FAIL");
    QPixmap op = OpSet::mkOpaque(16, 16);
    { QPainter p(&d); p.fillRect(d.rect(), QColor(0, 0, 0)); p.drawPixmap(4, 4, op); }
    a = d.toImage();
    printf("VERIFY blit       %s\n", near(a.pixelColor(4, 4), 40, 60, 220, 8) && near(a.pixelColor(30, 30), 0, 0, 0) ? "ok" : "FAIL");
    fflush(stdout);
}

/* ------------------------------------------------------------------ frame window */
class FrameWindow : public QWidget
{
public:
    FrameWindow()
    {
        /* Every frame repaints the whole window: no Qt background erase pass
         * (an extra full-screen fill under the sprites showed as flicker). */
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        if (QGuiApplication::platformName() == QLatin1String("linuxfb")) {
            m_fbFd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
            if (m_fbFd < 0) perror("open /dev/fb0 (no vsync)");
        }
    }
    ~FrameWindow() override { if (m_fbFd >= 0) ::close(m_fbFd); }
    void run() { showFullScreen(); QTimer::singleShot(0, this, &FrameWindow::tick); }

protected:
    virtual void paintFrame(QPainter &p) = 0;
    virtual bool finished() const = 0;
    void paintEvent(QPaintEvent *) override
    {
        { QPainter p(this); paintFrame(p); }
        if (m_fbFd >= 0) { unsigned dummy = 0; ::ioctl(m_fbFd, FBIO_WAITFORVSYNC, &dummy); }
    }
private:
    void tick()
    {
        if (finished()) { QApplication::quit(); return; }
        update();
        QTimer::singleShot(m_paceMs, this, &FrameWindow::tick);   /* QT2D_FPS=n paces the redraw (default 0 = unpaced) */
    }
    int m_paceMs = qEnvironmentVariableIntValue("QT2D_FPS") > 0 ? 1000 / qEnvironmentVariableIntValue("QT2D_FPS") : 0;
    int m_fbFd = -1;
};

/* ------------------------------------------------------------------ clip mode */
class ClipWindow : public FrameWindow
{
public:
    ClipWindow(QVector<int> counts, double stageSecs) : m_counts(counts), m_stageSecs(stageSecs)
    {
        for (int i = 0; i < 6; ++i) {
            static const QColor pal[6] = { QColor(255, 64, 64), QColor(64, 255, 64), QColor(64, 64, 255),
                                           QColor(255, 255, 64), QColor(64, 255, 255), QColor(255, 64, 255) };
            QColor c = pal[i]; c.setAlpha(200);
            QImage im(96, 96, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&im);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(0, 0, 96, 96, c);
            p.end();
            m_spr[i] = QPixmap::fromImage(im);
        }
        unsigned r = 12345;
        auto rnd = [&r]() { r = r * 1664525u + 1013904223u; return r >> 8; };
        for (int i = 0; i < 1024; ++i) {
            px[i] = rnd() % 1000 / 1000.0; py[i] = rnd() % 1000 / 1000.0;
            vx[i] = (int(rnd() % 7) - 3) * 1.5; if (vx[i] == 0) vx[i] = 2;   /* px per frame */
            vy[i] = (int(rnd() % 7) - 3) * 1.5; if (vy[i] == 0) vy[i] = -2;
        }
        m_plat = QGuiApplication::platformName();
    }
    void begin()
    {
        m_W = QGuiApplication::primaryScreen()->size().width();
        m_H = QGuiApplication::primaryScreen()->size().height();
        for (int i = 0; i < 1024; ++i) { px[i] *= (m_W - 96); py[i] *= (m_H - 96); }
        printf("CLIP platform=%s res=%dx%d stages=%d stage_secs=%.1f\n", qPrintable(m_plat), m_W, m_H, int(m_counts.size()), m_stageSecs);
        fflush(stdout);
        m_t0.start(); m_last = 0;
        m_stageSnap = m_secSnap = cpuSnap();
        updateHud(0, 0, 0, 0);
        run();
    }
protected:
    bool finished() const override { return m_stage >= m_counts.size(); }
    void paintFrame(QPainter &p) override
    {
        const double now = m_t0.nsecsElapsed() / 1e9;
        const int n = m_counts[qMin(m_stage, m_counts.size() - 1)];
        /* No full-screen background fill per frame (it is not a UI op and blocks the app ~12 ms at 1080p):
         * fill once on the first frame, then erase only the previous sprite rectangles (small fills). */
        static const QColor bg(24, 28, 40);
        p.setBrush(bg);   /* solid brush: lets fillRect take the GPU path (see drawPass) */
        /* the window is first painted at 640x480, then resized to full screen: the backing store is re-created
         * (content undefined), so refill whenever the painted size changes, not only on the very first frame */
        if (m_firstFrame || m_paintSize != p.device()->width() * 100000 + p.device()->height()) {
            p.fillRect(0, 0, m_W, m_H, bg); m_firstFrame = false; m_paintSize = p.device()->width() * 100000 + p.device()->height(); }
        else for (int i = 0; i < m_prevN; ++i) p.fillRect(m_prevX[i], m_prevY[i], 96, 96, bg);
        for (int i = 0; i < n; ++i) {
            px[i] += vx[i]; py[i] += vy[i];   /* fixed step per frame, like bench2d: no big jumps at low fps */
            if (px[i] < 0) { px[i] = 0; vx[i] = -vx[i]; }
            if (px[i] > m_W - 96) { px[i] = m_W - 96; vx[i] = -vx[i]; }
            if (py[i] < 0) { py[i] = 0; vy[i] = -vy[i]; }
            if (py[i] > m_H - 96) { py[i] = m_H - 96; vy[i] = -vy[i]; }
            m_prevX[i] = int(px[i]); m_prevY[i] = int(py[i]);
            p.drawPixmap(m_prevX[i], m_prevY[i], m_spr[m_nvar > 1 ? i % m_nvar : 0]);
        }
        m_prevN = n;
        p.drawPixmap(10, 10, m_hud);
        ++m_frames; ++m_stageFrames;
        /* 1 s HUD refresh */
        if (now - m_secT >= 1.0) {
            CpuSnap s = cpuSnap(); double sys, app; cpuPct(m_secSnap, s, sys, app);
            double fps = m_frames / (now - m_secT);
            updateHud(n, fps, sys, app);
            printf("SAMPLE platform=%s sprites=%d fps=%.1f cpu_sys=%.1f cpu_app=%.1f\n", qPrintable(m_plat), n, fps, sys, app);
            fflush(stdout);
            m_secSnap = s; m_secT = now; m_frames = 0;
        }
        /* stage end */
        if (now - m_stageT >= m_stageSecs) {
            CpuSnap s = cpuSnap(); double sys, app; cpuPct(m_stageSnap, s, sys, app);
            double el = now - m_stageT, fps = m_stageFrames / el;
            double mp = (double(m_W) * m_H + double(n) * 96 * 96) * fps / 1e6;
            printf("STAGE platform=%s res=%dx%d sprites=%d secs=%.1f fps=%.1f ms_frame=%.2f cpu_sys=%.1f cpu_app=%.1f mpix_s=%.0f\n",
                   qPrintable(m_plat), m_W, m_H, n, el, fps, 1000.0 / fps, sys, app, mp);
            fflush(stdout);
            m_stageSnap = s; m_stageT = now; m_stageFrames = 0; ++m_stage;
        }
    }
private:
    void updateHud(int n, double fps, double sys, double app)
    {
        double mp = (double(m_W) * m_H + double(n) * 96 * 96) * fps / 1e6;
        m_hud = makeHud({ QString("platform  %1   %2x%3").arg(m_plat).arg(m_W).arg(m_H),
                          QString("sprites   %1  (96x96, alpha)").arg(n),
                          QString("FPS %1    %2 ms/frame").arg(fps, 0, 'f', 1).arg(fps > 0 ? 1000.0 / fps : 0, 0, 'f', 1),
                          QString("CPU: all cores %1 %   app %2 % of 1 core").arg(sys, 0, 'f', 0).arg(app, 0, 'f', 0),
                          QString("blended %1 Mpixel/s").arg(mp, 0, 'f', 0) });
    }
    int m_prevX[1024] = {}, m_prevY[1024] = {}, m_prevN = 0; bool m_firstFrame = true; int m_paintSize = 0;
    QVector<int> m_counts; double m_stageSecs; int m_nvar = qBound(1, qEnvironmentVariableIntValue("QT2D_VARIANTS") ? qEnvironmentVariableIntValue("QT2D_VARIANTS") : 6, 6);
    QPixmap m_spr[6], m_hud; QString m_plat;
    double px[1024], py[1024], vx[1024], vy[1024];
    int m_W = 0, m_H = 0, m_stage = 0, m_frames = 0, m_stageFrames = 0;
    double m_last = 0, m_secT = 0, m_stageT = 0;
    QElapsedTimer m_t0; CpuSnap m_stageSnap, m_secSnap;
};

/* ------------------------------------------------------------------ ops: display mode */
class OpsDisplayWindow : public FrameWindow
{
    int m_paintSize = 0;
public:
    OpsDisplayWindow(double secs) : m_secs(secs) { m_plat = QGuiApplication::platformName(); }
    void begin()
    {
        m_W = QGuiApplication::primaryScreen()->size().width();
        m_H = QGuiApplication::primaryScreen()->size().height();
        for (const SizeSpec &s : sizeSpecs(m_W, m_H)) { OpSet o; o.build(s, m_W, m_H); m_sets << o; }
        printf("OPS-DISPLAY platform=%s res=%dx%d secs_per_case=%.1f\n", qPrintable(m_plat), m_W, m_H, m_secs);
        m_t0.start(); m_caseT = 0; m_snap = cpuSnap();
        enter();
        run();
    }
protected:
    bool finished() const override { return m_case >= int(NOPS) * m_sets.size(); }
    void paintFrame(QPainter &p) override
    {
        const double now = m_t0.nsecsElapsed() / 1e9;
        Op op = Op(m_case / m_sets.size()); const OpSet &s = m_sets[m_case % m_sets.size()];
        const int sz = p.device()->width() * 100000 + p.device()->height();   /* backing store is re-created after the 640x480 -> full-screen resize */
        if (m_frames == 0 || sz != m_paintSize) { p.setBrush(QColor(40, 40, 40)); p.fillRect(0, 0, m_W, m_H, QColor(40, 40, 40)); m_paintSize = sz; }   /* once per case, not per frame */
        drawPass(p, op, s, m_W, m_H, m_frames);
        p.drawPixmap(10, 10, m_hud);
        ++m_frames;
        if (now - m_caseT >= m_secs) {
            CpuSnap c = cpuSnap(); double sys, app; cpuPct(m_snap, c, sys, app);
            double el = now - m_caseT, fps = m_frames / el;
            double ops = fps * s.sz.k, mp = ops * s.sz.dw * s.sz.dh / 1e6;
            printf("RESULT platform=%s mode=display-vsync res=%dx%d op=%s size=%s secs=%.1f passes=%d fps=%.1f ops_s=%.0f mpix_s=%.0f cpu_sys=%.1f cpu_app=%.1f\n",
                   qPrintable(m_plat), m_W, m_H, opName[op], qPrintable(s.sz.label), el, m_frames, fps, ops, mp, sys, app);
            fflush(stdout);
            ++m_case; m_caseT = now; m_snap = c; m_frames = 0;
            if (!finished()) enter();
        }
    }
private:
    void enter()
    {
        Op op = Op(m_case / m_sets.size()); const OpSet &s = m_sets[m_case % m_sets.size()];
        m_hud = makeHud({ QString("op %1   size %2   ops/frame %3").arg(opName[op]).arg(s.sz.label).arg(s.sz.k),
                          QString("platform %1  %2x%3 (vsync)").arg(m_plat).arg(m_W).arg(m_H) }, 420);
    }
    double m_secs; QString m_plat; QVector<OpSet> m_sets; QPixmap m_hud;
    int m_W = 0, m_H = 0, m_case = 0, m_frames = 0; double m_caseT = 0;
    QElapsedTimer m_t0; CpuSnap m_snap;
};

/* ------------------------------------------------------------------ ops: offscreen, no vsync */
static void runOpsOffscreen(int W, int H, double secs)
{
    const QString plat = QGuiApplication::platformName();
    printf("OPS-OFFSCREEN platform=%s res=%dx%d secs_per_case=%.1f (QPixmap target, no vsync, no display output)\n", qPrintable(plat), W, H, secs);
    verifyOps();
    QPixmap dest(W, H);
    { QPainter p(&dest); p.fillRect(dest.rect(), QColor(40, 40, 40)); }
    for (const SizeSpec &sz : sizeSpecs(W, H)) {
        OpSet set; set.build(sz, W, H);
        for (int o = 0; o < NOPS; ++o) {
            for (int w = 0; w < 3; ++w) { QPainter p(&dest); drawPass(p, Op(o), set, W, H, w); }   /* warm-up */
            CpuSnap c0 = cpuSnap(); QElapsedTimer t; t.start();
            long passes = 0;
            while (t.elapsed() < secs * 1000.0) {
                { QPainter p(&dest); drawPass(p, Op(o), set, W, H, int(passes)); }   /* painter end = GPU fence */
                ++passes;
            }
            double el = t.nsecsElapsed() / 1e9; CpuSnap c1 = cpuSnap();
            double sys, app; cpuPct(c0, c1, sys, app);
            double ops = double(passes) * sz.k, ops_s = ops / el, mp = ops_s * sz.dw * sz.dh / 1e6;
            printf("RESULT platform=%s mode=offscreen-novsync res=%dx%d op=%s size=%s secs=%.1f passes=%ld fps=%.1f ops_s=%.0f mpix_s=%.0f screens_s=%.1f us_op=%.2f cpu_sys=%.1f cpu_app=%.1f\n",
                   qPrintable(plat), W, H, opName[o], qPrintable(sz.label), el, passes, passes / el, ops_s, mp, mp * 1e6 / (double(W) * H), 1e6 / ops_s, sys, app);
            fflush(stdout);
        }
    }
}


int main(int argc, char **argv)
{
    bool clip = false, ops = false, display = false;
    double secs = 3.0, stageSecs = 5.0;
    QVector<int> counts = { 16, 32, 64, 128, 256 };
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--clip")) clip = true;
        else if (!strcmp(argv[i], "--ops")) ops = true;
        else if (!strcmp(argv[i], "--display")) display = true;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stage-secs") && i + 1 < argc) stageSecs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sprites") && i + 1 < argc) {
            counts.clear();
            for (const QString &s : QString(argv[++i]).split(',')) counts << qBound(1, s.toInt(), 1024);
        } else { fprintf(stderr, "usage: etnaviv2d_benchmark --clip [--stage-secs S] [--sprites 16,32,..]\n"
                                 "       etnaviv2d_benchmark --ops [--display] [--secs S]\n"); return 2; }
    }
    if (clip == ops) { fprintf(stderr, "choose exactly one of --clip / --ops\n"); return 2; }

    QApplication app(argc, argv);
    const QSize scr = QGuiApplication::primaryScreen()->size();
    if (clip) { ClipWindow w(counts, stageSecs); w.begin(); return app.exec(); }
    if (display) { OpsDisplayWindow w(secs); w.begin(); return app.exec(); }
    runOpsOffscreen(scr.width(), scr.height(), secs);
    return 0;
}
