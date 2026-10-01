// SPDX-License-Identifier: BSD-3-Clause
#include "../src/previewaids.h"
#include <cassert>

int main()
{
    QImage flat(24, 16, QImage::Format_RGB888);
    flat.fill(qRgb(100, 100, 100));
    const QImage original = flat.copy();
    assert(PreviewAids::apply(flat, false, false) == flat);
    assert(PreviewAids::apply(flat, true, true) == flat);
    QImage edge = flat.copy();
    for (int y = 0; y < edge.height(); ++y)
        for (int x = 12; x < edge.width(); ++x) edge.setPixel(x, y, qRgb(255, 255, 255));
    const QImage saved = edge.copy();
    const QImage peak = PreviewAids::apply(edge, true, false);
    assert(peak.pixel(11, 8) == qRgb(255, 48, 48));
    assert(peak.pixel(5, 8) == edge.pixel(5, 8));
    const QImage zebra = PreviewAids::apply(edge, false, true);
    assert(zebra.pixel(16, 8) == qRgb(32, 32, 32));
    assert(zebra.pixel(12, 8) == edge.pixel(12, 8));
    const QImage both = PreviewAids::apply(edge, true, true);
    assert(both.pixel(12, 8) == zebra.pixel(12, 8));
    assert(both.pixel(11, 8) == peak.pixel(11, 8));
    assert(flat == original && edge == saved);
    flat.fill(qRgb(242, 242, 242));
    assert(PreviewAids::apply(flat, false, true) == flat);
    flat.fill(qRgb(243, 243, 243));
    assert(PreviewAids::apply(flat, false, true).pixel(0, 0) == qRgb(32, 32, 32));
    assert(PreviewAids::apply(QImage(), true, true).isNull());
    QImage tiny(1, 1, QImage::Format_RGB888);
    tiny.fill(qRgb(100, 100, 100));
    assert(PreviewAids::apply(tiny, true, false) == tiny);
}
