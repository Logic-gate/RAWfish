// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include "imageadjustments.h"

#include <QColor>
#include <QtGlobal>

#include <cmath>

namespace {

int adjustedByte(int value, qreal factor)
{
    return qBound(0, int(std::lround(value * factor)), 255);
}

}

namespace ImageAdjustments {

bool requested(qreal exposure, int colorTemperature, int colorTint)
{
    return !qFuzzyCompare(exposure, 1.0) ||
           (colorTemperature > 0 && colorTemperature != 5500) ||
           colorTint != 0;
}

void apply(QImage *image, qreal exposure, int colorTemperature, int colorTint)
{
    if (!image || image->isNull() ||
            !requested(exposure, colorTemperature, colorTint)) {
        return;
    }

    if (image->format() != QImage::Format_RGB32 &&
            image->format() != QImage::Format_ARGB32) {
        *image = image->convertToFormat(QImage::Format_RGB32);
    }

    qreal red = qBound<qreal>(0.25, exposure, 32.0);
    qreal green = red;
    qreal blue = red;

    if (colorTemperature > 0 && colorTemperature != 5500) {
        const qreal warmth = qBound<qreal>(
                    -1.0, (5500.0 - colorTemperature) / 4500.0, 1.0);
        red *= 1.0 + warmth * 0.35;
        blue *= 1.0 - warmth * 0.35;
    }

    if (colorTint != 0) {
        const qreal magenta = qBound<qreal>(-1.0, colorTint / 200.0, 1.0);
        red *= 1.0 + magenta * 0.18;
        blue *= 1.0 + magenta * 0.18;
        green *= 1.0 - magenta * 0.18;
    }

    for (int y = 0; y < image->height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image->scanLine(y));
        for (int x = 0; x < image->width(); ++x) {
            line[x] = qRgb(adjustedByte(qRed(line[x]), red),
                           adjustedByte(qGreen(line[x]), green),
                           adjustedByte(qBlue(line[x]), blue));
        }
    }
}

}
