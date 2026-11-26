/*
 * procstats.h -- CPU, memory and battery figures for the log, read from
 * /proc and /sys: what `top` and `grep VmRSS /proc/PID/status` show, taken
 * at the same moments for the app and the engine, so that two settings can
 * be compared from the log alone (rawfish-ir.conf statsLog).
 */
#ifndef PROCSTATS_H
#define PROCSTATS_H

#include <QString>
#include <QElapsedTimer>
#include <QHash>

class ProcStats
{
public:
    /* One log line: each process's CPU since the previous call (100 % = one
     * core, as top), its RSS, peak RSS and threads; the whole system's CPU
     * (100 % = all cores); the battery's current and voltage if readable.
     * enginePid 0: the app only. extra goes in front (the mode, the fps). */
    QString line(qint64 enginePid, const QString &extra);
    void reset();                  /* the next line starts a new interval   */

private:
    QString proc(qint64 pid, const char *name, double secs);
    QHash<qint64, quint64> m_ticks;        /* utime+stime at the last line   */
    quint64 m_sysBusy = 0, m_sysTotal = 0; /* /proc/stat at the last line    */
    QElapsedTimer m_clock;
};

#endif
