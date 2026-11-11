/*
 * FocusCircle.qml -- the metering/focus circle drawn where the user tapped,
 * styled by the AF outcome the engine reports (CONTROL_AF_STATE in the SF2M
 * "ae" line):
 *   searching (scan running)       : dotted
 *   locked, not sharp / impossible : dashed      (NOT_FOCUSED_LOCKED)
 *   locked, sharp (ok)             : solid
 * While the engine has not answered yet the circle is plain white dotted.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Canvas {
    id: circle
    property int afState: -1       // the controller's circle state (fcstate)
    property color colour: "white"
    property bool sun: false       // light-only reference: a sun, not a circle
    // The focus marker is a SQUARE of the size of the focus region itself (the
    // engine's af_region: a square, 5 % of the frame's short side), so that
    // what is framed is what the camera focuses on. 0 = the old fixed circle.
    property real regionSide: 0
    readonly property real lineW: Math.max(2, Math.round(2 * Theme.pixelRatio))

    // Camera2: 0 INACTIVE, 1 PASSIVE_SCAN, 2 PASSIVE_FOCUSED, 3 ACTIVE_SCAN,
    // 4 FOCUSED_LOCKED, 5 NOT_FOCUSED_LOCKED, 6 PASSIVE_UNFOCUSED.
    readonly property string style: (afState === 4 || afState === 2) ? "solid"
                                   : (afState === 5 || afState === 6) ? "dashed" : "dotted"

    width: !sun && regionSide > 0 ? regionSide + 2 * lineW : Theme.itemSizeMedium; height: width
    onWidthChanged: requestPaint()
    onStyleChanged: requestPaint()
    onColourChanged: requestPaint()
    onSunChanged: requestPaint()
    // A Canvas created hidden is not always painted when it is first shown
    // (the sun, hidden until a touch is held, sometimes stayed blank): it is
    // asked to paint each time it becomes visible or available.
    onVisibleChanged: if (visible) requestPaint()
    onAvailableChanged: if (available) requestPaint()
    onPaint: {
        var ctx = getContext("2d")
        ctx.clearRect(0, 0, width, height)
        ctx.lineWidth = lineW
        ctx.strokeStyle = colour
        var r = width / 2 - lineW, cx = width / 2, cy = height / 2
        if (sun) {
            // A small disc with eight rays: "only the light is taken here".
            ctx.beginPath(); ctx.arc(cx, cy, r * 0.35, 0, 2 * Math.PI); ctx.stroke()
            for (var k = 0; k < 8; ++k) {
                var a = k * Math.PI / 4
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(a) * r * 0.55, cy + Math.sin(a) * r * 0.55)
                ctx.lineTo(cx + Math.cos(a) * r * 0.95, cy + Math.sin(a) * r * 0.95)
                ctx.stroke()
            }
            return
        }
        if (regionSide > 0) {
            // The focus region: a square, its edge drawn just outside it.
            var x0 = lineW / 2, s = width - lineW
            if (style === "solid") { ctx.strokeRect(x0, x0, s, s); return }
            // Dotted (searching) or dashed (failed): segments along the four sides.
            var seg = style === "dotted" ? s / 12 : s / 4, onf = style === "dotted" ? 0.35 : 0.6
            for (var side = 0; side < 4; ++side)
                for (var t = 0; t < s - 0.5; t += seg) {
                    var a = t, b = Math.min(s, t + seg * onf)
                    ctx.beginPath()
                    if (side === 0) { ctx.moveTo(x0 + a, x0); ctx.lineTo(x0 + b, x0) }
                    if (side === 1) { ctx.moveTo(x0 + s, x0 + a); ctx.lineTo(x0 + s, x0 + b) }
                    if (side === 2) { ctx.moveTo(x0 + s - a, x0 + s); ctx.lineTo(x0 + s - b, x0 + s) }
                    if (side === 3) { ctx.moveTo(x0, x0 + s - a); ctx.lineTo(x0, x0 + s - b) }
                    ctx.stroke()
                }
            return
        }
        if (style === "solid") {
            ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.stroke()
            return
        }
        // Dashes: arc segments. Dotted = short segments, dashed = long ones.
        var n = style === "dotted" ? 24 : 8
        var on = style === "dotted" ? 0.35 : 0.6          // share of each period drawn
        for (var i = 0; i < n; ++i) {
            var a0 = i * 2 * Math.PI / n, a1 = a0 + on * 2 * Math.PI / n
            ctx.beginPath(); ctx.arc(cx, cy, r, a0, a1); ctx.stroke()
        }
    }
}
