/*
 * dashboard - V5-2 IVI demo (ivi-shell): full-screen Qt dashboard on the primary plane (IVI surface 1000) and a
 * centered NV12 video popup on the overlay plane (IVI surface 2000, started with gst-launch-1.0 + waylandsink).
 *
 * Dashboard: three image boxes (tap = open the popup with that clip), control buttons (CLOSE, VOL-, VOL+, HUD),
 * animated gauges and a clock. Opaque UI: use QT_ETNA2D_FMT=rgb565 or argb4444 (never argb8888 on 1080p).
 * Lazy analysis HUD (top-left, 1 Hz): video fps (only while the popup is open), UI paints/s, CPU, memory, CMA,
 * demo condition, plane formats. Drawing: static art pre-rendered once, GPU blits and fills, text redrawn only
 * when the value changes, dirty rectangles only.
 *
 * Usage: dashboard --clips a.h264,b.h264,c.h264 [--thumbs DIR] [--cond TEXT] [--fps N]
 *                 [--auto N] [--snap FILE --snapsec N] [--sec N]
 *   --auto N      no mouse/touch: every N seconds toggle the popup (open box 1, close, open box 2, ...)
 *   --snap FILE   save the dashboard as PNG after --snapsec N seconds (the popup is a separate plane, not in it)
 *   --sec N       exit after N seconds
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDateTime>
#include <QFileInfo>
#include <QtMath>
#include "../common/hud.h"

static const char *kLog = "/tmp/demo_gst.log";

class Dashboard : public QWidget
{
public:
    Dashboard(int fps, const QStringList &clips, const QString &thumbs, const QString &cond, int autoSec)
        : m_clips(clips), m_thumbDir(thumbs), m_cond(cond), m_fps(fps)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NoSystemBackground);
        m_clock.start();
        QTimer *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this] { tick(); });
        t->start(1000 / qMax(1, m_fps));
        if (autoSec > 0) {
            QTimer *a = new QTimer(this);
            connect(a, &QTimer::timeout, this, [this] { if (m_open >= 0) closePopup(); else openPopup(m_auto++ % 3); });
            a->start(autoSec * 1000);
        }
    }
    ~Dashboard() { if (m_gst) { m_gst->disconnect(); m_gst->kill(); m_gst->waitForFinished(1000); } }

protected:
    void resizeEvent(QResizeEvent *) override { layoutAll(); update(); }

    void paintEvent(QPaintEvent *e) override
    {
        ++m_paints;
        QPainter p(this);
        for (const QRect &r : e->region().rects()) { p.save(); p.setClipRect(r); drawAll(p, r); p.restore(); }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        for (int i = 0; i < 3; ++i) if (m_box[i].contains(e->pos())) { openPopup(i); return; }
        for (int i = 0; i < 4; ++i)
            if (m_btn[i].contains(e->pos())) { m_down = i; update(m_btn[i]); onButton(i); return; }
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_down >= 0) { const int d = m_down; m_down = -1; update(m_btn[d]); }
    }

private:
    // ---- popup video (overlay plane) ----------------------------------------------------------------------
    void openPopup(int i)
    {
        if (m_gst || i < 0 || i >= m_clips.size()) return;      // one popup at a time
        m_gst = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("GST_WAYLANDSINK_IVI_SURFACE_ID", "2000");
        m_gst->setProcessEnvironment(env);
        m_gst->setStandardOutputFile(kLog);
        m_gst->setStandardErrorFile(kLog);
        connect(m_gst, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
            m_gst->deleteLater(); m_gst = nullptr; setOpen(-1); });
        m_gst->start("gst-launch-1.0", QStringList() << "-v" << "filesrc" << ("location=" + m_clips[i]) << "!" << "h264parse" << "!"
            << "v4l2slh264dec" << "!" << "fpsdisplaysink" << "video-sink=waylandsink" << "text-overlay=false" << "sync=true"
            << "signal-fps-measurements=true" << "fps-update-interval=1000");
        setOpen(i);
    }
    void closePopup() { if (m_gst) m_gst->terminate(); }
    void setOpen(int i)
    {
        const int old = m_open; m_open = i;
        if (old >= 0) update(m_box[old].adjusted(-S(5), -S(5), S(5), S(5)));
        if (i >= 0)   update(m_box[i].adjusted(-S(5), -S(5), S(5), S(5)));
        m_hudT = -1e9;                                       // refresh the HUD at the next tick
    }

    // ---- simulation / HUD -----------------------------------------------------------------------------------
    void tick()
    {
        const double t = m_clock.elapsed() / 1000.0;
        m_temp = 45.0 + 15.0 * qSin(t * 0.3);
        mark(m_tp_i, qRound(m_temp), m_rG[2]);
        if (t - m_numT > 0.2) { m_numT = t; update(m_rGTxt[0]); update(m_rGTxt[1]); update(m_rGTxt[2]); }
        const int sec = int(QDateTime::currentSecsSinceEpoch() % 86400);
        mark(m_clk_i, sec, m_rClock);
        if (t - m_hudT > 1.0) {
            const double fpsUi = m_hudT < 0 ? 0 : (m_paints - m_paints0) / (t - m_hudT);
            m_paints0 = m_paints; m_hudT = t; updateHud(fpsUi); update(m_rHud);
            mark(m_cpu_i, qRound(m_cpu), m_rG[0]);
        }
    }
    void mark(int &prev, int cur, const QRect &r) { if (cur != prev) { prev = cur; update(r); } }

    void updateHud(double fpsUi)
    {
        const HudStats h = m_samp.sample(m_open >= 0 ? QString(kLog) : QString());
        m_cpu = h.cpu;
        const QString fmt = qEnvironmentVariableIsSet("QT_ETNA2D_FMT") ? QString(qgetenv("QT_ETNA2D_FMT")).toUpper() : "ARGB8888";
        const QString clip = m_open >= 0 ? QFileInfo(m_clips[m_open]).completeBaseName() : "none";
        m_hud[0] = QString("VIDEO %1 fps    UI paints %2/s").arg(m_open >= 0 ? h.videoFps : QString("-")).arg(fpsUi, 0, 'f', 0);
        m_hud[1] = QString("CPU %1 %  (c0 %2  c1 %3)").arg(h.cpu, 0, 'f', 0).arg(h.c0, 0, 'f', 0).arg(h.c1, 0, 'f', 0);
        m_hud[2] = QString("MEM %1 / %2 MB   CMA free %3 MB").arg((h.totalKb - h.availKb) / 1024).arg(h.totalKb / 1024).arg(h.cmaFreeKb / 1024);
        m_hud[3] = QString("%1 | %2x%3 | popup %4").arg(m_cond).arg(width()).arg(height()).arg(clip);
        m_hud[4] = QString("primary %1 UI | overlay %2").arg(fmt).arg(m_open >= 0 ? "NV12 video" : "none");
    }

    void onButton(int i)
    {
        if (i == 0) closePopup();
        else if (i == 1) { m_vol = qMax(0, m_vol - 10); update(m_rG[1]); update(m_rGTxt[1]); }
        else if (i == 2) { m_vol = qMin(100, m_vol + 10); update(m_rG[1]); update(m_rGTxt[1]); }
        else { m_hudOn = !m_hudOn; update(m_rHud); }
    }

    // ---- layout / pre-rendered art ---------------------------------------------------------------------------
    int S(double v) const { return qRound(v * m_s); }

    void layoutAll()
    {
        m_s = height() / 600.0;
        const int W = width(), H = height();
        m_rHud = QRect(S(10), S(10), S(280), S(94));
        m_rClock = QRect(W - S(170), S(14), S(158), S(30));
        const int bw = S(290), bh = S(190), gap = S(20), x0 = (W - (3 * bw + 2 * gap)) / 2;
        for (int i = 0; i < 3; ++i) {
            m_box[i] = QRect(x0 + i * (bw + gap), S(125), bw, bh);
            m_rG[i] = QRect(x0 + i * (bw + gap), S(375), bw, S(26));
            m_rGTxt[i] = QRect(x0 + i * (bw + gap), S(405), bw, S(24));
        }
        for (int i = 0; i < 4; ++i) m_btn[i] = QRect(W / 2 - S(322) + i * S(165), H - S(70), S(150), S(52));
        QFont f; f.setPixelSize(qMax(8, S(15))); f.setBold(true); m_font = f;
        QFont h; h.setPixelSize(qMax(8, S(11))); m_hudFont = h;

        // static background: gradient, title, image boxes with thumbnails, gauge labels
        QImage bg(W, H, QImage::Format_RGB32);
        { QPainter q(&bg); q.setRenderHint(QPainter::SmoothPixmapTransform);
          QLinearGradient g(0, 0, 0, H); g.setColorAt(0, QColor(18, 28, 48)); g.setColorAt(1, QColor(8, 12, 22)); q.fillRect(bg.rect(), g);
          QFont tf = m_font; tf.setPixelSize(S(26)); q.setFont(tf); q.setPen(QColor(220, 230, 255));
          q.drawText(QRect(0, S(10), W, S(40)), Qt::AlignCenter, "DASHBOARD");
          const char *cap[3] = {"Flight 1", "Flight 2", "Flight 3"};
          const char *gl[3] = {"CPU LOAD", "VOLUME", "TEMP (sim)"};
          q.setFont(m_font);
          for (int i = 0; i < 3; ++i) {
              QPixmap th(QString("%1/ivi_%2.png").arg(m_thumbDir).arg(i + 1));
              q.fillRect(m_box[i].adjusted(-S(2), -S(2), S(2), S(2)), QColor(90, 110, 150));
              if (!th.isNull()) q.drawPixmap(m_box[i], th.scaled(m_box[i].size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation), QRect(0, 0, m_box[i].width(), m_box[i].height()));
              else q.fillRect(m_box[i], QColor(40, 50, 70));
              q.fillRect(QRect(m_box[i].x(), m_box[i].bottom() - S(30), m_box[i].width(), S(31)), QColor(0, 0, 0, 150));
              q.setPen(Qt::white); q.drawText(m_box[i].adjusted(S(8), 0, 0, -S(5)), Qt::AlignLeft | Qt::AlignBottom, QString("%1  (tap to play)").arg(cap[i]));
              q.setPen(QColor(170, 190, 230)); q.drawText(QRect(m_rG[i].x(), m_rG[i].y() - S(24), m_rG[i].width(), S(22)), Qt::AlignLeft | Qt::AlignVCenter, gl[i]);
          }
        }
        m_bg = QPixmap::fromImage(bg);

        QImage hp(m_rHud.size(), QImage::Format_ARGB32_Premultiplied); hp.fill(Qt::transparent);
        { QPainter q(&hp); q.setPen(Qt::NoPen); q.setRenderHint(QPainter::Antialiasing); q.setBrush(QColor(0, 0, 0, 150)); q.drawRoundedRect(hp.rect(), S(8), S(8)); }
        m_pmHud = QPixmap::fromImage(hp);

        const char *names[4] = {"CLOSE", "VOL -", "VOL +", "HUD"};
        for (int i = 0; i < 4; ++i)
            for (int st = 0; st < 2; ++st) {
                QImage im(m_btn[i].size(), QImage::Format_RGB32); im.fill(QColor(8, 12, 22));
                QPainter q(&im); q.setRenderHint(QPainter::Antialiasing); q.setPen(QPen(Qt::white, S(2)));
                q.setBrush(st ? QColor(230, 120, 30) : (i == 0 ? QColor(170, 50, 50) : QColor(30, 100, 180)));
                q.drawRoundedRect(im.rect().adjusted(2, 2, -2, -2), S(12), S(12));
                q.setFont(m_font); q.drawText(im.rect(), Qt::AlignCenter, names[i]); q.end();
                m_pmBtn[i][st] = QPixmap::fromImage(im);
            }
        updateHud(0);
    }

    void bar(QPainter &p, const QRect &r, const QRect &t, double frac, const QColor &c)
    {
        if (!r.intersects(t)) return;
        p.fillRect(t, QColor(255, 255, 255));
        p.fillRect(t.adjusted(S(3), S(3), -S(3), -S(3)), QColor(20, 20, 20));
        const QRect in = t.adjusted(S(5), S(5), -S(5), -S(5));
        p.fillRect(QRect(in.x(), in.y(), qRound(in.width() * qBound(0.0, frac, 1.0)), in.height()), c);
    }

    void drawAll(QPainter &p, const QRect &r)
    {
        p.drawPixmap(r.topLeft(), m_bg, r);                    // background blit (static art)
        p.setFont(m_font);
        if (m_open >= 0) {                                      // marker of the box that is playing
            const QRect b = m_box[m_open].adjusted(-S(5), -S(5), S(5), S(5)); const int w = S(4);
            const QColor c(255, 170, 40);
            for (const QRect &s : {QRect(b.x(), b.y(), b.width(), w), QRect(b.x(), b.bottom() - w + 1, b.width(), w),
                                   QRect(b.x(), b.y(), w, b.height()), QRect(b.right() - w + 1, b.y(), w, b.height())})
                if (r.intersects(s)) p.fillRect(s.intersected(r), c);
        }
        bar(p, r, m_rG[0], m_cpu / 100.0, QColor(255, 170, 40));
        bar(p, r, m_rG[1], m_vol / 100.0, QColor(60, 180, 255));
        bar(p, r, m_rG[2], (m_temp - 20) / 60.0, QColor(90, 210, 110));
        p.setPen(Qt::white);
        if (r.intersects(m_rGTxt[0])) p.drawText(m_rGTxt[0], Qt::AlignLeft | Qt::AlignVCenter, QString("%1 %").arg(m_cpu, 0, 'f', 0));
        if (r.intersects(m_rGTxt[1])) p.drawText(m_rGTxt[1], Qt::AlignLeft | Qt::AlignVCenter, QString("%1 %").arg(m_vol));
        if (r.intersects(m_rGTxt[2])) p.drawText(m_rGTxt[2], Qt::AlignLeft | Qt::AlignVCenter, QString("%1 C").arg(m_temp, 0, 'f', 0));
        if (r.intersects(m_rClock)) p.drawText(m_rClock, Qt::AlignRight | Qt::AlignVCenter, QDateTime::currentDateTime().toString("HH:mm:ss"));
        for (int i = 0; i < 4; ++i) if (r.intersects(m_btn[i])) p.drawPixmap(m_btn[i].topLeft(), m_pmBtn[i][m_down == i ? 1 : 0]);
        if (m_hudOn && r.intersects(m_rHud)) {
            p.drawPixmap(m_rHud.topLeft(), m_pmHud);
            p.setPen(Qt::white); p.setFont(m_hudFont);
            for (int i = 0; i < 5; ++i) p.drawText(m_rHud.x() + S(8), m_rHud.y() + S(18) + i * S(16), m_hud[i]);
        }
    }

    QStringList m_clips; QString m_thumbDir, m_cond; int m_fps;
    QProcess *m_gst = nullptr; int m_open = -1, m_auto = 0;
    QElapsedTimer m_clock; double m_numT = 0, m_hudT = -1, m_cpu = 0, m_temp = 45, m_s = 1;
    quint64 m_paints = 0, m_paints0 = 0; HudSampler m_samp;
    bool m_hudOn = true; int m_down = -1, m_vol = 50, m_tp_i = -1, m_cpu_i = -1, m_clk_i = -1;
    QRect m_rHud, m_rClock, m_box[3], m_rG[3], m_rGTxt[3], m_btn[4];
    QPixmap m_bg, m_pmHud, m_pmBtn[4][2];
    QFont m_font, m_hudFont; QString m_hud[5];
};

int main(int argc, char **argv)
{
    QApplication a(argc, argv);
    int fps = qEnvironmentVariableIntValue("QT2D_FPS"); if (fps <= 0) fps = 30;
    int sec = 0, snapSec = 0, autoSec = 0; QString snap, thumbs = "thumbs", cond = "ivi-shell"; QStringList clips;
    for (int i = 1; i < argc; ++i) {
        const QString k = argv[i];
        if (k == "--fps" && i + 1 < argc) fps = QString(argv[++i]).toInt();
        else if (k == "--clips" && i + 1 < argc) clips = QString(argv[++i]).split(',');
        else if (k == "--thumbs" && i + 1 < argc) thumbs = argv[++i];
        else if (k == "--cond" && i + 1 < argc) cond = argv[++i];
        else if (k == "--auto" && i + 1 < argc) autoSec = QString(argv[++i]).toInt();
        else if (k == "--snap" && i + 1 < argc) snap = argv[++i];
        else if (k == "--snapsec" && i + 1 < argc) snapSec = QString(argv[++i]).toInt();
        else if (k == "--sec" && i + 1 < argc) sec = QString(argv[++i]).toInt();
    }
    Dashboard w(fps, clips, thumbs, cond, autoSec);
    w.showFullScreen();
    if (!snap.isEmpty()) scheduleSnap(&w, snap, snapSec);
    if (sec > 0) QTimer::singleShot(sec * 1000, &a, &QApplication::quit);
    return a.exec();
}
