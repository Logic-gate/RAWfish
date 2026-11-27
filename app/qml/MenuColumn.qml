/*
 * MenuColumn.qml -- one setting of the overlay menu, Jolla Camera / RAWfish
 * style: a tiny caption, then the possible values stacked as round items
 * (a theme icon when one exists, else a short text), the current value
 * marked by a faint highlight disc. Tapping an item picks it; while pressed,
 * the overlay's header shows the value's long name. When the values do not
 * fit in the height (maxRows, the band's: a landscape screen), they spread
 * over two or more side-by-side columns, top to bottom then left to right:
 * the last ones (JPG 100, Shade, ±3) used to be cut off there.
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
    // A drawn icon instead (the theme has none for it): a component whose
    // parent gives `value` (the item's) and `tint` (its colour), e.g.
    // LensFixIcon. null: the theme icon, else the text.
    property Component iconComponent: null
    property Item header                // MenuHeader, shows the pressed value
    property real colW: Theme.iconSizeMedium + 2 * Theme.paddingMedium   // one column's width
    property int maxRows: 99            // values stacked at most, then another column
    readonly property int cols: Math.max(1, Math.ceil(values.length / Math.max(1, maxRows)))
    width: colW * cols
    // Two-tap confirmation, for actions that overwrite or erase something (the
    // focus calibration): the first tap only arms the item -- it blinks in
    // translucent red and the header says to tap again -- and a second tap on
    // the SAME item within 4 s carries it out. Anything else disarms it.
    property bool confirm: false
    property var armed: undefined
    // Greyed out: shown, but a tap only says why (dimmedHint) and changes
    // nothing -- a choice that means nothing for the current image type.
    // The default value: its disc is drawn with a light green ring, so the
    // way back to the defaults stays visible. undefined: none marked.
    property var defaultValue: undefined
    property bool dimmed: false
    property string dimmedHint: ""
    signal picked(var value)
    Timer { id: disarm; interval: 4000; onTriggered: col.armed = undefined }
    function tap(v) {
        if (dimmed) { if (header && dimmedHint !== "") header.show(dimmedHint); return }
        if (!confirm) { picked(v); return }
        if (armed !== undefined && armed === v) { armed = undefined; disarm.stop(); picked(v); return }
        armed = v
        disarm.restart()
        if (header) header.show(qsTr("Tap again to confirm: %1").arg(labelFor(v)))
    }

    spacing: 0
    opacity: dimmed ? Theme.opacityLow : 1.0
    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        color: Theme.lightPrimaryColor; opacity: Theme.opacityHigh
        font.pixelSize: Theme.fontSizeTiny
        text: col.caption
        height: implicitHeight + Theme.paddingSmall
    }
    Grid {
        flow: Grid.TopToBottom
        rows: Math.max(1, Math.ceil(col.values.length / col.cols))   // balanced: 8 in 6 rows -> 4 + 4
        Repeater {
            model: col.values
            MouseArea {
                id: item
                width: col.colW
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
                Rectangle {                                   // the default: a light green ring
                    anchors.centerIn: parent
                    width: Theme.iconSizeMedium + Theme.paddingSmall; height: width; radius: width / 2
                    z: -1
                    color: "transparent"
                    border.color: "#8fd89a"; border.width: Math.max(1, Math.round(Theme.pixelRatio * 1.5))
                    opacity: 0.8
                    visible: col.defaultValue !== undefined && col.defaultValue === modelData
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
                Loader {                                      // a drawn icon (iconComponent)
                    id: drawn
                    anchors.centerIn: parent
                    active: col.iconComponent !== null
                    sourceComponent: col.iconComponent
                    property var value: modelData
                    property color tint: item.pressed || item.selected ? Theme.highlightColor : Theme.lightPrimaryColor
                }
                Label {                                       // text when there is no icon
                    anchors.centerIn: parent
                    visible: !icon.visible && !drawn.active
                    text: col.shortFor(modelData)
                    color: item.pressed || item.selected ? Theme.highlightColor : Theme.lightPrimaryColor
                    font { pixelSize: Theme.fontSizeTiny; bold: item.selected }
                }
            }
        }
    }
}
