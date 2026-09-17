// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.6
import QtGraphicalEffects 1.0
import Sailfish.Silica 1.0

Item {
    id: root

    property QtObject settings
    property string settingProperty
    property var model: []
    property var valueLabel
    property string caption
    property int orientation: ListView.Horizontal
    property bool wrap: false
    property color highlightColor: Theme.colorScheme == Theme.LightOnDark
                                   ? Theme.highlightColor
                                   : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)
    property bool _syncing
    property bool _changingSetting
    property bool _userMoving
    property bool tapered: false
    property int selectedFontSize: Theme.fontSizeLarge
    readonly property int recenterMargin: model ? model.length * 2 : 0
    property var currentValue: settings && settingProperty.length > 0
                               ? settings[settingProperty] : undefined
    property var displayValue: currentValue
    readonly property int repeatCount: wrap && model && model.length > 1 ? 51 : 1
    readonly property int baseIndex: Math.floor(repeatCount / 2) * (model ? model.length : 0)
    readonly property var visualModel: wrap && model && model.length > 1
                                     ? repeatedModel()
                                     : model
    readonly property int selectedModelIndex: Math.max(0, indexForValue(currentValue))

    width: parent ? parent.width : Screen.width
    height: Theme.itemSizeLarge

    onCurrentValueChanged: {
        if (!_changingSetting) {
            syncTimer.restart()
        }
    }
    onModelChanged: syncTimer.restart()

    Component.onCompleted: syncTimer.restart()

    Timer {
        id: syncTimer

        interval: 0
        repeat: false
        onTriggered: syncFromValue()
    }

    function textForValue(value) {
        if (valueLabel) {
            return valueLabel(value)
        }
        return value
    }

    function valuesEqual(left, right) {
        return left == right
    }

    function indexForValue(value) {
        for (var i = 0; i < model.length; ++i) {
            if (valuesEqual(model[i], value)) {
                return i
            }
        }
        return -1
    }

    function syncFromValue() {
        if (!model || model.length === 0) {
            return
        }
        var modelIndex = indexForValue(currentValue)
        if (modelIndex < 0) {
            modelIndex = 0
            if (settings && settingProperty.length > 0) {
                _changingSetting = true
                settings[settingProperty] = model[0]
                _changingSetting = false
            }
        }
        _syncing = true
        list.currentIndex = visualIndexForModelIndex(modelIndex)
        if (list.count > 0) {
            list.positionViewAtIndex(list.currentIndex, ListView.Center)
        }
        _syncing = false
    }

    function modelIndexForVisualIndex(index) {
        if (!wrap || model.length <= 1) {
            return index
        }
        return ((index % model.length) + model.length) % model.length
    }

    function visualIndexForModelIndex(index) {
        return wrap && model.length > 1 ? baseIndex + index : index
    }

    function commitVisualIndex(index) {
        if (!settings || settingProperty.length === 0 || !model
                || model.length === 0 || index < 0) {
            return
        }

        _changingSetting = true
        settings[settingProperty] = model[modelIndexForVisualIndex(index)]
        _changingSetting = false
    }

    function repeatedModel() {
        var values = []
        for (var repeat = 0; repeat < repeatCount; ++repeat) {
            for (var index = 0; index < model.length; ++index) {
                values.push(model[index])
            }
        }
        return values
    }

    Label {
        id: captionLabel

        anchors {
            left: parent.left
            right: parent.right
        }
        y: list.vertical ? Math.max(0, list.sideMargin + Theme.paddingSmall) : 0
        z: 2
        height: Theme.fontSizeTiny + Theme.paddingSmall
        visible: root.caption.length > 0
        horizontalAlignment: Text.AlignHCenter
        color: root.highlightColor
        opacity: Theme.opacityHigh
        font {
            pixelSize: Theme.fontSizeTiny
            bold: true
        }
        text: root.caption
    }

    ListView {
        id: list

        readonly property bool vertical: root.orientation === ListView.Vertical
        readonly property real itemWidth: vertical ? width
                                                   : Math.max(Theme.itemSizeMedium,
                                                              Math.round(root.width / 3))
        readonly property real itemHeight: vertical ? Math.max(Theme.fontSizeLarge * 1.6,
                                                               Math.round(height / 7))
                                                    : height
        readonly property real sideMargin: vertical ? Math.max(0, height / 2 - itemHeight / 2)
                                                    : Math.max(0, width / 2 - itemWidth / 2)

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            bottom: parent.bottom
        }
        clip: true
        orientation: root.orientation
        model: root.visualModel
        currentIndex: root.visualIndexForModelIndex(root.selectedModelIndex)
        flickDeceleration: 2 * Theme.flickDeceleration
        maximumFlickVelocity: Theme.maximumFlickVelocity / 2
        highlightMoveDuration: 160
        highlightRangeMode: ListView.StrictlyEnforceRange
        preferredHighlightBegin: sideMargin
        preferredHighlightEnd: sideMargin
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem

        onCountChanged: {
            if (count > 0) {
                syncTimer.restart()
            }
        }

        onMovementStarted: root._userMoving = true

        onCurrentIndexChanged: {
            if (root._userMoving && !root._syncing) {
                root.commitVisualIndex(currentIndex)
            }
        }

        onMovementEnded: {
            if (root._userMoving && !root._syncing) {
                root.commitVisualIndex(currentIndex)
            }
            root._userMoving = false

            if (root.wrap && root.model && root.model.length > 1 &&
                    (currentIndex < root.recenterMargin ||
                     currentIndex > count - root.recenterMargin)) {
                _syncing = true
                currentIndex = root.visualIndexForModelIndex(
                            root.modelIndexForVisualIndex(currentIndex))
                positionViewAtIndex(currentIndex, ListView.Center)
                _syncing = false
            }
        }

        header: Item {
            width: list.vertical ? 1 : list.sideMargin
            height: list.vertical ? list.sideMargin : 1
        }

        footer: Item {
            width: list.vertical ? 1 : list.sideMargin
            height: list.vertical ? list.sideMargin : 1
        }

        delegate: MouseArea {
            id: item

            readonly property bool selected: root.modelIndexForVisualIndex(index)
                                             === root.selectedModelIndex
            readonly property real centerDistance: root.tapered
                                                   ? Math.min(3, Math.abs(index - list.currentIndex))
                                                   : (selected ? 0 : 1)
            readonly property real textScale: root.tapered
                                             ? Math.max(0.58, 1.0 - centerDistance * 0.14)
                                             : 1.0
            readonly property real textOpacity: root.tapered
                                               ? Math.max(0.16, 1.0 - centerDistance * 0.24)
                                               : (selected ? 1.0 : Theme.opacityLow)
            readonly property real blurRadius: root.tapered
                                               ? centerDistance * 0.65
                                               : 0

            width: list.itemWidth
            height: list.itemHeight
            onClicked: {
                list.currentIndex = index
                list.positionViewAtIndex(index, ListView.Center)
                root.commitVisualIndex(index)
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.paddingSmall
                horizontalAlignment: Text.AlignHCenter
                truncationMode: TruncationMode.Fade
                color: item.selected ? root.highlightColor : Theme.lightPrimaryColor
                opacity: item.textOpacity
                scale: item.textScale
                font {
                    pixelSize: item.selected ? root.selectedFontSize
                                             : Theme.fontSizeMedium
                    bold: item.selected
                }
                text: root.textForValue(item.selected ? root.displayValue : modelData)
                layer.enabled: root.tapered && item.blurRadius > 0
                layer.effect: FastBlur {
                    radius: item.blurRadius
                }
            }
        }
    }
}
