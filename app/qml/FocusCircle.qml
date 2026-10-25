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
    readonly property real lineW: Math.max(2, Math.round(2 * Theme.pixelRatio))

    // Camera2: 0 INACTIVE, 1 PASSIVE_SCAN, 2 PASSIVE_FOCUSED, 3 ACTIVE_SCAN,
    // 4 FOCUSED_LOCKED, 5 NOT_FOCUSED_LOCKED, 6 PASSIVE_UNFOCUSED.
    readonly property string style: (afState === 4 || afState === 2) ? "solid"
                                   : (afState === 5 || afState === 6) ? "dashed" : "dotted"

    width: Theme.itemSizeMedium; height: width
    onStyleChanged: requestPaint()
    onColourChanged: requestPaint()
    onSunChanged: requestPaint()
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
