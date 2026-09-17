// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import Sailfish.Silica 1.0

MouseArea {
    id: root

    property QtObject settings
    property string settingProperty
    property var model: []
    property var valueLabel
    property string caption
    property color highlightColor: Theme.colorScheme == Theme.LightOnDark
                                   ? Theme.highlightColor
                                   : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)
    property var currentValue: settings && settingProperty.length > 0
                               ? settings[settingProperty] : undefined

    height: Theme.itemSizeMedium

    function textForValue(value) {
        return valueLabel ? valueLabel(value) : value
    }

    function indexForValue(value) {
        for (var i = 0; i < model.length; ++i) {
            if (model[i] == value) {
                return i
            }
        }
        return 0
    }

    onClicked: {
        if (settings && settingProperty.length > 0 && model.length > 0) {
            settings[settingProperty] = model[(indexForValue(currentValue) + 1)
                                              % model.length]
        }
    }

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
            color: root.highlightColor
            opacity: Theme.opacityHigh
            font {
                pixelSize: Theme.fontSizeTiny
                bold: true
            }
            text: root.caption
        }

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            truncationMode: TruncationMode.Fade
            color: root.pressed ? root.highlightColor : Theme.lightPrimaryColor
            font {
                pixelSize: Theme.fontSizeLarge
                bold: true
            }
            text: root.textForValue(root.currentValue)
        }
    }
}
