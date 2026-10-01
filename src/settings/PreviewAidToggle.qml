// SPDX-License-Identifier: BSD-3-Clause
import QtQuick 2.6
import Sailfish.Silica 1.0

MouseArea {
    property string text
    property bool checked: false
    height: Theme.itemSizeExtraSmall
    opacity: enabled ? 1 : Theme.opacityLow

    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.paddingSmall / 2
        radius: Theme.paddingSmall
        color: Theme.highlightBackgroundColor
        opacity: parent.checked || parent.pressed ? Theme.opacityFaint : 0
    }
    Label {
        anchors.centerIn: parent
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSizeTiny
        font.bold: true
        color: parent.checked ? Theme.highlightColor : Theme.lightPrimaryColor
        text: parent.text
    }
}
