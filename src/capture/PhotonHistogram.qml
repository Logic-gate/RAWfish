// SPDX-License-Identifier: BSD-3-Clause
//
// Small translucent RGB histogram for the corner of the viewfinder, as in Photon.
// `values` is a list of { r, g, b } bins.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: histogram

    property var values: []

    implicitWidth: 96 * PhotonTheme.dp
    implicitHeight: 48 * PhotonTheme.dp
    width: implicitWidth
    height: implicitHeight

    onValuesChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: PhotonTheme.radiusSmall
        color: PhotonTheme.overlayPill
    }

    Canvas {
        id: canvas

        anchors { fill: parent; margins: 3 * PhotonTheme.dp }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var v = histogram.values
            if (!v || v.length === 0)
                return
            var peak = 1
            for (var i = 0; i < v.length; ++i)
                peak = Math.max(peak, v[i].r, v[i].g, v[i].b)

            function channel(key, color) {
                ctx.beginPath()
                ctx.moveTo(0, height)
                for (var n = 0; n < v.length; ++n)
                    ctx.lineTo(v.length === 1 ? 0 : n * width / (v.length - 1),
                               height - v[n][key] / peak * height)
                ctx.lineTo(width, height)
                ctx.closePath()
                ctx.fillStyle = color
                ctx.fill()
            }

            channel("r", Qt.rgba(1.0, 0.25, 0.2, 0.45))
            channel("g", Qt.rgba(0.3, 1.0, 0.4, 0.40))
            channel("b", Qt.rgba(0.3, 0.6, 1.0, 0.45))
        }
    }
}
