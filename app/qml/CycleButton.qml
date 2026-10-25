/*
 * CycleButton.qml -- an invisible tappable button showing a tiny bold caption
 * above a large bold value, sized like RAWfish's CycleValueButton (height
 * itemSizeMedium, caption fontSizeTiny, value fontSizeLarge). Tapping emits
 * cycle(); the parent updates the value. Text follows the ambience.
 */
import QtQuick 2.0
import Sailfish.Silica 1.0

MouseArea {
    id: root
    property string caption
    property string subCaption          // optional small note under the value
    property string value
    property color textColor: "white"      // value colour (deck controls are white)
    property bool compact: false           // a smaller value (a compact deck, the XA2)
    signal cycle()

    height: Theme.itemSizeMedium
    onClicked: root.cycle()

    Column {
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
        }
        spacing: Theme.paddingSmall

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondaryHighlightColor
            opacity: Theme.opacityHigh
            font { pixelSize: Theme.fontSizeTiny; bold: true }
            text: root.caption
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            truncationMode: TruncationMode.Fade
            color: root.pressed ? Theme.highlightColor : root.textColor
            font { pixelSize: root.compact ? Theme.fontSizeMedium : Theme.fontSizeLarge; bold: true }
            text: root.value
        }
        Label {
            width: parent.width
            visible: root.subCaption.length > 0
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondaryHighlightColor
            font.pixelSize: Theme.fontSizeTiny
            text: root.subCaption
        }
    }
}
