// SPDX-License-Identifier: BSD-3-Clause
//
// Small line icons drawn in code (timer, histogram, gear) so the Photon-style bars do
// not depend on image assets. Strokes scale with the item size.
import QtQuick 2.0
import com.vivid.camera 1.0

Canvas {
    id: icon

    property string kind: "gear"
    property color color: PhotonTheme.textPrimary

    implicitWidth: 24 * PhotonTheme.dp
    implicitHeight: 24 * PhotonTheme.dp

    onKindChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()
    onAvailableChanged: if (available) requestPaint()

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        var w = width, h = height
        var cx = w / 2, cy = h / 2
        var line = Math.max(1.5, w * 0.085)
        ctx.strokeStyle = icon.color
        ctx.fillStyle = icon.color
        ctx.lineWidth = line
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        if (kind === "timer") {
            var r = w * 0.34
            ctx.beginPath()
            ctx.arc(cx, cy + h * 0.06, r, 0, 2 * Math.PI)
            ctx.stroke()
            ctx.beginPath()                        // crown
            ctx.moveTo(cx - w * 0.1, cy - r - h * 0.1 + h * 0.06)
            ctx.lineTo(cx + w * 0.1, cy - r - h * 0.1 + h * 0.06)
            ctx.stroke()
            ctx.beginPath()                        // hand
            ctx.moveTo(cx, cy + h * 0.06)
            ctx.lineTo(cx + r * 0.55, cy + h * 0.06 - r * 0.45)
            ctx.stroke()
        } else if (kind === "histogram") {
            var bw = w * 0.16, base = h * 0.84
            var heights = [ 0.34, 0.62, 0.46 ]
            var xs = [ w * 0.18, w * 0.42, w * 0.66 ]
            for (var i = 0; i < 3; ++i)
                ctx.fillRect(xs[i], base - h * heights[i], bw, h * heights[i])
        } else if (kind === "flash" || kind === "flashoff" || kind === "flashlight") {
            ctx.beginPath()
            ctx.moveTo(w * 0.58, h * 0.08)
            ctx.lineTo(w * 0.28, h * 0.55)
            ctx.lineTo(w * 0.48, h * 0.55)
            ctx.lineTo(w * 0.40, h * 0.92)
            ctx.lineTo(w * 0.74, h * 0.42)
            ctx.lineTo(w * 0.54, h * 0.42)
            ctx.closePath()
            ctx.fill()
            if (kind === "flashlight") {
                ctx.beginPath()
                ctx.arc(cx, cy, w * 0.46, 0, 2 * Math.PI)
                ctx.stroke()
            }
            if (kind === "flashoff") {
                ctx.beginPath()
                ctx.moveTo(w * 0.16, h * 0.14)
                ctx.lineTo(w * 0.84, h * 0.90)
                ctx.stroke()
            }
        } else if (kind === "flip") {              // two arrows chasing round a circle
            var fr = w * 0.34
            var arrow = function (a0, a1) {
                ctx.beginPath()
                ctx.arc(cx, cy, fr, a0, a1, false)
                ctx.stroke()
                var ex = cx + Math.cos(a1) * fr, ey = cy + Math.sin(a1) * fr
                var tx = -Math.sin(a1), ty = Math.cos(a1)       // tangent
                var nx = Math.cos(a1), ny = Math.sin(a1)        // outward
                var hs = w * 0.14
                ctx.beginPath()
                ctx.moveTo(ex + tx * hs, ey + ty * hs)
                ctx.lineTo(ex + nx * hs * 0.8, ey + ny * hs * 0.8)
                ctx.lineTo(ex - nx * hs * 0.8, ey - ny * hs * 0.8)
                ctx.closePath()
                ctx.fill()
            }
            arrow(Math.PI * 0.1, Math.PI * 0.85)
            arrow(Math.PI * 1.1, Math.PI * 1.85)
        } else {                                   // gear
            var teeth = 8
            var ro = w * 0.46, ri = w * 0.34
            ctx.beginPath()
            for (var t = 0; t < teeth; ++t) {
                var a = t * 2 * Math.PI / teeth
                var pts = [ [ri, -0.2], [ro, -0.12], [ro, 0.12], [ri, 0.2] ]
                for (var p = 0; p < 4; ++p) {
                    var ang = a + pts[p][1]
                    var x = cx + Math.cos(ang) * pts[p][0]
                    var y = cy + Math.sin(ang) * pts[p][0]
                    if (t === 0 && p === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                }
            }
            ctx.closePath()
            ctx.arc(cx, cy, w * 0.14, 0, 2 * Math.PI, true)   // hole
            ctx.fill()
        }
    }
}
