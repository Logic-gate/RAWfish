// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style horizontal ruler over a list of discrete choices: one small tick per
// choice, a taller yellow tick at the current choice, the end labels underneath and
// the current choice's label in the middle. While dragging, the ruler follows the
// finger without changing anything; the choice is committed once on release.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: ruler

    property int count: 0
    property int index: 0
    property string startLabel
    property string endLabel
    property string currentLabel
    // Every n-th tick is drawn taller (e.g. whole EV steps); 0 draws them all alike.
    property int majorStep: 0

    // Emitted once, when the finger lifts on a different choice than `index`.
    signal indexCommitted(int index)

    readonly property bool dragging: touch.pressed
    readonly property int shownIndex: dragging ? touch.dragIndex : index
    readonly property real _slot: width / Math.max(1, count - 1)

    implicitHeight: 40 * PhotonTheme.dp
    height: implicitHeight

    function _indexAt(mouseX) {
        if (count < 2)
            return 0
        var i = Math.round(Math.max(0, Math.min(1, mouseX / Math.max(1, width))) * (count - 1))
        return Math.max(0, Math.min(count - 1, i))
    }

    Repeater {
        model: ruler.count

        delegate: Rectangle {
            readonly property bool current: index === ruler.shownIndex
            readonly property bool major: ruler.majorStep > 0 && index % ruler.majorStep === 0

            x: index * ruler._slot - width / 2
            y: current ? 0 : (major ? 4 * PhotonTheme.dp : 8 * PhotonTheme.dp)
            width: Math.max(1, (current ? 2 : 1) * PhotonTheme.dp)
            height: current ? 20 * PhotonTheme.dp
                            : (major ? 12 * PhotonTheme.dp : 6 * PhotonTheme.dp)
            color: current ? PhotonTheme.hud : PhotonTheme.textTertiary
        }
    }

    Text {
        x: 0
        y: parent.height - height
        text: ruler.startLabel
        color: PhotonTheme.textTertiary
        font.pixelSize: PhotonTheme.fontSizeCaption
    }

    Text {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom }
        text: ruler.currentLabel
        color: PhotonTheme.hud
        font.pixelSize: PhotonTheme.fontSizeCaption
        font.bold: true
    }

    Text {
        anchors { right: parent.right; bottom: parent.bottom }
        text: ruler.endLabel
        color: PhotonTheme.textTertiary
        font.pixelSize: PhotonTheme.fontSizeCaption
    }

    MouseArea {
        id: touch

        property int dragIndex: 0

        anchors.fill: parent
        preventStealing: true

        onPressed: dragIndex = ruler._indexAt(mouse.x)
        onPositionChanged: dragIndex = ruler._indexAt(mouse.x)
        onReleased: {
            if (dragIndex !== ruler.index)
                ruler.indexCommitted(dragIndex)
        }
    }
}
