/*
 * MenuColumn.qml -- one setting of the overlay menu, Jolla Camera / RAWfish
 * style: a tiny caption, then the possible values stacked as round items
 * (a theme icon when one exists, else a short text), the current value
 * marked by a faint highlight disc. Tapping an item picks it; while pressed,
 * the overlay's header shows the value's long name.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Column {
    id: col
    property string caption
    property var values: []            // the choices
    property var current               // the chosen one
    property var labelFor: function(v) { return String(v) }      // header text
    property var shortFor: function(v) { return String(v) }      // text in the disc
    property var iconFor: function(v) { return "" }              // theme icon or ""
    property Item header                // MenuHeader, shows the pressed value
    // Two-tap confirmation, for actions that overwrite or erase something (the
    // focus calibration): the first tap only arms the item -- it blinks in
    // translucent red and the header says to tap again -- and a second tap on
    // the SAME item within 4 s carries it out. Anything else disarms it.
    property bool confirm: false
    property var armed: undefined
    signal picked(var value)
    Timer { id: disarm; interval: 4000; onTriggered: col.armed = undefined }
    function tap(v) {
        if (!confirm) { picked(v); return }
        if (armed !== undefined && armed === v) { armed = undefined; disarm.stop(); picked(v); return }
        armed = v
        disarm.restart()
        if (header) header.show(qsTr("Tap again to confirm: %1").arg(labelFor(v)))
    }

    spacing: 0
    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        color: Theme.lightPrimaryColor; opacity: Theme.opacityHigh
        font.pixelSize: Theme.fontSizeTiny
        text: col.caption
        height: implicitHeight + Theme.paddingSmall
    }
    Repeater {
        model: col.values
        MouseArea {
            id: item
            width: col.width
            height: Theme.iconSizeMedium + Theme.paddingMedium     // tight: a disc plus a little air
            readonly property bool selected: modelData === col.current
            onPressed: if (col.header) col.header.show(col.labelFor(modelData))
            onReleased: if (col.header) col.header.hide()
            onCanceled: if (col.header) col.header.hide()
            onClicked: col.tap(modelData)
            Rectangle {                                   // armed: blinking red
                anchors.centerIn: parent
                width: Theme.iconSizeMedium + Theme.paddingSmall; height: width; radius: width / 2
                z: -1
                color: "#e03030"
                visible: col.armed !== undefined && col.armed === modelData
                SequentialAnimation on opacity {
                    running: parent.visible; loops: Animation.Infinite
                    NumberAnimation { from: 0.15; to: 0.6; duration: 350 }
                    NumberAnimation { from: 0.6; to: 0.15; duration: 350 }
                }
            }
            Rectangle {                                   // highlight disc
                anchors.centerIn: parent
                width: Theme.iconSizeMedium + Theme.paddingSmall; height: width; radius: width / 2
                z: -1
                color: Theme.highlightBackgroundColor
                opacity: item.selected || item.pressed ? Theme.opacityFaint : 0.0
                Behavior on opacity { FadeAnimation {} }
            }
            Image {
                id: icon
                anchors.centerIn: parent
                readonly property string name: col.iconFor(modelData)
                source: name.length > 0 ? "image://theme/" + name + "?" +
                        (item.pressed || item.selected ? Theme.highlightColor : Theme.lightPrimaryColor) : ""
                visible: status === Image.Ready
            }
            Label {                                       // text when there is no icon
                anchors.centerIn: parent
                visible: !icon.visible
                text: col.shortFor(modelData)
                color: item.pressed || item.selected ? Theme.highlightColor : Theme.lightPrimaryColor
                font { pixelSize: Theme.fontSizeTiny; bold: item.selected }
            }
        }
    }
}
