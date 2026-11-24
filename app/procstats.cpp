/*
 * procstats.cpp -- see procstats.h.
 */
#include "procstats.h"
#include <QFile>
#include <QDir>
#include <QStringList>
#include <unistd.h>

static QByteArray readSmall(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.read(8192);
}

/* "VmRSS:     50540 kB" -> 50540 */
static long statusKb(const QByteArray &status, const char *key)
{
    const int at = status.indexOf(key);
    if (at < 0) return -1;
    int i = at + int(qstrlen(key));
    while (i < status.size() && (status[i] == ':' || status[i] == ' ' || status[i] == '\t')) ++i;
    int j = i;
    while (j < status.size() && status[j] >= '0' && status[j] <= '9') ++j;
    return j > i ? status.mid(i, j - i).toLong() : -1;
}

void ProcStats::reset()
{
    m_ticks.clear();
    m_sysBusy = m_sysTotal = 0;
    m_clock.invalidate();
}

QString ProcStats::proc(qint64 pid, const char *name, double secs)
{
    const QString base = QStringLiteral("/proc/%1/").arg(pid);
    const QByteArray stat = readSmall(base + QStringLiteral("stat"));
    const int rp = stat.lastIndexOf(')');
    if (rp < 0) return QStringLiteral("%1 gone").arg(QLatin1String(name));
    /* after "pid (comm) ": state is field 3; utime 14, stime 15, threads 20 */
    const QList<QByteArray> f = stat.mid(rp + 2).split(' ');
    if (f.size() < 18) return QStringLiteral("%1 ?").arg(QLatin1String(name));
    const quint64 ticks = f[11].toULongLong() + f[12].toULongLong();
    const int threads = f[17].toInt();
    QString cpu = QStringLiteral("-");
    if (m_ticks.contains(pid) && secs > 0) {
        static const double hz = double(sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100);
        cpu = QString::number(100.0 * double(ticks - m_ticks.value(pid)) / hz / secs, 'f', 1) + QLatin1Char('%');
    }
    m_ticks.insert(pid, ticks);
    const QByteArray status = readSmall(base + QStringLiteral("status"));
    return QStringLiteral("%1 cpu=%2 rss=%3MB hwm=%4MB thr=%5")
            .arg(QLatin1String(name), cpu)
            .arg(statusKb(status, "VmRSS") / 1024.0, 0, 'f', 1)
            .arg(statusKb(status, "VmHWM") / 1024.0, 0, 'f', 1)
            .arg(threads);
}

QString ProcStats::line(qint64 enginePid, const QString &extra)
{
    const double secs = m_clock.isValid() ? m_clock.nsecsElapsed() / 1e9 : 0.0;
    m_clock.start();
    QStringList parts;
    if (!extra.isEmpty()) parts << extra;
    parts << proc(getpid(), "app", secs);
    if (enginePid > 0) parts << proc(enginePid, "engine", secs);
    else parts << QStringLiteral("engine none");

    /* the whole system: the "cpu" line of /proc/stat (idle = idle + iowait) */
    const QByteArray ps = readSmall(QStringLiteral("/proc/stat"));
    const int nl = ps.indexOf('\n');
    const QList<QByteArray> c = ps.left(nl).simplified().split(' ');
    if (c.size() >= 6 && c[0] == "cpu") {
        quint64 total = 0;
        for (int i = 1; i < c.size() && i <= 8; ++i) total += c[i].toULongLong();
        const quint64 idle = c[4].toULongLong() + c[5].toULongLong();
        const quint64 busy = total - idle;
        int cores = 0;
        for (const QByteArray &l : ps.split('\n')) if (l.startsWith("cpu") && l.size() > 3 && l[3] != ' ') ++cores;
        if (m_sysTotal && total > m_sysTotal)
            parts << QStringLiteral("system cpu=%1% of %2 cores")
                     .arg(100.0 * double(busy - m_sysBusy) / double(total - m_sysTotal), 0, 'f', 1).arg(cores);
        m_sysBusy = busy; m_sysTotal = total;
    }

    /* the battery: the first power supply of type Battery (current in uA,
     * voltage in uV; the sign of the current depends on the driver) */
    const QDir ps_dir(QStringLiteral("/sys/class/power_supply"));
    for (const QString &e : ps_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString d = ps_dir.filePath(e) + QLatin1Char('/');
        if (readSmall(d + QStringLiteral("type")).trimmed() != "Battery") continue;
        const QByteArray cur = readSmall(d + QStringLiteral("current_now")).trimmed();
        const QByteArray volt = readSmall(d + QStringLiteral("voltage_now")).trimmed();
        QString b = QStringLiteral("battery");
        if (!cur.isEmpty()) b += QStringLiteral(" %1mA").arg(cur.toLongLong() / 1000);
        if (!volt.isEmpty()) b += QStringLiteral(" %1V").arg(volt.toLongLong() / 1e6, 0, 'f', 2);
        const QByteArray cap = readSmall(d + QStringLiteral("capacity")).trimmed();
        if (!cap.isEmpty()) b += QStringLiteral(" %1%").arg(QString::fromLatin1(cap));
        const QByteArray st = readSmall(d + QStringLiteral("status")).trimmed();
        if (!st.isEmpty()) b += QLatin1Char(' ') + QString::fromLatin1(st).toLower();
        parts << b;
        break;
    }
    return parts.join(QStringLiteral(" | "));
}
