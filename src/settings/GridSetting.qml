// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.6
import Sailfish.Silica 1.0

Column {
    id: root

    property Item header
    property string caption
    property var model: []
    property QtObject settings
    property string settingProperty
    property var valueLabel: function(value) { return String(value) }
    property real maximumHeight: Screen.height

    width: Screen.width
    height: implicitHeight

    Label {
        id: title
        width: parent.width
        height: implicitHeight + Theme.paddingSmall
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        color: Theme.lightPrimaryColor
        opacity: Theme.opacityHigh
        font.pixelSize: Theme.fontSizeTiny
        text: root.caption
    }

    Flickable {
        width: parent.width
        height: Math.min(sceneGrid.height, Math.max(0, root.maximumHeight - title.height))
        contentWidth: width
        contentHeight: sceneGrid.height
        clip: true
        interactive: contentHeight > height
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds

        Grid {
            id: sceneGrid

            readonly property int columnCount: 3
            readonly property real cellWidth: width / columnCount

            width: parent.width
            columns: sceneGrid.columnCount

            Repeater {
                model: root.model

                MouseArea {
                    id: sceneItem

                    readonly property bool selected: root.settings && root.settings[root.settingProperty] === modelData

                    width: sceneGrid.cellWidth
                    height: Theme.itemSizeExtraSmall

                    onPressed: {
                        if (root.header) {
                            root.header.pressedMenu = null
                        }
                    }

                    onClicked: root.settings[root.settingProperty] = modelData

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width - Theme.paddingSmall
                        height: Theme.itemSizeExtraSmall - Theme.paddingSmall
                        radius: Theme.paddingSmall / 2
                        color: Theme.highlightBackgroundColor
                        opacity: sceneItem.selected || sceneItem.pressed
                                 ? Theme.opacityFaint : 0.0
                        Behavior on opacity { FadeAnimation {} }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Theme.paddingSmall
                        horizontalAlignment: Text.AlignHCenter
                        truncationMode: TruncationMode.Fade
                        color: Theme.lightPrimaryColor
                        font {
                            pixelSize: Theme.fontSizeTiny
                            bold: sceneItem.selected
                        }
                        text: root.valueLabel(modelData)
                    }
                }
            }
        }
    }
}
