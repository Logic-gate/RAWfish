// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <QImage>
#include <QVector>
#include <cstdlib>

namespace PreviewAids {
// Thresholds refer to the displayed RGB preview, not RAW sensor clipping.
inline QImage apply(const QImage &source, bool peaking, bool zebras)
{
    if (source.isNull() || (!peaking && !zebras)) return source;
    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    const int width = rgb.width(), height = rgb.height();
    QVector<int> luminance;
    if (peaking) {
        luminance.resize(width * height);
        for (int y = 0; y < height; ++y) {
            const uchar *row = rgb.constScanLine(y);
            for (int x = 0; x < width; ++x)
                luminance[y * width + x] = (77 * row[3*x] + 150 * row[3*x+1] + 29 * row[3*x+2]) >> 8;
        }
    }
    QImage result = rgb.copy();
    for (int y = 0; y < height; ++y) {
        const uchar *input = rgb.constScanLine(y);
        uchar *output = result.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const int brightness = (77 * input[3*x] + 150 * input[3*x+1] + 29 * input[3*x+2]) >> 8;
            if (zebras && brightness >= 243) {
                // Keep zebra priority even in the clear half of each stripe.
                if (((x + y) / 6) % 2 == 0)
                    output[3*x] = output[3*x+1] = output[3*x+2] = 32;
                continue;
            }
            if (!peaking || x == 0 || y == 0 || x == width-1 || y == height-1) continue;
            const int i = y * width + x;
            const int gx = -luminance[i-width-1] + luminance[i-width+1]
                           -2*luminance[i-1] + 2*luminance[i+1]
                           -luminance[i+width-1] + luminance[i+width+1];
            const int gy = -luminance[i-width-1] - 2*luminance[i-width] - luminance[i-width+1]
                           +luminance[i+width-1] + 2*luminance[i+width] + luminance[i+width+1];
            if (std::abs(gx) + std::abs(gy) >= 320) {
                output[3*x] = 255;
                output[3*x+1] = 48;
                output[3*x+2] = 48;
            }
        }
    }
    return result;
}
}
