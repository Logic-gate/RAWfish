// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style zoom bar: a ratio/focal-length toggle on the left, the zoom stops in
// the middle (the current one in yellow) and a looks button on the right. Tapping a
// stop jumps to it; dragging along the bar zooms continuously, and while the zoom is
// between stops the nearest stop shows the live value, as in PhotonCamera. Presentation
// only; the owner applies zoomRequested().
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: bar

    property real zoom: 1.0
    property real maximumZoom: 1.0
    // 35 mm equivalent focal length at 1x; 0 when unknown (only ratios are shown then).
    property real equivalentFocalLength: 0
    property bool showFocalLength: true
    property bool looksActive: false

    readonly property bool focalAvailable: equivalentFocalLength > 0
    readonly property bool focal: showFocalLength && focalAvailable
    readonly property var stops: {
        var base = [1, 2, 3, 5]
        var result = []
        for (var i = 0; i < base.length; ++i) {
            if (base[i] <= maximumZoom + 0.001)
                result.push(base[i])
        }
        return result.length > 0 ? result : [1]
    }
    // Index of the stop nearest the live zoom, and whether the zoom sits on it.
    readonly property int nearest: {
        var best = 0
        for (var i = 1; i < stops.length; ++i) {
            if (Math.abs(stops[i] - zoom) < Math.abs(stops[best] - zoom))
                best = i
        }
        return best
    }
    readonly property bool onStop: Math.abs(stops[nearest] - zoom) <= 0.03

    signal zoomRequested(real value)
    signal displayModeToggled()
    signal looksClicked()

    implicitHeight: PhotonTheme.zoomBarHeight
    height: implicitHeight

    function label(ratio) {
        if (focal)
            return String(Math.round(equivalentFocalLength * ratio))
        var tenth = Math.round(ratio * 10) / 10
        return tenth < 10 ? tenth.toFixed(1) + "x" : Math.round(ratio) + "x"
    }

    Item {
        id: toggle

        visible: bar.focalAvailable
        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
        width: bar.height
        height: bar.height

        PhotonIcon {
            anchors.centerIn: parent
            width: 20 * PhotonTheme.dp
            height: width
            kind: "swap"
            color: PhotonTheme.textPrimary
        }

        MouseArea { anchors.fill: parent; onClicked: bar.displayModeToggled() }
    }

    Item {
        id: looksButton

        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: bar.height
        height: bar.height

        PhotonIcon {
            anchors.centerIn: parent
            width: 20 * PhotonTheme.dp
            height: width
            kind: "sparkle"
            color: bar.looksActive ? PhotonTheme.hud : PhotonTheme.textSecondary
        }

        MouseArea { anchors.fill: parent; onClicked: bar.looksClicked() }
    }

    Item {
        id: track

        anchors {
            left: toggle.right
            right: looksButton.left
            top: parent.top
            bottom: parent.bottom
        }

        Row {
            id: stopsRow

            anchors.centerIn: parent
            spacing: 0

            Repeater {
                model: bar.stops

                Item {
                    readonly property bool selected: index === bar.nearest
                    // Between stops the nearest one shows the live value.
                    readonly property real shown: selected && !bar.onStop ? bar.zoom : modelData

                    width: Math.min(track.width / Math.max(4, bar.stops.length), 56 * PhotonTheme.dp)
                    height: bar.height

                    Text {
                        anchors.centerIn: parent
                        text: bar.label(parent.shown)
                        color: parent.selected ? PhotonTheme.hud : PhotonTheme.textPrimary
                        font.pixelSize: parent.selected ? PhotonTheme.fontSizeBody
                                                         : PhotonTheme.fontSizeCaption
                        font.bold: parent.selected
                        style: Text.Outline
                        styleColor: "#80000000"
                    }
                }
            }
        }

        MouseArea {
            id: touch

            property real pressX: 0
            property real lastX: 0
            property bool dragging: false

            anchors.fill: parent
            preventStealing: dragging

            onPressed: {
                pressX = mouse.x
                lastX = mouse.x
                dragging = false
            }

            onPositionChanged: {
                if (!dragging && Math.abs(mouse.x - pressX) > 10 * PhotonTheme.dp)
                    dragging = true
                if (!dragging)
                    return
                // Exponential, so a drag feels the same at 1x and at 5x.
                var next = bar.zoom * Math.exp(-(mouse.x - lastX) * 0.004)
                lastX = mouse.x
                bar.zoomRequested(Math.max(1, Math.min(bar.maximumZoom, next)))
            }

            onReleased: {
                if (dragging) {
                    dragging = false
                    return
                }
                // Tap: the stop whose slot holds the touch. The nearest stop returns to
                // its own value when it was showing the live one.
                var local = stopsRow.mapFromItem(touch, mouse.x, mouse.y)
                var item = stopsRow.childAt(local.x, stopsRow.height / 2)
                if (!item)
                    return
                for (var i = 0; i < stopsRow.children.length; ++i) {
                    if (stopsRow.children[i] === item) {
                        bar.zoomRequested(bar.stops[i])
                        return
                    }
                }
            }
        }
    }
}
