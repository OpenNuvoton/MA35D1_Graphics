// Shared by the V5-1 (cockpit) and V5-2 (dashboard) demos: analysis HUD data and the --snap helper.
#pragma once
#include <QFile>
#include <QList>
#include <QPixmap>
#include <QRegularExpression>
#include <QString>
#include <QTimer>
#include <QWidget>

struct HudStats { double cpu = 0, c0 = 0, c1 = 0; long totalKb = 0, availKb = 0, cmaFreeKb = 0; QString videoFps = "-"; };

// Reads /proc/stat (CPU total and per core since the previous call), /proc/meminfo (memory, CMA free) and the
// last "current: X" of a gst `fpsdisplaysink -v` log (video fps; "-" if the log is empty or missing).
class HudSampler
{
public:
    HudStats sample(const QString &fpslog)
    {
        HudStats h;
        QFile s("/proc/stat");
        QList<QByteArray> lines;
        if (s.open(QIODevice::ReadOnly)) lines = s.readAll().split('\n');
        h.cpu = cpuPct(lines, "cpu", 0);
        h.c0  = cpuPct(lines, "cpu0", 1);
        h.c1  = cpuPct(lines, "cpu1", 2);
        QFile f("/proc/meminfo");
        if (f.open(QIODevice::ReadOnly))
            for (const QByteArray &l : f.readAll().split('\n')) {
                if (l.startsWith("MemTotal:")) h.totalKb = l.simplified().split(' ')[1].toLong();
                else if (l.startsWith("MemAvailable:")) h.availKb = l.simplified().split(' ')[1].toLong();
                else if (l.startsWith("CmaFree:")) h.cmaFreeKb = l.simplified().split(' ')[1].toLong();
            }
        if (!fpslog.isEmpty()) {
            QFile g(fpslog);
            if (g.open(QIODevice::ReadOnly)) {
                if (g.size() > 8192) g.seek(g.size() - 8192);
                const QString txt = QString::fromLatin1(g.readAll());
                QRegularExpressionMatchIterator it = QRegularExpression("current: ([0-9.]+)").globalMatch(txt);
                QString last; while (it.hasNext()) last = it.next().captured(1);
                if (!last.isEmpty()) h.videoFps = last;
            }
        }
        return h;
    }

private:
    double cpuPct(const QList<QByteArray> &lines, const QByteArray &name, int slot)
    {
        for (const QByteArray &l : lines) {
            const QList<QByteArray> p = l.simplified().split(' ');
            if (p.isEmpty() || p[0] != name) continue;
            quint64 t = 0, idle = 0;
            for (int i = 1; i < p.size() && i < 9; ++i) { t += p[i].toULongLong(); if (i == 4 || i == 5) idle += p[i].toULongLong(); }
            const quint64 dt = t - m_t[slot], di = idle - m_i[slot];
            m_t[slot] = t; m_i[slot] = idle;
            return dt ? 100.0 * (dt - di) / dt : 0;
        }
        return 0;
    }
    quint64 m_t[3] = {0, 0, 0}, m_i[3] = {0, 0, 0};
};

// Save the widget as PNG (alpha kept) after `sec` seconds.
inline void scheduleSnap(QWidget *w, const QString &file, int sec)
{
    QTimer::singleShot(sec * 1000, w, [w, file] { QPixmap pm(w->size()); pm.fill(Qt::transparent); w->render(&pm); pm.save(file); });
}
