// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

SettingsMenu {
    id: menu

    property string property
    property QtObject settings
    property var valueLabel
    readonly property real compactItemHeight: Math.min(
                                                width,
                                                Math.max(Theme.itemSizeExtraSmall,
                                                         (Screen.width - Theme.itemSizeSmall
                                                          - Theme.paddingSmall * model.length)
                                                         / Math.max(1, model.length)))

    delegate: SettingsMenuItemBase {
        settings: menu.settings
        property: menu.property
        value: modelData
        itemHeight: menu.compactItemHeight
        hideWhenOffscreen: false

        Label {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.paddingSmall
            horizontalAlignment: Text.AlignHCenter
            truncationMode: TruncationMode.Fade
            color: Theme.lightPrimaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
            font.bold: true
            text: menu.valueLabel ? menu.valueLabel(modelData) : modelData
        }
    }
}
