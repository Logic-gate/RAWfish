// SPDX-FileCopyrightText: 2013 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2020 - 2021 Open Mobile Platform LLC.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.1
import QtMultimedia 5.0
import Amber.QrFilter 1.0
import Sailfish.Silica 1.0
import com.vivid.camera 1.0
import "pages"

ApplicationWindow {
    id: window

    property var captureModel: null
    property bool galleryActive
    property bool galleryVisible
    property int galleryIndex
    property bool camera2CaptureBusy
    readonly property bool _applicationActive: Qt.application.state === Qt.ApplicationActive
    readonly property real camera2TopStripHeight: Theme.itemSizeMedium

    on_ApplicationActiveChanged: {
        if (_applicationActive) {
            camera2Preview.restartPreview()
        }
    }

    allowedOrientations: defaultAllowedOrientations
    _defaultPageOrientations: Orientation.All
    _defaultLabelFormat: Text.PlainText

    cover: Qt.resolvedUrl("cover/CameraCover.qml")

    initialPage: Component {
        MainCameraPage {
            camera2Viewfinder: camera2Preview
            videoViewfinder: videoOutput
            camera2TopInset: camera2TopStripHeight
        }
    }

    // viewfinder background
    Rectangle {
        parent: window
        anchors.fill: parent
        z: -1
        color: "black"
        visible: (pageStack.depth < 2 && !pageStack.busy) || !galleryActive
    }

    VideoOutput {
        id: videoOutput

        z: -1
        width: window.width
        height: window.width < window.height
                ? window.height - Math.round(window.height * 0.34)
                : window.height
        visible: pageStack.depth < 2 && !galleryActive

        Behavior on y {
            enabled: !galleryVisible
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }

        filters: [ qrFilter ]
    }

    Camera2Preview {
        id: camera2Preview

        z: -1
        y: camera2TopStripHeight
        width: window.width < window.height
               ? window.width
               : window.width - Math.round(window.width * 0.40)
        height: Math.max(1, window.width < window.height
                         ? window.height - Math.round(window.height * 0.40) - camera2TopStripHeight
                         : window.height - camera2TopStripHeight)
        active: Settings.global.captureMode === "image"
                && !window.camera2CaptureBusy
                && window._applicationActive
                && pageStack.depth < 2
                && !galleryActive
        visible: active
        cameraId: Settings.global.position === Camera.FrontFace ? "1" : "0"
        previewSize: Qt.size(1280, 960)
        captureSize: Settings.mode.rawCaptureSize
        captureTimeout: Settings.mode.rawCaptureTimeout
        jpegCaptureEnabled: Settings.mode.camera2CaptureFormat === "jpeg"
                            && Settings.mode.rawCaptureSpeedMode !== "quality"
        rawCaptureEnabled: Settings.mode.camera2CaptureFormat === "raw"
                           && Settings.mode.rawCaptureSpeedMode !== "quality"
        jpegQuality: Settings.mode.rawCaptureJpegQuality
        jpegOrientation: Settings.mode.rawCaptureRotation
        orientation: Settings.global.position === Camera.FrontFace ? 270 : 90
        mirror: Settings.global.position === Camera.FrontFace
        fill: true

        Behavior on y {
            enabled: !galleryVisible
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }
    }

    QrFilter {
        id: qrFilter

        active: Settings.global.qrFilterEnabled
                && Settings.global.captureMode === "image"
                && Settings.global.position === Camera.BackFace

        onActiveChanged: qrFilter.clearResult()
    }
}
