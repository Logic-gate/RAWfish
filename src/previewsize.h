#ifndef PREVIEWSIZE_H
#define PREVIEWSIZE_H

#include <QSize>
#include <QStringList>
#include <cmath>
#include <tuple>

// Only select sizes present in both the real HAL and the effective profile.
inline QSize preferredPreviewSize(const QStringList &advertised,
                                 const QStringList &effective)
{
    QSize best;
    std::tuple<int, double, qint64, int, int> bestRank;
    for (const QString &text : effective) {
        if (!advertised.contains(text))
            continue;
        const QStringList parts = text.split(QLatin1Char('x'));
        if (parts.size() != 2)
            continue;
        const QSize size(parts[0].toInt(), parts[1].toInt());
        if (size.width() <= 0 || size.height() <= 0)
            continue;
        if (size == QSize(1280, 960))
            return size;
        const qint64 area = qint64(size.width()) * size.height();
        const bool large = area > 1440 * 1080;
        const auto rank = std::make_tuple(
                    large ? 1 : 0,
                    large ? 0.0 : std::abs(double(size.width()) / size.height() - 4.0 / 3.0),
                    large ? area : qAbs(area - qint64(1280 * 960)),
                    size.width(), size.height());
        if (!best.isValid() || rank < bestRank) {
            best = size;
            bestRank = rank;
        }
    }
    return best;
}

#endif
