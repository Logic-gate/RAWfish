/*
 * BrightnessSlider.qml -- the simple mode's exposure control, in the place
 * of the wheels: a vertical slider of EV compensation (Jolla Camera has the
 * same idea). Written here rather than rotating a Silica Slider: a rotated
 * slider was hard to grab and sometimes did not move.
 *
 * Range: -3 .. +3 EV in the HAL's steps (caps.evStep), clamped to the HAL's
 * own range. The value is printed horizontally above the track.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Item {
    id: slider
    property var exposure                       // ExposureLogic
    property var caps                           // viewfinder.caps
    readonly property real evStep: caps && caps.evStep ? caps.evStep : 1 / 3
    readonly property int stepMin: Math.max(caps && caps.evMin !== undefined ? caps.evMin : -9, Math.round(-3 / evStep))
    readonly property int stepMax: Math.min(caps && caps.evMax !== undefined ? caps.evMax : 9, Math.round(3 / evStep))
    readonly property int steps: exposure ? exposure.evSteps : 0
    readonly property real ev: steps * evStep

    Label {                                     // the value, horizontal
        id: readout
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
        text: (ev > 0 ? "+" : "") + ev.toFixed(1)
        color: "white"; font { pixelSize: Theme.fontSizeLarge; bold: true }
    }
    Item {                                      // the track and its handle
        id: track
        anchors { top: readout.bottom; bottom: caption.top; horizontalCenter: parent.horizontalCenter
                  topMargin: Theme.paddingMedium; bottomMargin: Theme.paddingMedium }
        width: Theme.itemSizeSmall               // wide enough to grab easily
        readonly property real usable: height - handle.height
        function stepAt(y) {                     // y in track coords -> HAL steps (top = +)
            var f = 1 - Math.max(0, Math.min(1, (y - handle.height / 2) / usable))
            return Math.round(stepMin + f * (stepMax - stepMin))
        }
        Rectangle {                              // rail
            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; bottom: parent.bottom
                      topMargin: handle.height / 2; bottomMargin: handle.height / 2 }
            width: Math.max(2, Math.round(2 * Theme.pixelRatio))
            color: Theme.lightPrimaryColor; opacity: Theme.opacityLow
        }
        Rectangle {                              // zero mark
            anchors.horizontalCenter: parent.horizontalCenter
            y: handle.height / 2 + (1 - (0 - stepMin) / (stepMax - stepMin)) * track.usable - height / 2
            width: Theme.paddingLarge; height: Math.max(2, Math.round(2 * Theme.pixelRatio))
            color: Theme.lightPrimaryColor; opacity: Theme.opacityHigh
        }
        Rectangle {                              // handle
            id: handle
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.iconSizeSmall; height: width; radius: width / 2
            y: (1 - (steps - stepMin) / (stepMax - stepMin)) * track.usable
            color: area.pressed ? Theme.highlightColor : "white"
        }
        MouseArea {
            id: area
            anchors.fill: parent
            onPressed: exposure.setEv(track.stepAt(mouse.y))
            onPositionChanged: if (pressed) { var s = track.stepAt(mouse.y); if (s !== steps) exposure.setEv(s) }
        }
    }
    Label {
        id: caption
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
        text: qsTr("brightness"); color: Theme.secondaryHighlightColor
        font.pixelSize: Theme.fontSizeTiny
    }
}
