/*
 * cockpit - V5-1 kiosk demo UI: simulated drone cockpit + lazy analysis HUD, drawn on a TRANSPARENT
 * full-screen window (overlay plane) over the NV12 video on the primary plane.
 *
 * Drawing policy (what our etnaviv2d QPA accelerates, see Part 3): opaque rect fills, pixmap blit / alpha /
 * scale = GPU; text, lines, translucent fills = CPU. So:
 *   - static art (compass strip, pitch ladder, HUD background, buttons) is pre-rendered once into QPixmaps
 *     and only blitted (GPU, rectangular clip);
 *   - bars are opaque rect fills (GPU);
 *   - text is drawn only when the shown value changes (HUD: 1 Hz, numbers: 5 Hz);
 *   - only dirty rectangles are repainted; everything else stays in the persistent backing store.
 *
 * Needs an alpha format (QT_ETNA2D_FMT=argb8888 or argb4444) to show the video through. RGB565 has no alpha (opaque).
 * argb4444 / argb1555 need the display driver patch in drivers/gpu/drm/nuvoton/ma35_plane.c (overlay alpha blending
 * was enabled only for ARGB8888 in the stock driver; the datasheet does not limit it).
 *
 * Usage: cockpit [--fps N] [--clip NAME] [--cond TEXT] [--fpslog FILE] [--sec N]
 *   --fps N       UI pace (default env QT2D_FPS, else 30)
 *   --clip NAME   video clip name shown in the HUD
 *   --cond TEXT   demo condition shown in the HUD (e.g. "kiosk-shell")
 *   --fpslog FILE gst-launch -v log of fpsdisplaysink; the last "current: X" is the video fps in the HUD
 *   --snap FILE   save the UI layer as PNG (with alpha) after --snapsec N seconds
 *   --sec N       exit after N seconds (default: run until killed)
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
#include <QFile>
#include <QRegularExpression>
#include <QtMath>
#include <cmath>
#include "../common/hud.h"

class Cockpit : public QWidget
{
public:
    Cockpit(int fps, const QString &clip, const QString &cond, const QString &fpslog)
        : m_fps(fps), m_clip(clip), m_cond(cond), m_fpslog(fpslog)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);        // we clear to transparent ourselves
        setAttribute(Qt::WA_NoSystemBackground);
        m_clock.start();
        QTimer *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this] { tick(); });
        t->start(1000 / qMax(1, m_fps));
    }

protected:
    void resizeEvent(QResizeEvent *) override { layoutAll(); update(); }

    void paintEvent(QPaintEvent *e) override
    {
        ++m_paints;
        QPainter p(this);
        const QVector<QRect> rects = e->region().rects();
        for (const QRect &r : rects) {
            p.save();
            p.setClipRect(r);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(r, Qt::transparent);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            drawAll(p, r);
            p.restore();
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        for (int i = 0; i < 3; ++i)
            if (m_btn[i].contains(e->pos())) { m_down = i; update(m_btn[i]); onButton(i); return; }
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_down >= 0) { const int d = m_down; m_down = -1; update(m_btn[d]); }
    }

private:
    // ---- simulation -------------------------------------------------------------------------------------
    void tick()
    {
        const double t = m_clock.elapsed() / 1000.0;
        const double dt = t - m_lastT; m_lastT = t;
        const double tgt = m_flying ? 120.0 : 0.0;       // altitude follows the takeoff/land button
        m_alt += qBound(-8.0 * dt, tgt - m_alt, 8.0 * dt);
        const double air = m_alt > 2.0 ? 1.0 : 0.0;
        m_speed = air * (18.0 + 6.0 * qSin(t * 0.5));
        m_pitch = air * 8.0 * qSin(t * 0.7);
        m_roll  = air * 20.0 * qSin(t * 0.4);
        if (m_rth) m_head += qBound(-30.0 * dt, -std::remainder(m_head, 360.0), 30.0 * dt);   // turn to heading 0
        else       m_head += air * (6.0 * qSin(t * 0.1) + 3.0) * dt;
        m_head = std::fmod(m_head + 360.0, 360.0);
        m_batt = qMax(5.0, 100.0 - t * 0.4);

        // dirty rectangles, only when the shown (quantised) value changes
        mark(m_alt_i, qRound(m_alt * 2),   m_rAlt);
        mark(m_spd_i, qRound(m_speed * 4), m_rSpd);
        mark(m_hd_i,  qRound(m_head * 2),  m_rComp);
        mark(m_pt_i,  qRound(m_pitch * 4), m_rAtt);
        mark(m_rl_i,  qRound(m_roll * 4),  m_rAtt);
        mark(m_bt_i,  qRound(m_batt),      m_rBatt);
        if (t - m_numT > 0.2) { m_numT = t; update(m_rAltTxt); update(m_rSpdTxt); update(m_rCompTxt); }
        if (t - m_hudT > 1.0) {
            const double fpsUi = m_hudT < 0 ? 0 : (m_paints - m_paints0) / (t - m_hudT);
            m_paints0 = m_paints; m_hudT = t; updateHud(fpsUi); update(m_rHud);
        }
    }
    void mark(int &prev, int cur, const QRect &r) { if (cur != prev) { prev = cur; update(r); } }

    void onButton(int i)
    {
        if (i == 0) m_flying = !m_flying;
        else if (i == 1) m_rth = !m_rth;
        else m_hudOn = !m_hudOn;
        update();
    }

    void updateHud(double fpsUi)
    {
        const HudStats h = m_samp.sample(m_fpslog);
        const double cpu = h.cpu, c0 = h.c0, c1 = h.c1; const long total = h.totalKb, avail = h.availKb, cma = h.cmaFreeKb; const QString vfps = h.videoFps;
        const QString fmt = qEnvironmentVariableIsSet("QT_ETNA2D_FMT") ? QString(qgetenv("QT_ETNA2D_FMT")).toUpper() : "ARGB8888";
        m_hud[0] = QString("VIDEO %1 fps    UI paints %2/s").arg(vfps).arg(fpsUi, 0, 'f', 0);
        m_hud[1] = QString("CPU %1 %  (c0 %2  c1 %3)").arg(cpu, 0, 'f', 0).arg(c0, 0, 'f', 0).arg(c1, 0, 'f', 0);
        m_hud[2] = QString("MEM %1 / %2 MB   CMA free %3 MB").arg((total - avail) / 1024).arg(total / 1024).arg(cma / 1024);
        m_hud[3] = QString("%1 | %2x%3 | %4").arg(m_cond).arg(width()).arg(height()).arg(m_clip);
        m_hud[4] = QString("primary NV12 video | overlay %1 UI").arg(fmt);
    }

    // ---- layout / pre-rendered art ----------------------------------------------------------------------
    int S(double v) const { return qRound(v * m_s); }

    void layoutAll()
    {
        m_s = height() / 600.0;
        const int W = width(), H = height();
        m_rHud   = QRect(S(10), S(10), S(280), S(94));
        m_rComp  = QRect(W / 2 - S(170), S(12), S(340), S(40));
        m_rCompTxt = QRect(W / 2 - S(40), S(54), S(80), S(24));
        m_rBatt  = QRect(W - S(150), S(12), S(138), S(30));
        m_rBattTxt = QRect(W - S(150), S(44), S(138), S(22));
        m_rAtt   = QRect(W / 2 - S(110), H / 2 - S(110), S(220), S(220));
        m_rAlt   = QRect(S(24), H / 2 - S(150), S(40), S(300));
        m_rAltTxt = QRect(S(10), H / 2 + S(156), S(140), S(26));
        m_rSpd   = QRect(W - S(64), H / 2 - S(150), S(40), S(300));
        m_rSpdTxt = QRect(W - S(170), H / 2 + S(156), S(160), S(26));
        for (int i = 0; i < 3; ++i)
            m_btn[i] = QRect(W / 2 - S(240) + i * S(165), H - S(70), S(150), S(52));

        QFont f; f.setPixelSize(qMax(8, S(15))); f.setBold(true); m_font = f;
        QFont h; h.setPixelSize(qMax(8, S(11))); m_hudFont = h;
        auto img = [](int w, int h) { QImage im(w, h, QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent); return im; };

        { QImage im = img(m_rHud.width(), m_rHud.height()); QPainter q(&im); q.setPen(Qt::NoPen);   // HUD panel: one alpha pixmap
          q.setRenderHint(QPainter::Antialiasing); q.setBrush(QColor(0, 0, 0, 150)); q.drawRoundedRect(im.rect(), S(8), S(8)); q.end();
          m_pmHud = QPixmap::fromImage(im); }

        { m_pxdeg = qMax(1, S(2)); QImage im = img(720 * m_pxdeg, m_rComp.height());                 // compass: two turns, wraps
          QPainter q(&im); q.setPen(QColor(255, 255, 255)); q.setFont(m_font);
          const char *n[] = {"N", "E", "S", "W"};
          for (int d = 0; d < 720; d += 5) {
              const int x = d * m_pxdeg; const bool big = d % 90 == 0, mid = d % 15 == 0;
              q.drawLine(x, 0, x, big ? S(14) : (mid ? S(9) : S(5)));
              if (big) q.drawText(QRect(x - S(15), S(16), S(30), S(20)), Qt::AlignCenter, n[(d / 90) % 4]);
          }
          q.end(); m_pmComp = QPixmap::fromImage(im); }

        { m_pxpitch = qMax(1, S(6)); const int h = 60 * m_pxpitch + m_rAtt.height();                 // pitch ladder -30..+30
          QImage im = img(m_rAtt.width(), h); QPainter q(&im); q.setPen(QColor(120, 255, 120)); q.setFont(m_font);
          for (int d = -30; d <= 30; d += 5) {
              const int y = h / 2 - d * m_pxpitch; const int hw = d % 10 == 0 ? S(50) : S(25);
              q.drawLine(im.width() / 2 - hw, y, im.width() / 2 + hw, y);
              if (d % 10 == 0 && d != 0) q.drawText(im.width() / 2 + S(54), y + S(5), QString::number(d));
          }
          q.end(); m_pmLadder = QPixmap::fromImage(im); }

        const char *names[4] = {"TAKEOFF", "RTH", "HUD", "LAND"};
        for (int i = 0; i < 4; ++i)
            for (int st = 0; st < 2; ++st) {
                const QRect b = m_btn[i < 3 ? i : 0];
                QImage im = img(b.width(), b.height()); QPainter q(&im); q.setRenderHint(QPainter::Antialiasing);
                q.setPen(QPen(Qt::white, S(2)));
                q.setBrush(i == 3 ? QColor(200, 50, 50, 210) : (st ? QColor(230, 120, 30, 230) : QColor(30, 110, 190, 200)));
                q.drawRoundedRect(im.rect().adjusted(2, 2, -2, -2), S(12), S(12));
                q.setFont(m_font); q.drawText(im.rect(), Qt::AlignCenter, names[i]); q.end();
                m_pmBtn[i][st] = QPixmap::fromImage(im);
            }
        updateHud(0);
    }

    // ---- drawing ----------------------------------------------------------------------------------------
    void drawAll(QPainter &p, const QRect &r)
    {
        p.setFont(m_font);
        if (m_hudOn && r.intersects(m_rHud)) {
            p.drawPixmap(m_rHud.topLeft(), m_pmHud);
            p.setPen(Qt::white); p.setFont(m_hudFont);
            for (int i = 0; i < 5; ++i) p.drawText(m_rHud.x() + S(8), m_rHud.y() + S(18) + i * S(16), m_hud[i]);
            p.setFont(m_font);
        }
        if (r.intersects(m_rComp)) {                     // compass: scrolling strip (blit, rect clip)
            const int off = qRound(m_head * m_pxdeg) - m_rComp.width() / 2;
            p.save(); p.setClipRect(m_rComp.intersected(r));
            p.drawPixmap(m_rComp.x() - off, m_rComp.y(), m_pmComp);
            p.restore();
            p.fillRect(QRect(m_rComp.center().x() - 1, m_rComp.y(), S(3), S(22)), QColor(255, 200, 0));
        }
        if (r.intersects(m_rCompTxt)) { p.setPen(Qt::white); p.drawText(m_rCompTxt, Qt::AlignCenter, QString("%1 deg").arg(qRound(m_head))); }
        if (r.intersects(m_rBatt) || r.intersects(m_rBattTxt)) {
            { const QRect t = m_rBatt; const int b = S(3);       // white frame only, inside transparent
              for (const QRect &f : {QRect(t.x(), t.y(), t.width(), b), QRect(t.x(), t.bottom() - b + 1, t.width(), b),
                                     QRect(t.x(), t.y(), b, t.height()), QRect(t.right() - b + 1, t.y(), b, t.height())})
                  p.fillRect(f, QColor(255, 255, 255)); }
            QRect fill = m_rBatt.adjusted(S(5), S(5), -S(5), -S(5)); fill.setWidth(qRound(fill.width() * m_batt / 100.0));
            p.fillRect(fill, m_batt > 30 ? QColor(60, 220, 90) : QColor(230, 70, 60));
            p.setPen(Qt::white); p.drawText(m_rBattTxt, Qt::AlignCenter, QString("BAT %1 %").arg(qRound(m_batt)));
        }
        if (r.intersects(m_rAtt)) {                       // attitude: pitch ladder (blit) + reticle + roll marker
            const int y = m_rAtt.y() - (m_pmLadder.height() - m_rAtt.height()) / 2 + qRound(m_pitch * m_pxpitch);
            p.save(); p.setClipRect(m_rAtt.intersected(r));
            p.drawPixmap(m_rAtt.x(), y, m_pmLadder);
            p.restore();
            const QPoint c = m_rAtt.center(); const QColor Y(255, 200, 0);
            p.fillRect(QRect(c.x() - S(40), c.y() - 1, S(26), S(3)), Y);
            p.fillRect(QRect(c.x() + S(14), c.y() - 1, S(26), S(3)), Y);
            p.fillRect(QRect(c.x() - 1, c.y() - S(8), S(3), S(16)), Y);
            const int rx = c.x() + qRound(m_roll / 30.0 * m_rAtt.width() / 2.2);      // roll scale (marker slides)
            p.fillRect(QRect(m_rAtt.x(), m_rAtt.bottom() - S(10), m_rAtt.width(), S(2)), QColor(255, 255, 255));
            p.fillRect(QRect(rx - S(3), m_rAtt.bottom() - S(18), S(7), S(16)), Y);
        }
        tape(p, r, m_rAlt, m_alt / 150.0, QColor(80, 180, 255));
        tape(p, r, m_rSpd, m_speed / 30.0, QColor(255, 170, 60));
        if (r.intersects(m_rAltTxt)) { p.setPen(Qt::white); p.drawText(m_rAltTxt, Qt::AlignLeft | Qt::AlignVCenter, QString("ALT %1 m").arg(m_alt, 0, 'f', 0)); }
        if (r.intersects(m_rSpdTxt)) { p.setPen(Qt::white); p.drawText(m_rSpdTxt, Qt::AlignRight | Qt::AlignVCenter, QString("SPD %1 m/s").arg(m_speed, 0, 'f', 0)); }
        for (int i = 0; i < 3; ++i) if (r.intersects(m_btn[i]))
            p.drawPixmap(m_btn[i].topLeft(), (i == 0 && m_flying) ? m_pmBtn[3][0] : m_pmBtn[i][m_down == i ? 1 : 0]);
    }
    void tape(QPainter &p, const QRect &r, const QRect &t, double frac, const QColor &c)
    {
        if (!r.intersects(t)) return;
        const int b = S(3);                                      // white frame only: the inside stays transparent
        for (const QRect &f : {QRect(t.x(), t.y(), t.width(), b), QRect(t.x(), t.bottom() - b + 1, t.width(), b),
                               QRect(t.x(), t.y(), b, t.height()), QRect(t.right() - b + 1, t.y(), b, t.height())})
            p.fillRect(f.intersected(r), QColor(255, 255, 255));
        const QRect in = t.adjusted(S(5), S(5), -S(5), -S(5));
        const int h = qRound(in.height() * qBound(0.0, frac, 1.0));
        p.fillRect(QRect(in.x(), in.bottom() - h + 1, in.width(), h), c);
    }

    int m_fps; QString m_clip, m_cond, m_fpslog;
    QElapsedTimer m_clock; double m_lastT = 0, m_numT = 0, m_hudT = -1;
    quint64 m_paints = 0, m_paints0 = 0; HudSampler m_samp;
    bool m_flying = true, m_rth = false, m_hudOn = true; int m_down = -1;
    double m_alt = 0, m_speed = 0, m_pitch = 0, m_roll = 0, m_head = 0, m_batt = 100, m_s = 1;
    int m_alt_i = -1, m_spd_i = -1, m_hd_i = -1, m_pt_i = -1, m_rl_i = -1, m_bt_i = -1, m_pxdeg = 2, m_pxpitch = 6;
    QRect m_rHud, m_rComp, m_rCompTxt, m_rBatt, m_rBattTxt, m_rAtt, m_rAlt, m_rAltTxt, m_rSpd, m_rSpdTxt, m_btn[3];
    QPixmap m_pmHud, m_pmComp, m_pmLadder, m_pmBtn[4][2];
    QFont m_font, m_hudFont; QString m_hud[5];
};

int main(int argc, char **argv)
{
    QApplication a(argc, argv);
    int fps = qEnvironmentVariableIntValue("QT2D_FPS"); if (fps <= 0) fps = 30;
    int sec = 0, snapSec = 0; QString snap, clip = "-", cond = "kiosk-shell", fpslog;
    for (int i = 1; i < argc; ++i) {
        const QString k = argv[i];
        if (k == "--fps" && i + 1 < argc) fps = QString(argv[++i]).toInt();
        else if (k == "--clip" && i + 1 < argc) clip = argv[++i];
        else if (k == "--cond" && i + 1 < argc) cond = argv[++i];
        else if (k == "--fpslog" && i + 1 < argc) fpslog = argv[++i];
        else if (k == "--snap" && i + 1 < argc) snap = argv[++i];
        else if (k == "--snapsec" && i + 1 < argc) snapSec = QString(argv[++i]).toInt();
        else if (k == "--sec" && i + 1 < argc) sec = QString(argv[++i]).toInt();
    }
    Cockpit w(fps, clip, cond, fpslog);
    w.showFullScreen();
    if (!snap.isEmpty()) scheduleSnap(&w, snap, snapSec);   // UI layer as PNG with alpha
    if (sec > 0) QTimer::singleShot(sec * 1000, &a, &QApplication::quit);
    return a.exec();
}
