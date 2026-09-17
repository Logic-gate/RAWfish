// SPDX-FileCopyrightText: 2016 - 2021 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

Column {
    id: menu

    property var title
    property alias model: repeater.model
    property alias delegate: repeater.delegate
    property Item currentItem
    property MouseArea pressedItem
    property string caption
    readonly property Item highlightItem: pressedItem && pressedItem.pressed
            ? pressedItem
            : currentItem
    property Item header

    property bool pressed: pressedItem && pressedItem.pressed
    property bool active: model.length > 0

    onPressedChanged: {
        if (pressed && header) {
            header.pressedMenu = menu
        }
    }

    width: Screen.width / 4
    height: implicitHeight
    visible: active

    Label {
        width: parent.width
        height: caption.length > 0 ? implicitHeight + Theme.paddingSmall : 0
        visible: caption.length > 0
        horizontalAlignment: Text.AlignHCenter
        truncationMode: TruncationMode.Fade
        color: Theme.lightPrimaryColor
        opacity: Theme.opacityHigh
        font.pixelSize: Theme.fontSizeTiny
        text: caption
    }

    Repeater {
        id: repeater
    }
}
