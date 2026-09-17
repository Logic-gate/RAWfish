// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.6
import Sailfish.Silica 1.0
import com.vivid.camera 1.0

Column {
    id: root

    property Item header

    width: Screen.width
    height: implicitHeight

    Label {
        width: parent.width
        height: implicitHeight + Theme.paddingSmall
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        color: Theme.lightPrimaryColor
        opacity: Theme.opacityHigh
        font.pixelSize: Theme.fontSizeTiny
        text: "Scene"
    }

    Grid {
        id: sceneGrid

        readonly property int columnCount: 3
        readonly property real cellWidth: width / columnCount

        width: parent.width
        columns: sceneGrid.columnCount

        Repeater {
            model: Settings.camera2SceneModel()

            MouseArea {
                id: sceneItem

                readonly property bool selected: Settings.mode.rawCaptureScene === modelData

                width: sceneGrid.cellWidth
                height: Theme.itemSizeExtraSmall

                onPressed: {
                    if (root.header) {
                        root.header.pressedMenu = null
                    }
                }

                onClicked: Settings.mode.rawCaptureScene = modelData

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
                    text: Settings.rawCaptureSceneLabel(modelData)
                }
            }
        }
    }
}
