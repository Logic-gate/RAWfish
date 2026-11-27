// SPDX-License-Identifier: BSD-3-Clause
//
// Timestamped startup markers ("ms since the process started") written to
// ~/Pictures/RAWfish/rawfish-startup.log, to find out where launch time goes.
#ifndef STARTUPLOG_H
#define STARTUPLOG_H

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>

#include <unistd.h>

namespace StartupLog {

inline double processAgeMs()
{
    // Field 22 of /proc/self/stat is the start time in clock ticks since boot.
    QFile stat(QStringLiteral("/proc/self/stat"));
    QFile uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray statData = stat.readAll();
    const int close = statData.lastIndexOf(')');
    const QList<QByteArray> fields = statData.mid(close + 2).split(' ');
    if (fields.size() < 20)
        return -1;
    const double start = fields.at(19).toDouble() / double(sysconf(_SC_CLK_TCK));
    const double now = uptime.readAll().split(' ').value(0).toDouble();
    return (now - start) * 1000.0;
}

inline void mark(const QString &event)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
            + QStringLiteral("/RAWfish");
    QDir().mkpath(dir);
    QFile file(dir + QStringLiteral("/rawfish-startup.log"));
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    file.write(QStringLiteral("%1 +%2ms %3\n")
               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
               .arg(qRound(processAgeMs()))
               .arg(event).toUtf8());
}

}

#endif
