/*
 * LensFixIcon.qml -- the Lens fix column's icon, drawn (the theme has none):
 * a 4 x 4 checkerboard, every other square filled; bent like a barrel-
 * distorted lens draws it for "off", straight for "on". Monochrome in the
 * colour the column gives (as the theme's icons are tinted), the size of a
 * theme icon. Loaded by MenuColumn (iconComponent): its parent, the
 * column's Loader, gives `value` (the column's value: true = on) and `tint`.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Canvas {
    id: icon
    width: Theme.iconSizeMedium; height: width
    readonly property bool straight: !!(parent && parent.value)
    readonly property color tint: parent && parent.tint !== undefined ? parent.tint : Theme.lightPrimaryColor
    onTintChanged: requestPaint()
    onStraightChanged: requestPaint()
    onWidthChanged: requestPaint()
    renderStrategy: Canvas.Immediate

    // A point of the board (-1..1 both ways) -> the icon's pixels: as drawn
    // by a barrel-distorted lens (the corners pulled in most, the edges bow
    // out) unless straight.
    function map(x, y) {
        var k = straight ? 0.0 : 0.17
        var f = 1.0 - k * (x * x + y * y)
        var half = width * 0.36
        return { x: width / 2 + x * f * half, y: height / 2 + y * f * half }
    }
    // The outline of the board rectangle x0..x1, y0..y1, its sides cut in
    // steps so that they bend.
    function path(ctx, x0, y0, x1, y1) {
        var n = 6, i, p
        p = map(x0, y0); ctx.moveTo(p.x, p.y)
        for (i = 1; i <= n; ++i) { p = map(x0 + (x1 - x0) * i / n, y0); ctx.lineTo(p.x, p.y) }
        for (i = 1; i <= n; ++i) { p = map(x1, y0 + (y1 - y0) * i / n); ctx.lineTo(p.x, p.y) }
        for (i = 1; i <= n; ++i) { p = map(x1 - (x1 - x0) * i / n, y1); ctx.lineTo(p.x, p.y) }
        for (i = 1; i <= n; ++i) { p = map(x0, y1 - (y1 - y0) * i / n); ctx.lineTo(p.x, p.y) }
        ctx.closePath()
    }
    onPaint: {
        var ctx = getContext("2d"), cells = 4, s = 2.0 / cells, r, c
        ctx.reset()
        ctx.fillStyle = tint
        ctx.strokeStyle = tint
        ctx.lineJoin = "round"
        ctx.lineWidth = Math.max(1, Math.round(width / 28))
        ctx.beginPath()
        for (r = 0; r < cells; ++r)
            for (c = 0; c < cells; ++c)
                if ((r + c) % 2 === 0) path(ctx, -1 + c * s, -1 + r * s, -1 + (c + 1) * s, -1 + (r + 1) * s)
        ctx.fill()
        ctx.beginPath()
        path(ctx, -1, -1, 1, 1)                      // the board's edge
        ctx.stroke()
    }
}
