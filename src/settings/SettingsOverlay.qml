// SPDX-FileCopyrightText: 2016 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.4
import QtMultimedia 5.6
import Sailfish.Silica 1.0
import com.vivid.camera 1.0
import Nemo.Configuration 1.0

PinchArea {
    id: overlay

    property bool isPortrait
    property real topButtonRowHeight
    property bool showCommonControls: true // any controls from here
    property bool deviceToggleEnabled
    property bool inButtonLayout

    property alias shutter: shutterContainer.children
    property alias anchorContainer: anchorContainer
    property alias container: container
    readonly property alias settingsOpacity: grid.opacity
    property bool orientationTransitionRunning

    property bool _pinchActive
    property bool topMenuOpen
    property bool _closing
    // top menu open or transitioning
    readonly property bool _exposed: topMenuOpen
                                     || _closing
                                     || verticalAnimation.running
                                     || dragArea.drag.active

    default property alias _data: container.data

    readonly property int _captureButtonLocation: overlay.isPortrait
                                                  ? Settings.global.portraitCaptureButtonLocation
                                                  : Settings.global.landscapeCaptureButtonLocation

    property real _progress: (panel.y + panel.height) / panel.height

    property real _menuItemHorizontalSpacing: Screen.sizeCategory >= Screen.Large
                                              ? Theme.paddingLarge * 2
                                              : Theme.paddingLarge
    property real _headerHeight: Screen.sizeCategory >= Screen.Large
                                 ? Theme.itemSizeMedium
                                 : Theme.itemSizeSmall + Theme.paddingMedium
    property real _safeTopMargin: Screen.hasCutouts && overlay.isPortrait
                                  ? Screen.topCutout.height + Theme.paddingMedium
                                  : 0
    property real _headerTopMargin: _safeTopMargin
                                    + (Screen.sizeCategory >= Screen.Large
                                       ? Theme.paddingLarge + Theme.paddingSmall
                                       : Theme.paddingSmall)
    readonly property real _menuWidth: Screen.sizeCategory >= Screen.Large
                                       ? Theme.iconSizeLarge + Theme.paddingMedium*2 // increase icon hitbox
                                       : Theme.iconSizeMedium + Theme.paddingMedium + Theme.paddingSmall

    property color _highlightColor: Theme.colorScheme == Theme.LightOnDark
                                    ? Theme.highlightColor
                                    : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)

    property real _commonControlOpacity: showCommonControls ? 1.0 : 0.0
    Behavior on _commonControlOpacity { FadeAnimation {} }

    onShowCommonControlsChanged: {
        if (!showCommonControls) {
            closeMenus()
        }
    }

    on_CaptureButtonLocationChanged: inButtonLayout = false

    onIsPortraitChanged: {
        upperHeader.pressedMenu = null
    }

    signal clicked(var mouse)

    function closeMenus() {
        _closing = true
        whiteBalanceMenu.open = false
        topMenuOpen = false
        inButtonLayout = false
        _closing = false
    }

    onPinchStarted: _pinchActive = true
    onPinchFinished: _pinchActive = false

    property list<Item> _buttonAnchors
    _buttonAnchors: [
        ButtonAnchor { id: buttonAnchorTL; index: 0; anchors { left: parent.left; top: parent.top } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorCL; index: 1; anchors { left: parent.left; verticalCenter: parent.verticalCenter } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorBL; index: 2; anchors { left: parent.left; bottom: parent.bottom } },
        ButtonAnchor { id: buttonAnchorBC; index: 3; anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom } visible: overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorBR; index: 4; anchors { right: parent.right; bottom: parent.bottom } },
        ButtonAnchor { id: buttonAnchorCR; index: 5; anchors { right: parent.right; verticalCenter: parent.verticalCenter } visible: !overlay.isPortrait },
        ButtonAnchor { id: buttonAnchorTR; index: 6; anchors { right: parent.right; top: parent.top } visible: !overlay.isPortrait }
    ]

    // Position of other elements given the capture button position
    property var _portraitPositions: [

        // Unused
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorTL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorCL

        // Used
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorBC, "cameraPosition": overlayAnchorBC, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBC
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBC, "cameraPosition": overlayAnchorBC, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignBottom }, // buttonAnchorBR

        // Unused
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignBottom }, // buttonAnchorCR
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorBR, "cameraPosition": overlayAnchorBR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignBottom }, // buttonAnchorTR
    ]
    property var _landscapePositions: [
        // Used
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorCL, "cameraPosition": overlayAnchorCL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorTL
        { "captureMode": overlayAnchorBL, "camera2CaptureFormat": overlayAnchorTL, "cameraPosition": overlayAnchorTL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorCL
        { "captureMode": overlayAnchorCL, "camera2CaptureFormat": overlayAnchorTL, "cameraPosition": overlayAnchorTL, "exposure": Qt.AlignRight, "backCameraToggle": Qt.AlignLeft   }, // buttonAnchorBL

        // Unused
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorBC

        // Used
        { "captureMode": overlayAnchorCR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorBR
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorTR, "cameraPosition": overlayAnchorTR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorCR
        { "captureMode": overlayAnchorBR, "camera2CaptureFormat": overlayAnchorCR, "cameraPosition": overlayAnchorCR, "exposure": Qt.AlignLeft,  "backCameraToggle": Qt.AlignRight  }, // buttonAnchorTR
    ]

    property var _overlayPosition: overlay.isPortrait ? _portraitPositions[overlay._captureButtonLocation]
                                                      : _landscapePositions[overlay._captureButtonLocation]

    Item {
        id: camera2BottomDeck

        readonly property bool active: Settings.global.captureMode === "image"
        readonly property real controlHeight: Math.round(parent.height * 0.40)
        readonly property real controlWidth: Math.round(parent.width * 0.40)
        readonly property real headerHeight: Theme.fontSizeTiny + Theme.paddingSmall

        anchors {
            right: parent.right
            bottom: parent.bottom
        }
        z: 2
        width: active ? (overlay.isPortrait ? parent.width : controlWidth) : 0
        height: active ? (overlay.isPortrait ? controlHeight : parent.height) : 0
        visible: active

        Rectangle {
            anchors.fill: parent
            color: "black"
            opacity: 1.0
        }

        Row {
            id: camera2HeaderRow

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: camera2ControlGrid.sideMargin
                rightMargin: camera2ControlGrid.sideMargin
                topMargin: Theme.paddingSmall
            }
            height: Theme.fontSizeTiny
            spacing: camera2ControlGrid.columnGap
            z: 3

            Label {
                width: camera2ControlGrid.leftWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "LENS"
            }

            Label {
                width: camera2ControlGrid.centerWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "EV"
            }

            Label {
                width: camera2ControlGrid.speedWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "SPEED"
            }

            Label {
                width: camera2ControlGrid.isoWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                color: _highlightColor
                opacity: Theme.opacityHigh
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                text: "ISO"
            }
        }

        Grid {
            id: camera2ControlGrid

            readonly property real sideMargin: Math.max(Theme.paddingMedium,
                                                        camera2BottomDeck.width * 0.015)
            readonly property real columnGap: Math.max(1, Math.round(Theme.pixelRatio))
            readonly property real usableWidth: camera2BottomDeck.width
                                                - 2 * sideMargin - 3 * columnGap
            readonly property real leftWidth: Math.round(usableWidth * 0.20)
            readonly property real centerWidth: Math.round(usableWidth * 0.41)
            readonly property real speedWidth: Math.round(usableWidth * 0.19)
            readonly property real isoWidth: usableWidth - leftWidth
                                             - centerWidth - speedWidth
            readonly property var camera2Viewfinder: captureView ? captureView.camera2Viewfinder : null

            function focalLengthText() {
                var focal = camera2Viewfinder && camera2Viewfinder.focalLength
                        ? camera2Viewfinder.focalLength : 0
                if (focal <= 0) {
                    return "--"
                }
                return focal < 10 ? focal.toFixed(1) : Math.round(focal)
            }

            function liveShutterValue() {
                return Settings.mode.rawCaptureShutterNs == "0" &&
                        camera2Viewfinder &&
                        camera2Viewfinder.liveExposureTime !== "0"
                        ? camera2Viewfinder.liveExposureTime
                        : Settings.mode.rawCaptureShutterNs
            }

            function liveIsoValue() {
                return Settings.mode.rawCaptureIso == 0 &&
                        camera2Viewfinder &&
                        camera2Viewfinder.liveSensorSensitivity > 0
                        ? camera2Viewfinder.liveSensorSensitivity
                        : Settings.mode.rawCaptureIso
            }

            anchors {
                fill: parent
                leftMargin: sideMargin
                rightMargin: sideMargin
                topMargin: camera2BottomDeck.headerHeight
            }
            columns: 4
            columnSpacing: columnGap

            Column {
                width: camera2ControlGrid.leftWidth
                height: camera2ControlGrid.height

                Item {
                    width: parent.width
                    height: parent.height / 3

                    Item {
                        anchors {
                            left: parent.left
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                        }
                        height: Theme.itemSizeMedium

                        Label {
                            anchors.centerIn: parent
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            color: Theme.lightPrimaryColor
                            font {
                                pixelSize: Theme.fontSizeLarge
                                bold: true
                            }
                            text: camera2ControlGrid.focalLengthText() + " MM"
                        }
                    }
                }

                CycleValueButton {
                    width: parent.width
                    height: parent.height / 3
                    caption: "MODE"
                    settings: Settings.mode
                    settingProperty: "camera2CaptureFormat"
                    currentValue: Settings.mode.camera2CaptureFormat
                    model: [ "jpeg", "raw" ]
                    valueLabel: function(value) { return value === "raw" ? "RAW" : "JPG" }
                }

                CycleValueButton {
                    width: parent.width
                    height: parent.height / 3
                    caption: "FOCUS"
                    settings: Settings.mode
                    settingProperty: "rawCaptureFocusMode"
                    currentValue: Settings.mode.rawCaptureFocusMode
                    model: [ "auto", "continuous", "manual", "infinity", "none" ]
                    valueLabel: function(value) {
                        if (value === "manual") {
                            return Settings.rawCaptureFocusDistanceLabel(
                                        Settings.mode.rawCaptureFocusDistance)
                        }
                        switch (value) {
                        case "continuous": return "CONT"
                        case "manual": return "MAN"
                        case "infinity": return "INF"
                        case "none": return "OFF"
                        default: return "AUTO"
                        }
                    }
                }
            }

            Column {
                width: camera2ControlGrid.centerWidth
                height: camera2ControlGrid.height

                Item {
                    width: parent.width
                    height: parent.height * 0.34

                    Canvas {
                        id: exposureHistogram

                        anchors.fill: parent
                        opacity: 0.85

                        onPaint: {
                            var context = getContext("2d")
                            context.clearRect(0, 0, width, height)

                            var values = camera2ControlGrid.camera2Viewfinder
                                    ? camera2ControlGrid.camera2Viewfinder.histogram : []
                            if (!values || values.length === 0) {
                                return
                            }

                            var peak = 1
                            for (var index = 0; index < values.length; ++index) {
                                peak = Math.max(peak, values[index])
                            }

                            var barWidth = width / values.length
                            for (index = 0; index < values.length; ++index) {
                                var barHeight = Math.max(1, values[index] / peak * height)
                                context.fillStyle = index === 0
                                        ? Qt.rgba(0.25, 0.55, 1.0, 0.55)
                                        : index === values.length - 1
                                          ? Qt.rgba(1.0, 0.35, 0.25, 0.55)
                                          : Qt.rgba(1, 1, 1, 0.30)
                                context.fillRect(index * barWidth,
                                                 height - barHeight,
                                                 Math.max(1, barWidth - 1),
                                                 barHeight)
                            }
                        }

                        Connections {
                            target: camera2ControlGrid.camera2Viewfinder
                            onHistogramChanged: exposureHistogram.requestPaint()
                        }
                    }

                    ValueCarousel {
                        anchors.fill: parent
                        orientation: ListView.Horizontal
                        caption: ""
                        settings: Settings.global
                        settingProperty: "exposureCompensation"
                        currentValue: Settings.global.exposureCompensation
                        model: [ -4, -3, -2, -1, 0, 1, 2, 3, 4 ]
                        valueLabel: function(value) {
                            var label = Settings.exposureText(value)
                            return label.length > 0 ? label : "0"
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: parent.height * 0.66

                    Item {
                        id: bottomShutterAnchor

                        width: Theme.itemSizeExtraLarge * 1.18
                        height: Theme.itemSizeExtraLarge * 1.18
                        anchors.centerIn: parent
                    }
                }
            }

            ValueCarousel {
                width: camera2ControlGrid.speedWidth
                height: camera2ControlGrid.height
                orientation: ListView.Vertical
                caption: ""
                settings: Settings.mode
                settingProperty: "rawCaptureShutterNs"
                currentValue: Settings.mode.rawCaptureShutterNs
                displayValue: camera2ControlGrid.liveShutterValue()
                model: Settings.camera2ShutterModel()
                valueLabel: Settings.rawCaptureShutterLabel
                selectedFontSize: Theme.fontSizeExtraLarge
                tapered: true
                wrap: true
            }

            ValueCarousel {
                width: camera2ControlGrid.isoWidth
                height: camera2ControlGrid.height
                orientation: ListView.Vertical
                caption: ""
                settings: Settings.mode
                settingProperty: "rawCaptureIso"
                currentValue: Settings.mode.rawCaptureIso
                displayValue: camera2ControlGrid.liveIsoValue()
                model: Settings.camera2IsoModel()
                valueLabel: function(value) { return value > 0 ? value : "Auto" }
                selectedFontSize: Theme.fontSizeExtraLarge
                tapered: true
                wrap: true
            }
        }

    }

    Item {
        id: shutterContainer

        parent: camera2BottomDeck.active ? bottomShutterAnchor
                                         : overlay._buttonAnchors[overlay._captureButtonLocation]
        anchors.fill: parent
    }

    CameraDeviceToggle {
        onSelected: {
            Settings.deviceId = deviceId
            captureView.resetZoom()
        }

        parent: {
            switch (_overlayPosition.backCameraToggle) {
            case Qt.AlignBottom:
                return overlayAnchorBC
            case Qt.AlignLeft:
                return overlayAnchorCL
            default:
            case Qt.AlignRight:
                return overlayAnchorCR
            }
        }

        opacity: _commonControlOpacity
        labels: Settings.global.backCameraLabels
        visible: opacity > 0.0
                 && !!model && model.length > 1
                 && labels.length > 0
                 && Settings.deviceId !== Settings.global.frontFacingDeviceId
                 && !inButtonLayout
        orientation: overlay.isPortrait ? Qt.Horizontal : Qt.Vertical
        enabled: camera.cameraStatus === Camera.ActiveStatus
                 || captureView._camera2ViewfinderActive
        model: camera.backFacingCameras
        currentDeviceId: camera.deviceId

        x: {
            if (_overlayPosition.backCameraToggle === Qt.AlignLeft) {
                return parent.width + Theme.paddingLarge
            } else if (_overlayPosition.backCameraToggle === Qt.AlignRight) {
                return -width - (isPortrait ? 1 : 2) * Theme.paddingLarge
            }

            return parent.width/2 - width/2
        }

        y: {
            if (_overlayPosition.backCameraToggle & Qt.AlignBottom) {
                return -height - (overlay.isPortrait ? 2 : 1) * Theme.paddingLarge
            }

            return parent.height/2 - height/2
        }
    }

    ToggleButton {
        parent: _overlayPosition.cameraPosition
        anchors.centerIn: parent
        icon: "image://theme/icon-camera-switch"
        opacity: _commonControlOpacity
        visible: opacity > 0.0 && camera.hasCameraOnBothSides
        enabled: overlay.deviceToggleEnabled

        onClicked: {
            if (Settings.global.position === Camera.BackFace) {
                Settings.deviceId = Settings.global.frontFacingDeviceId
            } else {
                Settings.deviceId = Settings.global.previousBackFacingDeviceId
            }

            captureView.resetZoom()
        }
    }

    CaptureModeMenu {
        id: captureModeMenu

        property real itemStep: Theme.itemSizeExtraSmall + spacing

        parent: _overlayPosition.captureMode
        anchors.verticalCenterOffset: height/2
        alignment: (parent.anchors.left === container.left ? Qt.AlignRight
                                                           : Qt.AlignLeft) | Qt.AlignBottom
        open: true
        opacity: _commonControlOpacity
        visible: opacity > 0.0 && !camera2BottomDeck.active

        Rectangle {
            id: captureModeHighlight

            z: -1
            width: Theme.itemSizeExtraSmall
            height: Theme.itemSizeExtraSmall
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Theme.rgba(_highlightColor, Theme.opacityLow)
            opacity: y < -captureModeMenu.itemStep ? 1.0 - (captureModeMenu.itemStep + y) / (-captureModeMenu.itemStep/2)
                                                   : (y > 0 ? 1.0 - y / (captureModeMenu.itemStep/2) : 1.0)
            y: captureModeMenu.currentIndex == 0 ? -captureModeMenu.itemStep : 0
            Behavior on y {
                id: captureModeBehavior

                YAnimator { duration: 400; easing.type: Easing.OutQuad }
            }
        }
    }

    Camera2CaptureFormatMenu {
        id: camera2CaptureFormatMenu

        property real itemStep: Theme.itemSizeExtraSmall + spacing

        parent: _overlayPosition.camera2CaptureFormat
        anchors.verticalCenterOffset: overlay.isPortrait
                                     ? height/2 - Theme.itemSizeMedium - Theme.paddingSmall
                                     : height/2
        anchors.horizontalCenterOffset: overlay.isPortrait
                                        ? 0
                                        : (parent.anchors.left === container.left
                                           ? Theme.itemSizeMedium + Theme.paddingSmall
                                           : -Theme.itemSizeMedium - Theme.paddingSmall)
        alignment: (parent.anchors.left === container.left ? Qt.AlignRight
                                                           : Qt.AlignLeft) | Qt.AlignBottom
        open: true
        opacity: _commonControlOpacity
        visible: opacity > 0.0
                 && !camera2BottomDeck.active
                 && camera.captureMode === Camera.CaptureStillImage
                 && captureView.camera2CaptureAvailable

        Rectangle {
            z: -1
            width: Theme.itemSizeExtraSmall
            height: Theme.itemSizeExtraSmall
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Theme.rgba(_highlightColor, Theme.opacityLow)
            opacity: y < -camera2CaptureFormatMenu.itemStep ? 1.0 - (camera2CaptureFormatMenu.itemStep + y) / (-camera2CaptureFormatMenu.itemStep/2)
                                                            : (y > 0 ? 1.0 - y / (camera2CaptureFormatMenu.itemStep/2) : 1.0)
            y: camera2CaptureFormatMenu.currentIndex == 0 ? -camera2CaptureFormatMenu.itemStep : 0
            Behavior on y {
                YAnimator { duration: 400; easing.type: Easing.OutQuad }
            }
        }
    }

    MouseArea {
        id: dragArea

        property real _lastPos
        property real _direction
        property int _extraDragMargin: 0

        anchors.fill: parent
        enabled: !overlay.inButtonLayout && showCommonControls
        drag {
            target: panel
            minimumY: -panel.height
            maximumY: _extraDragMargin
            axis: Drag.YAxis
            filterChildren: true
            onActiveChanged: {
                if (!drag.active) {
                    if (panel.y - _extraDragMargin < -(panel.height / 3) && _direction <= 0) {
                        overlay.topMenuOpen = false
                    } else if (panel.y > (-panel.height * 2 / 3) && _direction >= 0) {
                        overlay.topMenuOpen = true
                    }
                    expandBehavior.enabled = true
                    panel.updateY()
                    expandBehavior.enabled = false
                }
            }
        }

        onPressed: {
            _direction = 0
            _lastPos = panel.y
        }
        onPositionChanged: {
            var pos = panel.y
            _direction = (_direction + pos - _lastPos) / 2
            _lastPos = panel.y
        }

        MouseArea {
            id: container

            property real pressX
            property real pressY

            function outOfBounds(mouseX, mouseY) {
                if (captureView && captureView._camera2ViewfinderActive) {
                    return mouseY < captureView.camera2TopInset
                }
                return mouseX < Theme.paddingLarge || mouseX > width - Theme.paddingLarge
                        || mouseY < Theme.paddingLarge || mouseY > height - Theme.paddingLarge
            }

            anchors.fill: parent
            opacity: Math.min(1 - overlay._progress, 1 - anchorContainer.opacity)
            enabled: !overlay._pinchActive && showCommonControls

            onPressed: {
                pressX = mouseX
                pressY = mouseY
            }

            onClicked: {
                if (overlay.topMenuOpen) {
                    overlay.topMenuOpen = false
                }
                // don't react near display edges
                if (outOfBounds(mouseX, mouseY))
                    return
                if (whiteBalanceMenu.expanded) {
                    whiteBalanceMenu.open = false
                } else if (overlay.inButtonLayout) {
                    overlay.inButtonLayout = false
                } else {
                    overlay.clicked(mouse)
                }
            }

            onPressAndHold: {
                // don't react near display edges
                if (outOfBounds(mouseX, mouseY)) return
                if (!overlay.topMenuOpen) {
                    var dragDistance = Math.max(Math.abs(mouseX - pressX),
                                                Math.abs(mouseY - pressY))
                    if (dragDistance < Theme.startDragDistance) {
                        overlay.inButtonLayout = true
                    }
                }
            }

            MouseArea {
                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                }
                height: captureView && captureView._camera2ViewfinderActive
                        ? captureView.camera2TopInset
                        : Math.max(Theme.itemSizeLarge,
                                   topRow._topRowMargin + Theme.iconSizeMedium)
                enabled: !overlay._exposed && !overlay.inButtonLayout && showCommonControls

                onClicked: overlay.topMenuOpen = true

                onPressAndHold: container.pressAndHold(mouse)
            }

            OverlayAnchor { id: overlayAnchorBL; anchors { left: parent.left; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorBC; anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorBR; anchors { right: parent.right; bottom: parent.bottom } }
            OverlayAnchor { id: overlayAnchorCL; anchors { left: parent.left; verticalCenter: parent.verticalCenter } }
            OverlayAnchor { id: overlayAnchorCR; anchors { right: parent.right; verticalCenter: parent.verticalCenter } }
            OverlayAnchor { id: overlayAnchorTL; anchors { left: parent.left; top: parent.top } }
            OverlayAnchor { id: overlayAnchorTR; anchors { right: parent.right; top: parent.top } }
        }

        MouseArea {
            anchors.fill: parent
            enabled: overlay._exposed
            onClicked: overlay.topMenuOpen = false
        }

        Item {
            id: panel

            y: -panel.height

            function updateY() {
                if (!dragArea.drag.active) {
                    if (overlay.topMenuOpen) {
                        panel.y = dragArea._extraDragMargin
                    } else {
                        panel.y = -panel.height
                    }
                }
            }

            Connections {
                target: overlay
                onIsPortraitChanged: panel.updateY()
                onTopMenuOpenChanged: {
                    expandBehavior.enabled = true
                    panel.updateY()
                    expandBehavior.enabled = false
                }
            }

            Behavior on y {
                id: expandBehavior

                enabled: false
                NumberAnimation {
                    id: verticalAnimation
                    duration: 200; easing.type: Easing.InOutQuad
                }
            }

            width: overlay.width
            height: Screen.width / 2
        }

        Rectangle {
            anchors.fill: parent
            visible: overlay._exposed
            color: "black"
            opacity: Theme.opacityHigh * (1 - container.opacity)
        }

        Item {
            id: grid

            readonly property bool camera2Still: Settings.global.captureMode === "image"
            readonly property bool rawCapture: camera2Still
                                               && Settings.mode.camera2CaptureFormat === "raw"
            readonly property bool camera2FocusSupported: true
            readonly property bool rawAutoFocus: rawCapture
                                                 && camera2FocusSupported
                                                 && (Settings.mode.rawCaptureFocusMode === "auto"
                                                     || Settings.mode.rawCaptureFocusMode === "continuous")
            readonly property bool filterActive: Settings.global.colorFiltersAllowed
                                                 && colorFilter.supportedFilters.length > 1
            readonly property var itemKeys: currentItemKeys()
            readonly property int count: itemKeys.length
            readonly property real spacing: overlay._menuItemHorizontalSpacing
            readonly property real contentWidth: Math.round(width * (overlay.isPortrait ? 0.88 : 0.92))
            readonly property real menuWidth: overlay._menuWidth
            readonly property real settingsAreaWidth: camera2BottomDeck.active && !overlay.isPortrait
                                                      ? Math.max(menuWidth,
                                                                 Math.round((width - camera2BottomDeck.width) * 0.90))
                                                      : contentWidth
            readonly property real sceneWidth: Math.min(settingsAreaWidth, menuWidth * 5 + spacing * 4)
            readonly property var pages: pagedItemKeys()

            width: parent.width
            height: parent.height
            anchors.centerIn: parent

            opacity: 1 - container.opacity
            enabled: overlay._exposed
            visible: overlay._exposed

            function currentItemKeys() {
                if (camera2Still) {
                    if (rawCapture) {
                        return rawItemKeys()
                    }
                    return camera2JpegItemKeys()
                }
                return legacyItemKeys()
            }

            function legacyItemKeys() {
                var keys = [ "timer" ]
                if (CameraConfigs.supportedFlashModes.length > 0) {
                    keys.push("flash")
                }
                if (experimentalModes.value && CameraConfigs.supportedExposureModes.length > 0) {
                    keys.push("exposure")
                } else if (CameraConfigs.supportedIsoSensitivities.length === 0) {
                    keys.push("exposure")
                }
                if (CameraConfigs.supportedIsoSensitivities.length > 0) {
                    keys.push("iso")
                }
                keys.push("grid")
                if (filterActive) {
                    keys.push("filter")
                }
                return keys
            }

            function rawItemKeys() {
                var keys = [ "timer", "speed", "quality", "rotate", "timeout" ]
                if (camera2FocusSupported) {
                    keys.push("focus")
                }
                if (camera2FocusSupported && Settings.mode.rawCaptureFocusMode === "manual") {
                    keys.push("distance")
                }
                if (rawAutoFocus) {
                    keys.push("afWait")
                }
                keys.push("scene")
                keys.push("noise")
                keys.push("progressive")
                keys.push("rawExposure")
                keys.push("wb")
                keys.push("tint")
                keys.push("grid")
                return keys
            }

            function camera2JpegItemKeys() {
                var keys = [ "timer", "speed", "quality", "rotate" ]
                if (camera2FocusSupported) {
                    keys.push("focus")
                }
                if (camera2FocusSupported && Settings.mode.rawCaptureFocusMode === "manual") {
                    keys.push("distance")
                }
                keys.push("scene")
                keys.push("noise")
                keys.push("rawExposure")
                keys.push("grid")
                return keys
            }

            function componentForKey(key) {
                switch (key) {
                case "timer": return timerSettingComponent
                case "flash": return flashSettingComponent
                case "exposure": return exposureSettingComponent
                case "iso": return isoSettingComponent
                case "filter": return filterSettingComponent
                case "speed": return speedSettingComponent
                case "size": return sizeSettingComponent
                case "quality": return qualitySettingComponent
                case "rotate": return rotateSettingComponent
                case "timeout": return timeoutSettingComponent
                case "focus": return focusSettingComponent
                case "distance": return focusDistanceSettingComponent
                case "afWait": return focusTimeoutSettingComponent
                case "scene": return sceneSettingComponent
                case "noise": return noiseReductionSettingComponent
                case "progressive": return progressiveJpegSettingComponent
                case "rawExposure": return rawExposureSettingComponent
                case "wb": return whiteBalanceSettingComponent
                case "tint": return tintSettingComponent
                default: return gridSettingComponent
                }
            }

            function itemWidthForKey(key) {
                return key === "scene" ? sceneWidth : menuWidth
            }

            function pagedItemKeys() {
                var pages = []
                var page = []
                var pageWidth = 0
                for (var i = 0; i < itemKeys.length; ++i) {
                    var key = itemKeys[i]
                    var itemWidth = itemWidthForKey(key)
                    var nextWidth = pageWidth + (page.length > 0 ? spacing : 0) + itemWidth
                    if (page.length > 0 && nextWidth > settingsAreaWidth) {
                        pages.push(page)
                        page = []
                        pageWidth = 0
                        nextWidth = itemWidth
                    }
                    page.push(key)
                    pageWidth = nextWidth
                }
                if (page.length > 0) {
                    pages.push(page)
                }
                return pages
            }

            Flickable {
                id: settingsFlickable

                width: grid.contentWidth
                height: Math.round(parent.height * (overlay.isPortrait ? 0.58 : 0.72))
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: overlay._safeTopMargin
                contentWidth: settingsRow.width
                contentHeight: settingsRow.height
                flickableDirection: Flickable.HorizontalFlick
                interactive: contentWidth > width
                visible: overlay.isPortrait || !camera2BottomDeck.active

                Row {
                    id: settingsRow

                    x: Math.max(0, (settingsFlickable.width - width) / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    height: childrenRect.height
                    spacing: grid.spacing

                    Repeater {
                        model: grid.itemKeys

                        Loader {
                            width: grid.itemWidthForKey(modelData)
                            height: item ? item.implicitHeight : Theme.itemSizeMedium
                            sourceComponent: grid.componentForKey(modelData)

                            onLoaded: item.width = width
                        }
                    }
                }
            }

            ListView {
                id: settingsPager

                width: grid.settingsAreaWidth
                height: Math.round(parent.height * 0.66)
                x: Math.round((parent.width - camera2BottomDeck.width - width) / 2)
                y: Math.round((parent.height - height) / 2 + overlay._headerHeight / 2)
                orientation: ListView.Horizontal
                model: grid.pages
                interactive: visible && count > 1
                visible: camera2BottomDeck.active && !overlay.isPortrait
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                snapMode: ListView.SnapOneItem
                highlightMoveDuration: 160
                onCountChanged: currentIndex = Math.min(currentIndex, Math.max(0, count - 1))

                delegate: Item {
                    readonly property var pageKeys: modelData

                    width: settingsPager.width
                    height: settingsPager.height

                    Row {
                        id: settingsRow

                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                        }
                        height: childrenRect.height
                        spacing: grid.spacing

                        Repeater {
                            model: pageKeys

                            Loader {
                                width: grid.itemWidthForKey(modelData)
                                height: item ? item.implicitHeight : Theme.itemSizeMedium
                                sourceComponent: grid.componentForKey(modelData)

                                onLoaded: item.width = width
                            }
                        }
                    }
                }
            }

            Row {
                anchors {
                    horizontalCenter: settingsPager.horizontalCenter
                    top: settingsPager.bottom
                    topMargin: Theme.paddingSmall
                }
                spacing: Theme.paddingSmall
                visible: settingsPager.visible && settingsPager.count > 1

                Repeater {
                    model: settingsPager.count

                    Rectangle {
                        width: Theme.paddingSmall
                        height: width
                        radius: width / 2
                        color: index === settingsPager.currentIndex
                               ? overlay._highlightColor : Theme.lightPrimaryColor
                        opacity: index === settingsPager.currentIndex
                                 ? Theme.opacityHigh : Theme.opacityLow
                    }
                }
            }

            Component {
                id: timerSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.timerText
                    caption: "Timer"
                    header: upperHeader
                    model: [ 0, 3, 10, 15 ]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "timer"
                        value: modelData
                        icon: Settings.timerIcon(modelData)
                    }
                }
            }

            Component {
                id: flashSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.flashText
                    caption: "Flash"
                    header: upperHeader
                    model: CameraConfigs.supportedFlashModes.length > 0
                           ? CameraConfigs.supportedFlashModes : [Camera.FlashOff]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "flash"
                        value: modelData
                        icon: Settings.flashIcon(modelData)
                    }
                }
            }

            Component {
                id: exposureSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.exposureModeText
                    caption: "Exposure"
                    header: upperHeader
                    model: experimentalModes.value && CameraConfigs.supportedExposureModes.length > 0
                           ? CameraConfigs.supportedExposureModes : [Camera.ExposureManual]
                    delegate: SettingsMenuItem {
                        settings: Settings.mode
                        property: "exposureMode"
                        value: modelData
                        icon: Settings.exposureModeIcon(modelData)
                    }
                }
            }

            Component {
                id: isoSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.isoText
                    caption: "ISO"
                    header: upperHeader
                    model: CameraConfigs.supportedIsoSensitivities.length > 0
                           ? CameraConfigs.supportedIsoSensitivities : [0]
                    delegate: SettingsMenuItemBase {
                        settings: Settings.mode
                        property: "iso"
                        value: modelData

                        IsoItem {
                            anchors.centerIn: parent
                            value: modelData
                        }
                    }
                }
            }

            Component {
                id: gridSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.viewfinderGridText
                    caption: "Grid"
                    header: upperHeader
                    model: Settings.viewfinderGridValues
                    delegate: SettingsMenuItem {
                        settings: Settings.global
                        property: "viewfinderGrid"
                        value: modelData
                        icon: Settings.viewfinderGridIcon(modelData)
                    }
                }
            }

            Component {
                id: filterSettingComponent
                SettingsMenu {
                    width: grid.menuWidth
                    title: Settings.colorFiltersEnabledText
                    caption: "Filter"
                    header: upperHeader
                    model: [false, true]
                    delegate: SettingsMenuItem {
                        settings: Settings.global
                        property: "colorFiltersEnabled"
                        value: modelData
                        icon: Settings.colorFiltersIcon(modelData)
                    }
                }
            }

            Component {
                id: speedSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureSpeedText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureSpeedMode"
                    caption: "Speed"
                    valueLabel: Settings.rawCaptureSpeedLabel
                    model: Settings.camera2SpeedModel()
                }
            }

            Component {
                id: sizeSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureSizeText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureSize"
                    caption: "Size"
                    valueLabel: function(value) { return value.split("x")[0] }
                    model: Settings.camera2SizeModel(Settings.mode.camera2CaptureFormat)
                }
            }

            Component {
                id: qualitySettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureJpegQualityText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureJpegQuality"
                    caption: "JPEG"
                    valueLabel: function(value) { return value }
                    model: [ 75, 85, 92, 96, 100 ]
                }
            }

            Component {
                id: rotateSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureRotationText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureRotation"
                    caption: "Rotate"
                    valueLabel: function(value) { return value + " deg" }
                    model: [ 0, 90, 180, 270 ]
                }
            }

            Component {
                id: timeoutSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureTimeoutText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureTimeout"
                    caption: "Timeout"
                    valueLabel: function(value) { return value + " s" }
                    model: [ 10, 30, 60 ]
                }
            }

            Component {
                id: focusSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureFocusModeText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureFocusMode"
                    caption: "Focus"
                    valueLabel: function(value) {
                        switch (value) {
                        case "auto": return "Auto"
                        case "continuous": return "Cont."
                        case "manual": return "Manual"
                        case "infinity": return "Infinity"
                        default: return "None"
                        }
                    }
                    model: [ "auto", "continuous", "manual", "infinity", "none" ]
                }
            }

            Component {
                id: focusDistanceSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureFocusDistanceText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureFocusDistance"
                    caption: "Distance"
                    valueLabel: Settings.rawCaptureFocusDistanceLabel
                    model: Settings.camera2FocusDistanceModel()
                }
            }

            Component {
                id: focusTimeoutSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureFocusTimeoutText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureFocusTimeout"
                    caption: "AF wait"
                    valueLabel: function(value) { return value + " s" }
                    model: [ 1, 3, 5, 10 ]
                }
            }

            Component {
                id: sceneSettingComponent
                SceneGridSetting {
                    width: grid.sceneWidth
                    header: upperHeader
                }
            }

            Component {
                id: progressiveJpegSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureProgressiveJpegText
                    caption: "JPEG mode"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureProgressiveJpeg"
                    valueLabel: function(value) { return value ? "Progress." : "Standard" }
                    model: [ false, true ]
                }
            }

            Component {
                id: noiseReductionSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureNoiseReductionText
                    caption: "Noise"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureNoiseReduction"
                    valueLabel: Settings.rawCaptureNoiseReductionLabel
                    model: Settings.camera2NoiseReductionModel()
                }
            }

            Component {
                id: rawExposureSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureExposureText
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureExposure"
                    caption: "Exposure"
                    valueLabel: function(value) { return "x" + value }
                    model: [ "0.5", "1.0", "1.5", "2.0", "4.0" ]
                }
            }

            Component {
                id: whiteBalanceSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureColorTemperatureText
                    caption: "WB"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureColorTemperature"
                    valueLabel: function(value) { return value > 0 ? value + "K" : "Auto" }
                    model: [ 0, 2000, 3200, 4500, 5500, 6500, 7500, 9000 ]
                }
            }

            Component {
                id: tintSettingComponent
                TextSettingMenu {
                    width: grid.menuWidth
                    title: Settings.rawCaptureColorTintText
                    caption: "Tint"
                    header: upperHeader
                    settings: Settings.mode
                    property: "rawCaptureColorTint"
                    valueLabel: function(value) { return value > 0 ? "+" + value : value }
                    model: [ -200, -100, -50, 0, 50, 100, 200 ]
                }
            }

        }

        HeaderLabel {
            id: upperHeader

            anchors {
                left: parent.left
                bottom: settingsPager.visible ? settingsPager.top : settingsFlickable.top
                right: parent.right
            }
            height: overlay._headerHeight
            opacity: grid.opacity
        }
    }

    Row {
        id: topRow

        property real _topRowMargin: Math.max(overlay.topButtonRowHeight/2 - overlay._menuWidth/2,
                                              (Screen.hasCutouts && overlay.isPortrait)
                                              ? (Screen.topCutout.height + Theme.paddingSmall) : 0)

        anchors.horizontalCenter: parent.horizontalCenter
        spacing: grid.spacing
        opacity: overlay._exposed ? 0.0 : _commonControlOpacity
        visible: opacity > 0.0

        function dragY(yValue) {
            return yValue != undefined ? Math.max(topRow._topRowMargin, grid.y + yValue)
                                       : topRow._topRowMargin
        }

        Item {
            height: 1
            width: overlay._menuWidth
            visible: false
        }

        Item {
            width: overlay._menuWidth
            height: width
            visible: !grid.camera2Still && CameraConfigs.supportedFlashModes.length > 0
            y: topRow.dragY(0)

            Icon {
                anchors.centerIn: parent
                color: Theme.lightPrimaryColor
                source: Settings.flashIcon(Settings.mode.flash)
            }
        }

        Item {
            width: overlay._menuWidth
            height: width
            visible: !grid.camera2Still
                     && (experimentalModes.value ? CameraConfigs.supportedExposureModes.length > 1
                                                 : CameraConfigs.supportedIsoSensitivities.length == 0)
            y: topRow.dragY(0)

            Icon {
                anchors.centerIn: parent
                color: Theme.lightPrimaryColor
                source: Settings.exposureModeIcon(experimentalModes.value ? Settings.mode.exposureMode
                                                                          : Camera.ExposureManual)
            }
        }

        Item {
            width: overlay._menuWidth
            height: width
            y: topRow.dragY(0)
            visible: !grid.camera2Still && CameraConfigs.supportedIsoSensitivities.length > 1

            IsoItem {
                anchors.centerIn: parent
                value: Settings.mode.iso
            }
        }
    }

    Item {
        width: parent.width
        opacity: grid.opacity
        visible: overlay._exposed
        anchors.bottom: parent.bottom

        CameraButton {
            background.visible: false
            enabled: !Settings.defaultSettings && parent.opacity > 0.0
            opacity: !Settings.defaultSettings ? 1.0 : 0.0
            Behavior on opacity { FadeAnimator {}}

            width: Theme.itemSizeMedium
            height: Theme.itemSizeMedium
            anchors {
                right: parent.right
                bottom: parent.bottom
            }

            icon {
                opacity: pressed ? Theme.opacityLow : 1.0
                source: "image://theme/icon-camera-reset?" + (pressed ? _highlightColor : Theme.lightPrimaryColor)
            }

            onClicked: {
                upperHeader.pressedMenu = null
                Settings.reset()
            }
        }
    }

    Column {
        x: exposureSlider.alignment == Qt.AlignLeft ? (isPortrait ? 0 : Theme.paddingLarge)
                                                    : parent.width - width - (isPortrait ? 0 : Theme.paddingLarge)
        anchors {
            verticalCenter: parent.verticalCenter
            verticalCenterOffset: isPortrait ? (reallyWideScreen ? -Theme.itemSizeSmall : Theme.paddingMedium) : 0
        }
        spacing: Theme.paddingSmall
        opacity: _commonControlOpacity
        visible: opacity > 0.0 && !grid.camera2Still

        WhiteBalanceMenu {
            id: whiteBalanceMenu

            anchors {
                horizontalCenter: exposureSlider.horizontalCenter
                centerIn: null
            }
            enabled: !Settings.global.colorFiltersEnabled
                     || camera.imageProcessing.colorFilter === CameraImageProcessing.ColorFilterNone

            alignment: exposureSlider.alignment
            opacity: enabled ? 1.0 - settingsOpacity : 0.0
            spacing: Theme.paddingMedium
        }

        ExposureSlider {
            id: exposureSlider

            alignment: _overlayPosition.exposure
            enabled: !overlay.topMenuOpen && !overlay.inButtonLayout && !whiteBalanceMenu.open
            opacity: (1.0 - settingsOpacity) * (1.0 - whiteBalanceMenu.openProgress)
            height: Theme.itemSizeSmall * 5
        }
    }

    Item {
        property int paddingVector: overlay.isPortrait
                                    ? -2
                                    : _overlayPosition.exposure === Qt.AlignRight ? 2 : -2

        parent: _overlayPosition.exposure === Qt.AlignRight ? overlayAnchorBL : overlayAnchorBR
        anchors {
            centerIn: parent
            verticalCenterOffset: overlay.isPortrait ? overlayAnchorBL.width*paddingVector : 0
            horizontalCenterOffset: overlay.isPortrait ? 0 : overlayAnchorBL.height*paddingVector
        }

        opacity: qrFilter.result.length !== 0 ? 1.0 : 0.0
        visible: opacity != 0.0
        Behavior on opacity { FadeAnimation {} }

        CameraButton {
            icon.source: "image://theme/icon-camera-qr"
            background.visible: false
            anchors.centerIn: parent
            onClicked: {
                pageStack.push("QrPage.qml", { text: qrFilter.result })
            }
        }
    }

    ColorFilterView {
        id: colorFilter

        property bool ready: CameraConfigs.supportedColorFilters.length > 0
                             && Settings.global.colorFiltersEnabled && Settings.global.colorFiltersAllowed
        property var allowedFilters: [
            CameraImageProcessing.ColorFilterNone, CameraImageProcessing.ColorFilterGrayscale,
            CameraImageProcessing.ColorFilterSepia, CameraImageProcessing.ColorFilterPosterize,
            CameraImageProcessing.ColorFilterWhiteboard, CameraImageProcessing.ColorFilterBlackboard
        ]
        property var supportedFilters: {
            var filters = []
            var supportedFilters = CameraConfigs.supportedColorFilters
            for (var i = 0; i < supportedFilters.length; i++) {
                var filter = supportedFilters[i]
                if (allowedFilters.indexOf(filter) >= 0) {
                    filters.push(filter)
                }
            }
            return filters
        }

        model: ready ? supportedFilters : undefined
        anchors.bottom: parent.bottom
        orientationTransitionRunning: overlay.orientationTransitionRunning
        x: overlay.isPortrait ? 0 : exposureSlider.width + Theme.paddingMedium

        width: {
            if (overlay.isPortrait) {
                return Screen.width
            } else {
                var leftControlWidth = x
                var rightControlWidth = buttonAnchorCR.width + buttonAnchorCR.largeMargin
                var resolution = camera.viewfinder.resolution.width
                var viewfinderWidth = Screen.width * (resolution.width > 0 ? resolution.width/resolution.height : 1.2)
                return Math.min(Screen.height - rightControlWidth, viewfinderWidth) - leftControlWidth
            }
        }

        height: overlay.isPortrait ? Screen.height/11 : Theme.itemSizeMedium
        enabled: !overlay._exposed && Settings.global.colorFiltersEnabled

        onCurrentIndexChanged: update()
        onMovingChanged: update()

        function update() {
            if (!moving && !orientationTransitionRunning) {
                camera.imageProcessing.colorFilter = colorFilter.model[colorFilter.currentIndex]
            }
        }
    }

    OpacityRampEffect {
        offset: 1 - 1 / slope
        sourceItem: colorFilter
        slope: 1 + 20 * colorFilter.width / Screen.width
        visible: Settings.global.colorFiltersEnabled

        direction: OpacityRamp.BothSides
        opacity: (1.0 - settingsOpacity) * _commonControlOpacity
    }

    Item {
        id: anchorContainer

        anchors.fill: parent
        visible: overlay.inButtonLayout || layoutAnimation.running
        opacity: overlay.inButtonLayout ? 1.0 : 0.0
        Behavior on opacity {
            FadeAnimation {
                id: layoutAnimation
            }
        }

        Rectangle {
            anchors.fill: parent
            opacity: Theme.opacityOverlay
            color: "black"
        }

        Label {
            anchors {
                centerIn: parent
                verticalCenterOffset: -Theme.paddingLarge
            }
            width: overlay.isPortrait
                   ? Screen.width - (2 * Theme.itemSizeExtraLarge)
                   : Screen.width - Theme.itemSizeExtraLarge
            font.pixelSize: Theme.fontSizeExtraLarge
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            textFormat: Text.AutoText
            color: _highlightColor

            text: overlay.isPortrait
                  ? "Select location for the portrait capture key"
                  : "Select location for the landscape capture key"
        }
    }

    ConfigurationValue {
        id: experimentalModes

        key: "/apps/rawfish/enable_experimental_modes"
        defaultValue: false
    }
}
