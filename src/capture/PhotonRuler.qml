// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style horizontal ruler: a row of small ticks with a taller yellow tick at the
// current value and the range end labels underneath ("-3" ... "0" ... "3"). Dragging
// anywhere on it moves the value, snapped to `step`.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: ruler

    property real from: -3
    property real to: 3
    property real step: 1.0 / 3.0
    property real value: 0
    property string unit

    signal valueEdited(real value)

    readonly property int _tickCount: Math.round((to - from) / step) + 1
    readonly property real _slot: width / Math.max(1, _tickCount - 1)

    implicitHeight: 40 * PhotonTheme.dp
    height: implicitHeight

    function _snap(v) {
        var snapped = from + Math.round((v - from) / step) * step
        return Math.max(from, Math.min(to, snapped))
    }

    Repeater {
        model: ruler._tickCount

        delegate: Rectangle {
            readonly property real tickValue: ruler.from + index * ruler.step
            readonly property bool major: Math.abs(tickValue - Math.round(tickValue)) < 0.001
            readonly property bool current: Math.abs(tickValue - ruler.value) < ruler.step / 2

            x: index * ruler._slot - width / 2
            y: current ? 0 : (major ? 4 * PhotonTheme.dp : 8 * PhotonTheme.dp)
            width: Math.max(1, (current ? 2 : 1) * PhotonTheme.dp)
            height: current ? 20 * PhotonTheme.dp : (major ? 12 * PhotonTheme.dp : 6 * PhotonTheme.dp)
            color: current ? PhotonTheme.hud : PhotonTheme.textTertiary
        }
    }

    Text {
        x: 0
        y: parent.height - height
        text: ruler.from + ruler.unit
        color: PhotonTheme.textTertiary
        font.pixelSize: PhotonTheme.fontSizeCaption
    }

    Text {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
        text: ruler.value + ruler.unit
        color: PhotonTheme.hud
        font.pixelSize: PhotonTheme.fontSizeCaption
        font.bold: true
    }

    Text {
        anchors { right: parent.right; bottom: parent.bottom }
        text: ruler.to + ruler.unit
        color: PhotonTheme.textTertiary
        font.pixelSize: PhotonTheme.fontSizeCaption
    }

    MouseArea {
        anchors.fill: parent
        preventStealing: true

        function update(mouseX) {
            var v = ruler._snap(ruler.from + (ruler.to - ruler.from)
                                * Math.max(0, Math.min(1, mouseX / Math.max(1, ruler.width))))
            if (Math.abs(v - ruler.value) > 1e-6)
                ruler.valueEdited(v)
        }

        onPressed: update(mouse.x)
        onPositionChanged: update(mouse.x)
    }
}
