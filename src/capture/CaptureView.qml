// SPDX-FileCopyrightText: 2013 - 2022 Jolla Ltd.
// SPDX-FileCopyrightText: 2020 Open Mobile Platform LLC.
// SPDX-FileCopyrightText: 2024 - 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.4
import QtMultimedia 5.4
import Nemo.Policy 1.0
import Nemo.Ngf 1.0
import Nemo.Notifications 1.0
import org.nemomobile.systemsettings 1.0
import Sailfish.Silica 1.0
import Sailfish.Policy 1.0
import com.vivid.camera 1.0

import "../settings"

FocusScope {
    id: captureView

    property bool active
    property int orientation
    property int effectiveIso: Settings.mode.iso
    property bool inButtonLayout: captureOverlay == null || captureOverlay.inButtonLayout
    property QtObject captureModel

    readonly property int viewfinderOrientation: {
        var rotation = 0
        switch (captureView.orientation) {
        case Orientation.Landscape: rotation = 90; break;
        case Orientation.PortraitInverted: rotation = 180; break;
        case Orientation.LandscapeInverted: rotation = 270; break;
        }

        return (720 + camera.orientation + rotation) % 360
    }
    property int captureOrientation
    property int pageRotation
    property bool orientationTransitionRunning

    property alias camera: camera
    property QtObject viewfinder
    property QtObject camera2Viewfinder
    property real camera2TopInset: 0

    readonly property bool recording: active && camera.videoRecorder.recorderState == CameraRecorder.RecordingState

    property bool _unload

    property bool touchFocusSupported: ((_camera2ViewfinderActive
                                         && (Settings.mode.rawCaptureFocusMode === "auto"
                                             || Settings.mode.rawCaptureFocusMode === "continuous"))
                                        || camera.focus.focusMode == Camera.FocusAuto
                                        || camera.focus.focusMode == Camera.FocusContinuous)
                                       && camera.captureMode != Camera.CaptureVideo

    // not bound to focusTimer.running, restarting timer shouldn't exit tap focus mode temporarily and lose focus state
    property bool tapFocusActive
    property var _camera2FocusPoint: Qt.point(0.5, 0.5)
    property bool _captureOnFocus
    property real _captureCountdown

    property bool reallyWideScreen: (Screen.height / Screen.width) >= 2.0
    // wide screen can move 4:3 viewfinder a little lower and avoid overlap with top&bottom buttons
    readonly property real viewfinderOffset: Math.min(0,
                                                      isPortrait ? (focusArea.width - height) / 2
                                                                 : (focusArea.width - width) / 2)
                                             + ((reallyWideScreen && (focusArea.width / focusArea.height <= 1.4))
                                                ? Theme.itemSizeLarge + Screen.topCutout.height
                                                : 0)

    readonly property bool isPortrait: orientation == Orientation.Portrait
                                       || orientation == Orientation.PortraitInverted
    readonly property bool effectiveActive: (active || recording) && _applicationActive && pageStack.depth < 2

    readonly property bool _canCapture: {
        switch (camera.captureMode) {
            case Camera.CaptureStillImage: 
                return captureView._camera2ViewfinderActive
                       ? extensions.rawImageCaptureAvailable
                         && !captureView._camera2CapturePending
                         && !captureView._camera2CaptureRunning
                       : camera.imageCapture.ready
            case Camera.CaptureVideo:
                return camera.videoRecorder.recorderStatus >= CameraRecorder.LoadedStatus 
                    && captureOverlay != null && captureOverlay._recSecsRemaining > 0
            default: 
                return false
        }
    }

    property bool _captureQueued
    property bool captureBusy
    property real camera2Zoom: 1.0
    property bool _camera2CapturePending
    property bool _camera2CaptureRunning
    property bool _camera2LiveJpegCaptureRunning
    property bool _qtFallbackCapturePending
    property string _camera2CaptureTargetPath
    property string _camera2CaptureCameraId
    property double _camera2CaptureStartedMs: 0
    onCaptureBusyChanged: {
        if (!captureBusy && _captureQueued) {
            _captureQueued = false
            camera.captureImage()
        }
    }

    property bool handleVolumeKeys: (captureView._camera2ViewfinderActive
                                     ? captureView._canCapture
                                     : camera.imageCapture.ready)
                                    && keysResource.acquired
                                    && camera.captureMode == Camera.CaptureStillImage
                                    && !captureView._captureOnFocus
                                    && !captureView.captureUiBlocked
    property bool captureOnVolumeRelease

    onHandleVolumeKeysChanged: {
        if (!handleVolumeKeys)
            captureOnVolumeRelease = false
    }

    readonly property bool _mirrorViewfinder: camera.position === Camera.FrontFace
    readonly property bool _horizontalMirror: _mirrorViewfinder && camera.orientation % 180 == 0
    readonly property bool _verticalMirror: _mirrorViewfinder && camera.orientation % 180 != 0
    readonly property real _viewfinderAspectRatio: camera.viewfinder.resolution.height > 0
                                                 ? camera.viewfinder.resolution.width
                                                   / camera.viewfinder.resolution.height
                                                 : 4 / 3
    readonly property real _camera2PreviewWidth: Math.max(1, isPortrait
                                                          ? width
                                                          : width - Math.round(width * 0.40))
    readonly property real _camera2PreviewHeight: Math.max(1, isPortrait
                                                           ? height - Math.round(height * 0.40)
                                                             - camera2TopInset
                                                           : height - camera2TopInset)

    readonly property bool _applicationActive: Qt.application.state == Qt.ApplicationActive
    readonly property bool _camera2ViewfinderActive: Settings.global.captureMode === "image"
                                                     && effectiveActive
    readonly property bool camera2CaptureAvailable: extensions.rawImageCaptureAvailable
    readonly property bool _liveJpegCapture: _camera2ViewfinderActive
                                            && Settings.mode.camera2CaptureFormat === "jpeg"
                                            && Settings.mode.rawCaptureSpeedMode !== "quality"
    readonly property bool _warmJpegReady: _liveJpegCapture
                                           && camera2Viewfinder
                                           && camera2Viewfinder.jpegCaptureReady === true
    readonly property bool _liveRawCapture: _camera2ViewfinderActive
                                           && Settings.mode.camera2CaptureFormat === "raw"
                                           && Settings.mode.rawCaptureSpeedMode !== "quality"
    readonly property bool _warmRawReady: _liveRawCapture
                                          && camera2Viewfinder
                                          && camera2Viewfinder.rawCaptureReady === true
    readonly property bool _manualCamera2Focus: _camera2ViewfinderActive
                                                && Settings.mode.rawCaptureFocusMode === "manual"
    readonly property bool captureUiBlocked: captureBusy
                                             && !_camera2LiveJpegCaptureRunning

    readonly property string deviceId: Settings.deviceId

    property var captureOverlay: null

    signal recordingStopped(url url, string mimeType)
    signal loaded
    signal captured

    Item {
        id: captureSnapshot

        property alias sourceItem: captureSnapshotEffect.sourceItem

        visible: false
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width*captureSnapshotEffect.scale
        height: parent.height*captureSnapshotEffect.scale

        ShaderEffectSource {
            id: captureSnapshotEffect

            hideSource: false
            live: false
            scale: 0.4
            anchors.centerIn: parent
            width: isPortrait ? captureView.width : captureView.height
            height: isPortrait ? captureView.height : captureView.width
            rotation: -captureView.pageRotation
        }
    }

    function setFocusPoint(point) {
        focusTimer.restart()
        tapFocusActive = true
        if (_camera2ViewfinderActive && camera2Viewfinder
                && typeof camera2Viewfinder.setFocusPoint === "function") {
            _camera2FocusPoint = point
            camera2Viewfinder.setFocusPoint(point.x, point.y)
            return
        }
        camera.unlock()
        camera.focus.customFocusPoint = point
        camera.searchAndLock()
    }

    function _resetFocus() {
        focusTimer.running = false
        tapFocusActive = false
        _camera2FocusPoint = Qt.point(0.5, 0.5)
        if (_camera2ViewfinderActive && camera2Viewfinder
                && typeof camera2Viewfinder.clearFocusPoint === "function") {
            camera2Viewfinder.clearFocusPoint()
        }
        camera.unlock()
    }

    function resetZoom() {
        camera2Zoom = 1.0
        camera.digitalZoom = 1.0
    }

    function _changeManualFocusDistance(step) {
        var model = Settings.camera2FocusDistanceModel()
        var index = model.indexOf(Settings.mode.rawCaptureFocusDistance)
        if (index < 0) {
            index = 0
        }
        Settings.mode.rawCaptureFocusDistance = model[(index + step + model.length)
                                                       % model.length]
    }

    function _captureSidecarPath(suffix) {
        var path = _camera2CaptureTargetPath
        var dot = path.lastIndexOf(".")
        return (dot >= 0 ? path.substring(0, dot) : path) + suffix
    }

    function _warmJpegState() {
        return "live=" + _liveJpegCapture
                + " ready=" + _warmJpegReady
                + " running=" + (camera2Viewfinder ? camera2Viewfinder.running : "no-camera2")
                + " enabled=" + (camera2Viewfinder ? camera2Viewfinder.jpegCaptureEnabled : "no-camera2")
                + " jpegReady=" + (camera2Viewfinder ? camera2Viewfinder.jpegCaptureReady : "no-camera2")
                + " rawReady=" + (camera2Viewfinder ? camera2Viewfinder.rawCaptureReady : "no-camera2")
                + " error=" + (camera2Viewfinder ? camera2Viewfinder.errorString : "no-camera2")
    }

    function _captureState() {
        return "canCapture=" + _canCapture
                + " busy=" + captureBusy
                + " blocked=" + captureUiBlocked
                + " pending=" + _camera2CapturePending
                + " running=" + _camera2CaptureRunning
                + " rawAvailable=" + extensions.rawImageCaptureAvailable
                + " cameraStatus=" + camera.cameraStatus
                + " " + _warmJpegState()
    }

    function _recoverStaleCamera2CaptureState() {
        if (!_camera2CaptureRunning || _camera2LiveJpegCaptureRunning) {
            return false
        }

        camera2CaptureWatchdog.stop()
        _camera2CapturePending = false
        _camera2CaptureRunning = false
        _camera2LiveJpegCaptureRunning = false
        _captureQueued = false
        _unload = false
        captureBusy = false
        if (_camera2ViewfinderActive) {
            window.camera2CaptureBusy = false
        }
        return true
    }

    function _triggerCapture() {
        // avoid duplicate capture if volume key and some other key trigger (e.g. shutter)
        captureOnVolumeRelease = false
        _recoverStaleCamera2CaptureState()

        if (captureTimer.running) {
            captureTimer.reset()
        } else if (startRecordTimer.running) {
            startRecordTimer.running = false
        } else if (camera.videoRecorder.recorderState == CameraRecorder.RecordingState) {
            camera.videoRecorder.stop()
        } else if (_canCapture) {
            if (Settings.mode.timer != 0) {
                microphoneWarningNotification.publishIfNeeded()
                captureTimer.restart()
            } else if (camera.captureMode == Camera.CaptureStillImage) {
                camera.captureImage()
            } else {
                microphoneWarningNotification.publishIfNeeded()
                camera.record()
            }
        } else {
            console.warn("Capture not ready:", _captureState())
        }
    }


    function _pickViewfinderResolution(resolutions, aspectRatio) {
        var ratio
        if (aspectRatio === CameraConfigs.AspectRatio_16_9) {
            ratio = 16.0 / 9.0
        } else { // CameraConfigs.AspectRatio_4_3
            ratio = 4.0 / 3.0
        }

        if (resolutions && resolutions.length > 0) {
            var selectedPixels = 0
            var selectedIndex = 0
            var targetWidth = Math.round(Screen.width * ratio)
            for (var i = 0; i < resolutions.length; i++) {
                var resolution = resolutions[i]
                if (resolution.height === Screen.width && resolution.width === targetWidth) {
                    return resolution
                }
            }
            return _pickResolution(resolutions, aspectRatio)
        }
        return "-1x-1"
    }

    function aspectRatioToFraction(aspectRatio) {
        var ratio = 4.0 / 3.0
        if (aspectRatio === CameraConfigs.AspectRatio_16_9) {
            ratio = 16.0 / 9.0
        } else if (aspectRatio !== CameraConfigs.AspectRatio_4_3) {
            console.warn("Unknown aspect ratio", aspectRatio)
        }
        return ratio
    }

    function _pickResolution(resolutions, aspectRatio) {
        var ratio = aspectRatioToFraction(aspectRatio)

        if (resolutions && resolutions.length > 0) {
            var selectedPixels = 0
            var selectedIndex = -1
            for (var i = 0; i < resolutions.length; i++) {
                var resolution = resolutions[i]
                var pixels = resolution.width * resolution.height

                if (Math.abs(ratio - resolution.width / resolution.height) < 0.05 && pixels > selectedPixels) {
                    selectedPixels = pixels
                    selectedIndex = i
                }
            }

            if (selectedIndex >= 0) {
                return resolutions[selectedIndex]
            }
        }
        return "-1x-1"
    }

    Notification {
        id: microphoneWarningNotification

        function publishIfNeeded() {
            if (camera.captureMode == Camera.CaptureVideo && !AccessPolicy.microphoneEnabled) {
                microphoneWarningNotification.publish()
            }
        }

        urgency: Notification.Critical
        body: "Camera audio won't be recorded, microphone disabled by "
              + aboutSettings.baseOperatingSystemName + " Device Manager"
    }

    Notification {
        id: camera2CaptureErrorNotification

        function publishMessage(message) {
            camera2CaptureErrorNotification.previewBody = message
            camera2CaptureErrorNotification.publish()
        }

        isTransient: true
        urgency: Notification.Critical
    }

    onEffectiveIsoChanged: {
        if (effectiveIso == 0) {
            camera.exposure.setAutoIsoSensitivity()
        } else {
            camera.exposure.manualIso = Settings.mode.iso
        }
    }

    on_CanCaptureChanged: {
        if (!_canCapture) {
            startRecordTimer.running = false
        }
    }

    Component.onCompleted: {
        loadOverlay()
    }

    onDeviceIdChanged: {
        _resetFocus()
        captureTimer.reset()
        Settings.global.deviceId = Settings.deviceId
        camera.deviceId = Settings.deviceId
        Settings.global.position = camera.position
        if (camera.position === Camera.BackFace) {
            Settings.global.previousBackFacingDeviceId = camera.deviceId
        }
    }

    onEffectiveActiveChanged: {
        qrFilter.clearResult()

        if (!effectiveActive) {
            _resetFocus()
            captureTimer.reset()
        }
    }

    Timer {
        // prevent video recording continuing forever in the background
        running: recording && !effectiveActive
        interval: 60*1000
        onTriggered: camera.videoRecorder.stop()
    }

    Timer {
        interval: 1000
        running: captureView._unload
                 && !captureView._camera2CapturePending
                 && !captureView._camera2CaptureRunning
                 && (camera.cameraStatus === Camera.UnloadedStatus
                     || camera.cameraStatus === Camera.CameraError)
        onTriggered: {
            captureView._unload = false
        }
    }

    Timer {
        id: reactivateTimer

        property int retryCounter
        readonly property bool abort: retryCounter >= 5

        interval: 1000
        running: camera.cameraStatus == Camera.LoadingStatus && !abort
        onTriggered: {
            // Try re-activate when stuck in loading status for 1sec.
            active = false
            active = true
            ++retryCounter
        }
    }

    NonGraphicalFeedback {
        id: shutterEvent
        event: "camera_shutter"
    }

    NonGraphicalFeedback {
        id: recordStartEvent
        event: "video_record_start"
    }

    Timer {
        id: startRecordTimer

        interval: 200
        onTriggered: {
            captureOverlay.writeMetaData()
            camera.videoRecorder.record()
            if (camera.videoRecorder.recorderState == CameraRecorder.RecordingState) {
                camera.videoRecorder.recorderStateChanged.connect(camera._finishRecording)
                extensions.disableNotifications(captureView, true)
            }
        }
    }

    SequentialAnimation {
        id: captureTimer

        property bool resetCameraOnStop

        function reset() {
            if (resetCameraOnStop) {
                _resetFocus()
                resetCameraOnStop = false
            }
            stop()
        }

        NumberAnimation {
            duration: Settings.mode.timer * 1000
            from: Settings.mode.timer
            to: 0
            easing.type: Easing.Linear
            target: captureView
            property: "_captureCountdown"
        }
        ScriptAction {
            script: {
                if (camera.captureMode == Camera.CaptureStillImage) {
                    if (camera.focusPointMode == Camera.FocusPointAuto) {
                        camera.searchAndLock()
                    }
                    camera.captureImage()
                } else {
                    camera.record()
                }

                if (captureTimer.resetCameraOnStop) {
                    _resetFocus()
                    captureTimer.resetCameraOnStop = false
                }
            }
        }
    }

    NonGraphicalFeedback {
        id: recordStopEvent
        event: "video_record_stop"
    }

    onRecordingStopped: {
        if (captureModel) {
            captureModel.appendCapture(url, mimeType)
        }
    }

    Connections {
        target: CameraConfigs
        onReadyChanged: {
            // Reset flash torch mode if it's not supported
            if (camera.captureMode === Camera.CaptureVideo
                    && CameraConfigs.supportedFlashModes.indexOf(Settings.mode.flash) === -1) {
                Settings.mode.flash = Camera.FlashOff
            }
        }
    }

    Connections {
        target: Settings.mode
        onRawCaptureFocusModeChanged: captureView._resetFocus()
    }

    Camera {
        id: camera

        function lockAutoFocus() {
            captureOverlay.closeMenus()
            // timed capture locks when timer triggers
            if (camera.captureMode == Camera.CaptureStillImage
                    && focus.focusMode != Camera.FocusInfinity
                    && focus.focusMode != Camera.FocusHyperfocal
                    && camera.lockStatus == Camera.Unlocked
                    && focus.focusPointMode == Camera.FocusPointAuto
                    && Settings.mode.timer == 0) {
                camera.searchAndLock()
            }
        }

        function unlockAutoFocus() {
            if (camera.captureMode == Camera.CaptureStillImage
                    && focus.focusMode != Camera.FocusInfinity
                    && focus.focusMode != Camera.FocusHyperfocal
                    && focus.focusPointMode == Camera.FocusPointAuto) {
                camera.unlock()
            }
        }

        function captureImage() {
            if (camera.lockStatus != Camera.Searching) {
                _completeCapture()
            } else {
                captureView._captureOnFocus = true
            }
        }

        function record() {
            videoRecorder.outputLocation = Settings.videoCapturePath("mp4")
            startRecordTimer.running = true
            recordStartEvent.play()
        }

        function _captureWithQtMultimedia() {
            camera.imageCapture.captureToLocation(Settings.photoCapturePath('jpg'))
        }

        function _finishCamera2ImageCapture(path, mimeType) {
            shutterEvent.play()
            flashAnimation.start()
            captureView._camera2CaptureRunning = false
            captureView._camera2LiveJpegCaptureRunning = false
            captureView._unload = false
            captureView._captureQueued = false
            captureBusy = false
            if (captureView._camera2ViewfinderActive) {
                window.camera2CaptureBusy = false
            }

            if (captureModel) {
                captureModel.appendCapture(Qt.resolvedUrl(path), mimeType)
            }

            Settings.completePhoto(Qt.resolvedUrl(path))
            captureView.captured()
            camera2CaptureWatchdog.stop()
        }

        function _failCamera2ImageCapture(error) {
            console.warn("Camera2 image capture failed:", error)
            camera2CaptureErrorNotification.publishMessage(error)
            captureView._camera2CapturePending = false
            captureView._camera2CaptureRunning = false
            captureView._camera2LiveJpegCaptureRunning = false
            captureView._captureQueued = false
            captureView._unload = false
            captureBusy = false
            if (captureView._camera2ViewfinderActive) {
                window.camera2CaptureBusy = false
            }
            camera2CaptureWatchdog.stop()
        }

        function _startColdCamera2JpegCapture() {
            if (captureView._camera2ViewfinderActive) {
                window.camera2CaptureBusy = true
            }
            return extensions.captureJpegImage(captureView._camera2CaptureTargetPath,
                                               captureView._camera2CaptureCameraId,
                                               Settings.mode.rawCaptureSize,
                                               Settings.mode.rawCaptureTimeout,
                                               Settings.mode.rawCaptureJpegQuality,
                                               Settings.mode.rawCaptureRotation,
                                               Settings.mode.rawCaptureExposure,
                                               Settings.mode.rawCaptureScene,
                                               Settings.mode.rawCaptureIso,
                                               Settings.mode.rawCaptureShutterNs,
                                               Settings.mode.rawCaptureAperture,
                                               Settings.mode.rawCaptureNoiseReduction,
                                               captureView.camera2Zoom)
        }

        function _retryColdCamera2JpegCapture(error) {
            if (!captureView._camera2LiveJpegCaptureRunning
                    || Settings.mode.camera2CaptureFormat !== "jpeg") {
                return false
            }
            _failCamera2ImageCapture(error)
            return true
        }

        function _startCamera2ImageCapture() {
            if (!captureView._camera2CapturePending || captureView._camera2CaptureRunning) {
                return
            }

            captureView._camera2CapturePending = false
            var captureStarted
            var liveJpegCapture = captureView._liveJpegCapture
            var captureSize = Settings.mode.rawCaptureSize
            if (Settings.mode.camera2CaptureFormat === "jpeg") {
                if (captureView._warmJpegReady) {
                    try {
                        captureStarted = camera2Viewfinder.captureJpeg(captureView._camera2CaptureTargetPath)
                    } catch (error) {
                        _failCamera2ImageCapture("Camera2 warm JPEG exception: " + error)
                        return
                    }
                    captureView._camera2LiveJpegCaptureRunning = captureStarted
                    if (!captureStarted) {
                        _failCamera2ImageCapture("Camera2 warm JPEG preview is not running")
                        return
                    }
                    camera2CaptureWatchdog.restart()
                    captureView._camera2CaptureRunning = true
                } else if (liveJpegCapture) {
                    _failCamera2ImageCapture("Warm JPEG not ready: "
                                             + captureView._captureState())
                    return
                } else if (!liveJpegCapture) {
                    captureView._camera2LiveJpegCaptureRunning = false
                    captureView._camera2CaptureRunning = true
                    captureStarted = _startColdCamera2JpegCapture()
                }
            } else {
                captureView._camera2CaptureRunning = true
                if (captureView._warmRawReady) {
                    captureStarted = camera2Viewfinder.captureRaw(
                                captureView._captureSidecarPath(".warm.raw16"),
                                captureView._captureSidecarPath(".warm.json"))
                    if (!captureStarted) {
                        _failCamera2ImageCapture("Camera2 warm RAW preview is not running")
                        return
                    }
                    camera2CaptureWatchdog.restart()
                    return
                }
                var rawFocusMode = Settings.mode.rawCaptureFocusMode
                var rawFocusTimeout = Settings.mode.rawCaptureFocusTimeout
                if (Settings.mode.rawCaptureSpeedMode === "fast" &&
                        rawFocusMode !== "manual" && rawFocusMode !== "infinity") {
                    rawFocusMode = "none"
                } else if (Settings.mode.rawCaptureSpeedMode === "balanced" &&
                           (rawFocusMode === "auto" || rawFocusMode === "continuous")) {
                    rawFocusTimeout = Math.min(rawFocusTimeout, 1)
                }
                captureStarted = extensions.captureRawImage(captureView._camera2CaptureTargetPath,
                                            captureView._camera2CaptureCameraId,
                                            captureSize,
                                            Settings.mode.rawCaptureTimeout,
                                            rawFocusMode,
                                            Settings.mode.rawCaptureFocusDistance,
                                            rawFocusTimeout,
                                            Settings.mode.rawCaptureFocusFailure,
                                            Settings.mode.rawCaptureExposure,
                                            Settings.mode.rawCaptureJpegQuality,
                                            Settings.mode.rawCaptureRotation,
                                            Settings.global.rawCaptureSaveFormat,
                                            Settings.mode.rawCaptureScene,
                                            Settings.mode.rawCaptureColorTemperature,
                                            Settings.mode.rawCaptureColorTint,
                                            Settings.mode.rawCaptureProgressiveJpeg,
                                            Settings.mode.rawCaptureIso,
                                            Settings.mode.rawCaptureShutterNs,
                                            Settings.mode.rawCaptureAperture,
                                            Settings.mode.rawCaptureNoiseReduction,
                                            captureView.camera2Zoom)
            }
            if (!captureStarted) {
                captureView._captureQueued = false
                captureView._camera2CaptureRunning = false
                captureView._camera2LiveJpegCaptureRunning = false
                captureView._unload = false
                captureBusy = false
                if (captureView._camera2ViewfinderActive) {
                    window.camera2CaptureBusy = false
                }
            }
        }

        function _completeCapture() {
            if (captureBusy) {
                _captureQueued = true
                return
            }

            captureBusy = true
            captureView._camera2CaptureStartedMs = Date.now()
            console.log("capture-timing qml shutter t=0 format="
                        + Settings.mode.camera2CaptureFormat
                        + " speed=" + Settings.mode.rawCaptureSpeedMode)
            if (captureView._liveJpegCapture) {
                console.log("capture-timing qml warm-state "
                            + captureView._warmJpegState())
            }
            var liveJpegCapture = captureView._liveJpegCapture
            var liveWarmCapture = liveJpegCapture || captureView._liveRawCapture
            if (captureView._camera2ViewfinderActive && !liveWarmCapture) {
                captureView._unload = true
                window.camera2CaptureBusy = true
                console.log("capture-timing qml unload-request t="
                            + (Date.now() - captureView._camera2CaptureStartedMs))
            }
            captureOverlay.writeMetaData()

            if (extensions.rawImageCaptureAvailable) {
                captureView._camera2CaptureTargetPath = Settings.photoCapturePath('jpg')
                captureView._camera2CaptureCameraId = "0"
                captureView._camera2CapturePending = true
                captureView._unload = !liveWarmCapture
                if (liveWarmCapture) {
                    _startCamera2ImageCapture()
                } else if (camera.cameraStatus === Camera.UnloadedStatus) {
                    camera2CaptureStartTimer.restart()
                }
            } else {
                _captureWithQtMultimedia()
            }

            if (focusTimer.running) {
                focusTimer.restart()
            }
        }

        function _finishRecording() {
            if (videoRecorder.recorderState == CameraRecorder.StoppedState) {
                videoRecorder.recorderStateChanged.disconnect(_finishRecording)
                extensions.disableNotifications(captureView, false)
                var finalUrl = Settings.completeCapture(videoRecorder.outputLocation)
                if (finalUrl != "") {
                    captureView.recordingStopped(finalUrl, videoRecorder.mediaContainer)
                }
                recordStopEvent.play()
            }
        }

        property bool hasCameraOnBothSides
        property var backFacingCameras

        // On some adaptations media booster makes camera initialization fail
        // and Camera must be reloaded, try to do that once when that happens.
        // Wait until the Camera item has completed and activation has been
        // requested before checking so its construction-time default
        // UnloadedState/UnloadedStatus is not treated as a reload failure.
        property bool reloadCheckEnabled
        property bool needsReload: reloadCheckEnabled
                                   && captureView.effectiveActive
                                   && (camera.errorCode === Camera.CameraError
                                       || (camera.cameraState === Camera.UnloadedState
                                           && camera.cameraStatus === Camera.UnloadedStatus))
        property bool initialized

        Component.onCompleted: reloadCheckEnabled = true

        onErrorCodeChanged: {
            if (errorCode == Camera.CameraError) {
                captureView._unload = true
            }
        }

        onNeedsReloadChanged: {
            if (needsReload) {
                captureView._unload = true
            }
        }

        deviceId: Settings.deviceId
        captureMode: Settings.global.captureMode == "image" ? Camera.CaptureStillImage
                                                            : Camera.CaptureVideo

        onCaptureModeChanged: {
            // Reset flash mode when changing to video mode
            if (initialized && captureMode === Camera.CaptureVideo) {
                Settings.mode.flash = Camera.FlashOff
            }
            captureView._resetFocus()
        }

        cameraState: {
            if (captureView._camera2ViewfinderActive) {
                return Camera.UnloadedState
            } else if (captureView.effectiveActive && !captureView._unload) {
                if (CameraConfigs.ready) {
                    return Camera.ActiveState
                } else {
                    return Camera.LoadedState
                }
            } else {
                return Camera.UnloadedState
            }
        }

        onCameraStateChanged: {
            if ((cameraState == Camera.ActiveState || captureView._camera2ViewfinderActive)
                    && captureOverlay) {
                captureView.loaded()
            }
        }

        onCameraStatusChanged: {
            if (camera.cameraStatus === Camera.ActiveStatus) {
                reactivateTimer.retryCounter = 0
                if (captureView._qtFallbackCapturePending) {
                    captureView._qtFallbackCapturePending = false
                    _captureWithQtMultimedia()
                }
            } else {
                if (!captureView._camera2CapturePending
                        && !captureView._camera2CaptureRunning
                        && !captureView._qtFallbackCapturePending) {
                    _captureQueued = false
                    captureView._camera2LiveJpegCaptureRunning = false
                    captureBusy = false
                }
                if (captureView._camera2CapturePending && camera.cameraStatus === Camera.UnloadedStatus) {
                    camera2CaptureStartTimer.restart()
                }
            }

            var backCameras = []
            if (cameraStatus === Camera.LoadedStatus && !initialized) {
                initialized = true
                var hasFrontFace = false
                var hasBackFace = false

                for (var i = 0; i < QtMultimedia.availableCameras.length; i++) {
                    var device = QtMultimedia.availableCameras[i]
                    if (!hasFrontFace && device.position === Camera.FrontFace) {
                        hasFrontFace = true
                        Settings.global.frontFacingDeviceId = device.deviceId
                    } else if (device.position === Camera.BackFace) {
                        hasBackFace = true
                        backCameras.push(device)
                    }
                }

                backFacingCameras = backCameras

                hasCameraOnBothSides = hasFrontFace && hasBackFace

                if (Settings.global.previousBackFacingDeviceId.length === 0 && backCameras.length > 0) {
                    if (backCameras.indexOf(QtMultimedia.defaultCamera.deviceId) >= 0) {
                        Settings.global.previousBackFacingDeviceId = QtMultimedia.defaultCamera.deviceId
                    } else {
                        Settings.global.previousBackFacingDeviceId = backCameras[0].deviceId
                    }
                }

                // Always disable flash torch at startup
                if (captureMode === Camera.CaptureVideo) {
                    Settings.mode.flash = Camera.FlashOff
                }
            }
        }

        imageCapture {
            resolution: _pickResolution(CameraConfigs.supportedImageResolutions, Settings.aspectRatio)

            onImageSaved: {
                // HDR case emits the exposed already on the first image, delay the feedback so user avoids
                // moving the device until it's safe again.
                if (camera.exposure.exposureMode == Camera.ExposureHDR) {
                    shutterEvent.play()
                    captureAnimation.start()
                }

                camera.unlockAutoFocus()
                captureBusy = false

                if (captureModel) {
                    captureModel.appendCapture(path, "image/jpeg")
                }

                Settings.completePhoto(Qt.resolvedUrl(path))
            }
            onImageExposed: {
                if (camera.exposure.exposureMode != Camera.ExposureHDR) {
                    shutterEvent.play()
                    captureAnimation.start()
                } else {
                    flashAnimation.start()
                }
            }
            onCaptureFailed: {
                camera.unlockAutoFocus()
                captureBusy = false
            }
        }
        videoRecorder {
            resolution: _pickResolution(CameraConfigs.supportedVideoResolutions, CameraConfigs.AspectRatio_16_9)

            audioChannels: 2
            audioSampleRate: Settings.global.audioSampleRate
            audioCodec: Settings.global.audioCodec
            videoCodec: Settings.global.videoCodec
            mediaContainer: Settings.global.mediaContainer

            videoEncodingMode: Settings.global.videoEncodingMode
            videoBitRate: Settings.global.videoBitRate
        }
        focus {
            // could expect that locking focus on auto or continous behaves the same, but
            // continuous doesn't work as well
            focusMode: {
                // The cameraStatus doesn't really matter as a precondition but incorporating
                // it ensures the binding is reevaluated when the status changes and the desired
                // focus mode is assigned. Otherwise QtMultimedia may reject a mode as unsupported
                // and default to auto because the binding was evaluated in the unloaded state and
                // real support was unknown at that time.
                if (camera.cameraStatus == Camera.ActiveStatus && tapFocusActive) {
                    return Camera.FocusAuto
                } else if (CameraConfigs.supportedFocusModes.indexOf(Camera.FocusContinuous) >= 0) {
                    return Camera.FocusContinuous
                } else if (CameraConfigs.supportedFocusModes.length > 0) {
                    return CameraConfigs.supportedFocusModes[0]
                } else {
                    return Camera.FocusAuto
                }
            }
            focusPointMode: tapFocusActive ? Camera.FocusPointCustom : Camera.FocusPointAuto
        }
        flash.mode: Settings.mode.flash
        imageProcessing.whiteBalanceMode: {
            var hasFilter = camera.imageProcessing.colorFilter !== CameraImageProcessing.ColorFilterNone
            return hasFilter ? CameraImageProcessing.WhiteBalanceAuto : Settings.global.whiteBalance
        }

        exposure {
            exposureMode: Settings.mode.exposureMode
            exposureCompensation: Settings.global.exposureCompensation / 2.0
            meteringMode: Settings.mode.meteringMode
        }

        viewfinder {
            resolution: {
                var resolutions = CameraConfigs.supportedViewfinderResolutions
                if (resolutions.length > 0) {
                    return _pickViewfinderResolution(resolutions, Settings.aspectRatio)
                }
                return "-1x-1"
            }

            // Let gst-droid decide the best framerate
        }

        metaData {
            orientation: captureView.captureOrientation
            cameraModel: deviceInfo.prettyName
            cameraManufacturer: deviceInfo.manufacturer
        }

        focus.onFocusModeChanged: camera.unlock()

        onLockStatusChanged: {
            if (lockStatus != Camera.Searching && captureView._captureOnFocus) {
                captureView._captureOnFocus = false
                camera._completeCapture()
            }
        }
    }

    Binding {
        target: CameraConfigs
        property: "camera"
        value: camera
    }

    DeviceInfo {
        id: deviceInfo
    }

    CameraExtensions {
        id: extensions

        onRawImageCaptured: {
            camera._finishCamera2ImageCapture(path, mimeType)
        }

        onRawImageCaptureFailed: {
            camera._failCamera2ImageCapture(error)
        }
    }

    Connections {
        target: captureView.camera2Viewfinder
        ignoreUnknownSignals: true

        onImageCaptured: {
            camera._finishCamera2ImageCapture(path, mimeType)
        }

        onRawImageReady: {
            extensions.processRawImage(captureView._camera2CaptureTargetPath,
                                       rawPath,
                                       metadataPath,
                                       Settings.mode.rawCaptureExposure,
                                       Settings.mode.rawCaptureJpegQuality,
                                       Settings.mode.rawCaptureRotation,
                                       Settings.global.rawCaptureSaveFormat,
                                       Settings.mode.rawCaptureColorTemperature,
                                       Settings.mode.rawCaptureColorTint,
                                       Settings.mode.rawCaptureProgressiveJpeg)
        }

        onImageCaptureFailed: {
            if (!camera._retryColdCamera2JpegCapture(error)) {
                camera._failCamera2ImageCapture(error)
            }
        }
    }

    Timer {
        id: camera2CaptureStartTimer

        interval: 250
        repeat: false
        onTriggered: {
            console.log("capture-timing qml start-timer-fired t="
                        + (Date.now() - captureView._camera2CaptureStartedMs))
            camera._startCamera2ImageCapture()
        }
    }

    Timer {
        id: camera2CaptureWatchdog

        interval: captureView._camera2LiveJpegCaptureRunning
                  ? 5000 : Math.max(1000, Settings.mode.rawCaptureTimeout * 1000)
        repeat: false
        onTriggered: {
            if (captureView._camera2CaptureRunning) {
                camera._failCamera2ImageCapture(
                            "Warm JPEG timed out after "
                            + (Date.now() - captureView._camera2CaptureStartedMs)
                            + "ms: " + captureView._captureState())
            }
        }
    }

    Binding {
        target: captureView.viewfinder
        property: "source"
        value: camera
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "zoom"
        value: captureView.camera2Zoom
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "focusMode"
        value: Settings.mode.rawCaptureFocusMode
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "focusDistance"
        value: parseFloat(Settings.mode.rawCaptureFocusDistance)
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "exposureCompensation"
        value: Settings.global.exposureCompensation
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "sensorSensitivity"
        value: Settings.mode.rawCaptureIso
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "exposureTime"
        value: Settings.mode.rawCaptureShutterNs
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "aperture"
        value: Settings.mode.rawCaptureAperture
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "noiseReduction"
        value: Settings.mode.rawCaptureNoiseReduction
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "renderExposure"
        value: parseFloat(Settings.mode.rawCaptureExposure)
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "sceneMode"
        value: Settings.mode.rawCaptureScene
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "colorTemperature"
        value: Settings.mode.camera2CaptureFormat === "raw"
               ? Settings.mode.rawCaptureColorTemperature : 0
    }

    Binding {
        target: captureView._camera2ViewfinderActive ? captureView.camera2Viewfinder : null
        property: "colorTint"
        value: Settings.mode.camera2CaptureFormat === "raw"
               ? Settings.mode.rawCaptureColorTint : 0
    }

    Rectangle {
        id: flashRectangle

        anchors.fill: parent
        color: "white"
        opacity: 0
    }

    SequentialAnimation {
        id: flashAnimation

        PropertyAction {
            target: flashRectangle
            property: "visible"
            value: true
        }
        OpacityAnimator {
            target: flashRectangle
            from: Theme.opacityHigh
            to: 0
            duration: 250
        }
        PropertyAction {
            target: flashRectangle
            property: "visible"
            value: false
        }
    }

    SequentialAnimation {
        id: captureAnimation

        PropertyAction {
            target: captureSnapshot
            property: "sourceItem"
            value: viewfinder
        }
        ScriptAction {
            script: captureSnapshotEffect.scheduleUpdate()
        }
        PropertyAction {
            target: captureSnapshot
            property: "x"
            value: 0
        }
        PropertyAction {
            target: captureSnapshot
            property: "visible"
            value: true
        }
        PropertyAction {
            target: viewfinder
            property: "opacity"
            value: 0
        }
        PauseAnimation {
            duration: 100
        }
        ParallelAnimation {
            XAnimator {
                target: captureSnapshot
                from: 0
                to: captureView.isPortrait ? -captureView.height : -captureView.width
                duration: 300
                easing.type: Easing.InQuad
            }
            OpacityAnimator {
                target: viewfinder
                to: 1
                duration: 300
            }
        }
        PropertyAction {
            target: captureSnapshot
            property: "visible"
            value: false
        }
        PropertyAction {
            target: captureSnapshot
            property: "sourceItem"
            value: null
        }
        ScriptAction {
            script: captureView.captured()
        }
    }

    property Component overlayComponent
    property var overlayIncubator

    function loadOverlay() {
        overlayComponent = Qt.createComponent("CaptureOverlay.qml", Component.Asynchronous, captureView)
        if (overlayComponent) {
            if (overlayComponent.status === Component.Ready) {
                incubateOverlay()
            } else if (overlayComponent.status === Component.Loading) {
                overlayComponent.statusChanged.connect(
                    function(status) {
                        if (overlayComponent) {
                            if (status == Component.Ready) {
                                incubateOverlay()
                            } else if (status == Component.Error) {
                                console.warn(overlayComponent.errorString())
                            }
                        }
                    })
            } else {
                console.log("Error loading capture overlay", overlayComponent.errorString())
            }
        }
    }

    function incubateOverlay() {
        overlayIncubator = overlayComponent.incubateObject(captureView,
                                                           { "captureView": captureView,
                                                             "camera": camera,
                                                             "focusArea": focusArea
                                                           }, Qt.Asynchronous)
        overlayIncubator.onStatusChanged = function(status) {
            if (status == Component.Ready) {
                captureOverlay = overlayIncubator.object
                captureOverlay.orientationTransitionRunning = Qt.binding(function () {
                    return captureView.orientationTransitionRunning
                })
                overlayFadeIn.start()
                overlayIncubator = null
                if ((camera.cameraState == Camera.ActiveState || captureView._camera2ViewfinderActive)
                        && captureOverlay) {
                    captureView.loaded()
                }
            } else if (status == Component.Error) {
                console.log("Failed to create capture overlay")
                overlayIncubator = null
            }
        }
    }

    FadeAnimator {
        id: overlayFadeIn

        target: captureOverlay
        to: 1.0
        duration: 100
    }

    Item {
        id: focusArea

        width: captureView._camera2ViewfinderActive
               ? captureView._camera2PreviewWidth
               : Screen.width * captureView._viewfinderAspectRatio
        height: captureView._camera2ViewfinderActive
                ? captureView._camera2PreviewHeight
                : Screen.width

        rotation: captureView._camera2ViewfinderActive
                  ? 0 : -captureView.viewfinderOrientation
        anchors {
            centerIn: parent
            verticalCenterOffset: (captureView._camera2ViewfinderActive
                                   ? captureView.camera2TopInset + focusArea.height / 2
                                     - parent.height / 2
                                   : (isPortrait ? viewfinderOffset : 0))
            horizontalCenterOffset: captureView._camera2ViewfinderActive
                                    ? focusArea.width / 2 - parent.width / 2
                                    : (isPortrait ? 0 : viewfinderOffset)
        }
        opacity: captureOverlay ? 1.0 - captureOverlay.settingsOpacity : 1.0

        Repeater {
            model: captureView._camera2ViewfinderActive
                   ? (captureView.tapFocusActive ? 1 : 0)
                   : camera.focus.focusZones
            delegate: Item {
                readonly property bool camera2FocusZone: captureView._camera2ViewfinderActive
                readonly property var zoneArea: camera2FocusZone
                                                ? Qt.rect(Math.max(0, Math.min(0.85, captureView._camera2FocusPoint.x - 0.075)),
                                                          Math.max(0, Math.min(0.85, captureView._camera2FocusPoint.y - 0.075)),
                                                          0.15, 0.15)
                                                : area
                readonly property int zoneStatus: camera2FocusZone
                                                  ? Camera.FocusAreaFocused
                                                  : status

                x: focusArea.width * (captureView._horizontalMirror
                                      ? 1 - zoneArea.x - zoneArea.width
                                      : zoneArea.x)
                y: focusArea.height * (captureView._verticalMirror
                                      ? 1 - zoneArea.y - zoneArea.height
                                      : zoneArea.y)
                width: focusArea.width * zoneArea.width
                height: focusArea.height * zoneArea.height

                visible: camera2FocusZone
                         || (zoneStatus != Camera.FocusAreaUnused
                             && camera.focus.focusPointMode == Camera.FocusPointCustom)

                Rectangle {
                    width: Math.min(parent.width, parent.height)
                    height: width
                    anchors.centerIn: parent
                    radius: width / 2
                    border {
                        width: Math.round(Theme.pixelRatio * 2)
                        color: zoneStatus == Camera.FocusAreaFocused
                               ? (Theme.colorScheme == Theme.LightOnDark
                                  ? Theme.highlightColor
                                  : Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark))
                               : "white"
                    }
                    color: "#00000000"
                }
            }
        }
    }

    Timer {
        id: focusTimer

        interval: 5000
        onTriggered: {
            if (!captureTimer.running) {
                captureView._resetFocus()
            } else {
                captureTimer.resetCameraOnStop = true
            }
        }
    }

    Keys.onVolumeDownPressed: {
        if (handleVolumeKeys && !event.isAutoRepeat) {
            if (_manualCamera2Focus) {
                _changeManualFocusDistance(-1)
            } else {
                camera.lockAutoFocus()
                captureOnVolumeRelease = true
            }
        }
    }
    Keys.onVolumeUpPressed: {
        if (handleVolumeKeys && !event.isAutoRepeat) {
            if (_manualCamera2Focus) {
                _changeManualFocusDistance(1)
            } else {
                camera.lockAutoFocus()
                captureOnVolumeRelease = true
            }
        }
    }

    function supportedKey(key) {
        return key === Qt.Key_CameraFocus
                || key === Qt.Key_Camera
                || key === Qt.Key_VolumeDown
                || key === Qt.Key_VolumeUp
    }

    Keys.onPressed: {
        if (supportedKey(event.key)) {
            event.accepted = true
        }

        if (event.isAutoRepeat) {
            return
        }

        if (event.key == Qt.Key_CameraFocus) {
            camera.lockAutoFocus()
        } else if (event.key == Qt.Key_Camera) {
            captureView._triggerCapture() // key having half-pressed state too so can capture already here
        }
    }

    Keys.onReleased: {
        if (supportedKey(event.key)) {
            event.accepted = true
        }

        if (event.isAutoRepeat) {
            return
        }

        if (event.key == Qt.Key_CameraFocus) {
            // note: forces capture if it was still pending. debatable if that should be allowed to finish.
            camera.unlockAutoFocus()
        } else if ((event.key == Qt.Key_VolumeDown || event.key == Qt.Key_VolumeUp)
                   && captureOnVolumeRelease && handleVolumeKeys) {
            captureView._triggerCapture()
        }
    }

    Permissions {
        enabled: captureView.activeFocus
                    && camera.captureMode == Camera.CaptureStillImage
                    && (camera.cameraState == Camera.ActiveState
                        || captureView._camera2ViewfinderActive)
        autoRelease: true
        applicationClass: "camera"

        Resource {
            id: keysResource

            type: Resource.ScaleButton
            optional: true
        }
    }

    Permissions {
        enabled: Qt.application.state == Qt.ApplicationActive
        autoRelease: true
        applicationClass: "camera"

        Resource {
            type: Resource.SnapButton
            optional: true
        }
    }

    AboutSettings {
        id: aboutSettings
    }
}
