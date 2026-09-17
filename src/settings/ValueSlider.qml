// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.6
import Sailfish.Silica 1.0

Item {
    id: root

    property QtObject settings
    property string settingProperty
    property var model: []
    property var valueLabel
    property string caption
    property color highlightColor: Theme.colorScheme == Theme.LightOnDark
                                   ? Theme.highlightColor
                                   : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)
    property bool _dragging
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
        return Math.floor(model.length / 2)
    }

    function xForIndex(index) {
        if (!model || model.length < 2) {
            return track.x
        }
        return track.x + index * track.width / (model.length - 1)
    }

    function setIndex(index) {
        if (!settings || settingProperty.length === 0 || !model || model.length === 0) {
            return
        }
        settings[settingProperty] = model[Math.max(0, Math.min(model.length - 1, index))]
    }

    Label {
        anchors {
            left: parent.left
            top: parent.top
        }
        color: root.highlightColor
        opacity: Theme.opacityHigh
        font {
            pixelSize: Theme.fontSizeTiny
            bold: true
        }
        text: root.caption
    }

    Label {
        anchors {
            right: parent.right
            top: parent.top
        }
        color: Theme.lightPrimaryColor
        font {
            pixelSize: Theme.fontSizeMedium
            bold: true
        }
        text: root.textForValue(root.currentValue)
    }

    Rectangle {
        id: track

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            margins: Theme.paddingMedium
        }
        height: Math.max(1, Math.round(Theme.pixelRatio * 2))
        color: Theme.rgba(Theme.lightPrimaryColor, Theme.opacityLow)
    }

    Repeater {
        model: root.model

        Rectangle {
            width: Math.max(1, Math.round(Theme.pixelRatio * 2))
            height: Theme.paddingMedium
            x: root.xForIndex(index) - width / 2
            y: track.y + track.height / 2 - height / 2
            color: index === root.indexForValue(root.currentValue)
                   ? root.highlightColor : Theme.rgba(Theme.lightPrimaryColor,
                                                       Theme.opacityLow)
        }
    }

    Rectangle {
        id: handle

        width: Theme.paddingLarge
        height: width
        radius: width / 2
        color: root.highlightColor
        x: root.xForIndex(root.indexForValue(root.currentValue)) - width / 2
        y: track.y + track.height / 2 - height / 2

        Behavior on x {
            enabled: !mouseArea.drag.active
            NumberAnimation { duration: 120; easing.type: Easing.OutQuad }
        }
    }

    MouseArea {
        id: mouseArea

        anchors.fill: parent

        function updateValue(mouseX) {
            var position = Math.max(track.x, Math.min(track.x + track.width, mouseX))
            var index = Math.round((position - track.x) / track.width
                                   * (root.model.length - 1))
            root.setIndex(index)
        }

        onPressed: updateValue(mouse.x)
        onPositionChanged: if (pressed) updateValue(mouse.x)
    }
}
