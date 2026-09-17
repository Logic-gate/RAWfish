// SPDX-FileCopyrightText: 2014 - 2022 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtMultimedia 5.6
import Nemo.Configuration 1.0
import com.vivid.camera 1.0

SettingsBase {
    property alias mode: modeSettings
    property alias global: globalSettings
    // Camera change goes here, CaptureView updates to global.deviceId
    property string deviceId: global.deviceId

    readonly property int aspectRatio: mode.aspectRatio
    readonly property int exposureCompensationDefault: 0
    property var viewfinderGridValues: [ "none", "thirds" ]

    readonly property var settingsDefaults: ({
                                                 "iso": 0,
                                                 "timer": 0,
                                                 "viewfinderGrid": "none",
                                                 "camera2CaptureFormat": "jpeg",
                                                 "rawCaptureSpeedMode": "balanced",
                                                 "rawCaptureSize": "4096x3072",
                                                 "rawCaptureFocusMode": "auto",
                                                 "rawCaptureFocusDistance": "0",
                                                 "rawCaptureTimeout": 30,
                                                 "rawCaptureFocusTimeout": 3,
                                                 "rawCaptureFocusFailure": "capture",
                                                 "rawCaptureExposure": "1.0",
                                                 "rawCaptureIso": 0,
                                                 "rawCaptureShutterNs": "0",
                                                 "rawCaptureAperture": 0,
                                                 "rawCaptureNoiseReduction": 0,
                                                 "rawCaptureJpegQuality": 92,
                                                 "rawCaptureRotation": 90,
                                                 "rawCaptureScene": "manual",
                                                 "rawCaptureProgressiveJpeg": false,
                                                 "rawCaptureColorTemperature": 0,
                                                 "rawCaptureColorTint": 0,
                                                 "camera2Viewfinder": true,
                                                 "exposureMode": Camera.ExposureManual,
                                                 "flash": ((globalSettings.captureMode == "image")
                                                           && (globalSettings.position === Camera.BackFace)
                                                           ? Camera.FlashAuto : Camera.FlashOff)
                                             })

    readonly property bool defaultSettings: modeSettings.iso === settingsDefaults["iso"]
                                            && modeSettings.timer === settingsDefaults["timer"]
                                            && modeSettings.viewfinderGrid === settingsDefaults["viewfinderGrid"]
                                            && modeSettings.camera2CaptureFormat === settingsDefaults["camera2CaptureFormat"]
                                            && modeSettings.rawCaptureSpeedMode === settingsDefaults["rawCaptureSpeedMode"]
                                            && modeSettings.rawCaptureSize === settingsDefaults["rawCaptureSize"]
                                            && modeSettings.rawCaptureFocusMode === settingsDefaults["rawCaptureFocusMode"]
                                            && modeSettings.rawCaptureFocusDistance === settingsDefaults["rawCaptureFocusDistance"]
                                            && modeSettings.rawCaptureTimeout === settingsDefaults["rawCaptureTimeout"]
                                            && modeSettings.rawCaptureFocusTimeout === settingsDefaults["rawCaptureFocusTimeout"]
                                            && modeSettings.rawCaptureFocusFailure === settingsDefaults["rawCaptureFocusFailure"]
                                            && modeSettings.rawCaptureExposure === settingsDefaults["rawCaptureExposure"]
                                            && modeSettings.rawCaptureIso === settingsDefaults["rawCaptureIso"]
                                            && modeSettings.rawCaptureShutterNs === settingsDefaults["rawCaptureShutterNs"]
                                            && modeSettings.rawCaptureAperture === settingsDefaults["rawCaptureAperture"]
                                            && modeSettings.rawCaptureNoiseReduction === settingsDefaults["rawCaptureNoiseReduction"]
                                            && modeSettings.rawCaptureJpegQuality === settingsDefaults["rawCaptureJpegQuality"]
                                            && modeSettings.rawCaptureRotation === settingsDefaults["rawCaptureRotation"]
                                            && modeSettings.rawCaptureScene === settingsDefaults["rawCaptureScene"]
                                            && modeSettings.rawCaptureProgressiveJpeg === settingsDefaults["rawCaptureProgressiveJpeg"]
                                            && modeSettings.rawCaptureColorTemperature === settingsDefaults["rawCaptureColorTemperature"]
                                            && modeSettings.rawCaptureColorTint === settingsDefaults["rawCaptureColorTint"]
                                            && modeSettings.camera2Viewfinder === settingsDefaults["camera2Viewfinder"]
                                            && globalSettings.exposureCompensation === exposureCompensationDefault
                                            && modeSettings.exposureMode === settingsDefaults["exposureMode"]
                                            && modeSettings.flash == settingsDefaults["flash"]

    function reset() {
        var basePath = globalSettings.path + "/" + modeSettings.path
        var i
        for (i in settingsDefaults) {
            _singleValue.key = basePath + "/" + i
            _singleValue.value = settingsDefaults[i]
        }
        _singleValue.key = globalSettings.path + "/exposureCompensation"
        _singleValue.value = exposureCompensationDefault
    }

    property ConfigurationValue _singleValue: ConfigurationValue {}

    property ConfigurationGroup _global: ConfigurationGroup {
        id: globalSettings

        path: "/apps/rawfish"

        // Note! don't touch this for changing between cameras, see cameraDevice on root
        property string deviceId
        property string previousBackFacingDeviceId
        property string frontFacingDeviceId
        property int position: Camera.BackFace
        property string captureMode: "image"

        // Need to be defined by adaptation to enable multiple back cameras,
        // e.g. normal, macro and wide angle camera labels could be ["1.0", "2.0", "0.6"]
        property var backCameraLabels: []

        property int portraitCaptureButtonLocation: 3
        property int landscapeCaptureButtonLocation: 5

        property string audioCodec: "audio/mpeg, mpegversion=(int)4"
        property int audioSampleRate: 48000
        property string videoCodec: "video/x-h264"
        property string mediaContainer: "video/quicktime, variant=(string)iso"

        property int videoEncodingMode: CameraRecorder.AverageBitRateEncoding
        property int videoBitRate: 12000000

        property bool saveLocationInfo
        property string rawCaptureSaveFormat: saveRawCaptureFiles ? "raw16" : "none"
        property bool saveRawCaptureFiles: true

        property bool qrFilterEnabled: false
        property bool colorFiltersEnabled: false
        property bool colorFiltersAllowed: true

        property int exposureCompensation: exposureCompensationDefault
        property int whiteBalance: CameraImageProcessing.WhiteBalanceAuto

        property var exposureCompensationValues: [ 4, 3, 2, 1, 0, -1, -2, -3, -4 ]
        property string viewfinderGrid: "none"

        onPositionChanged: {
            normalizeCamera2Settings()
        }

        Component.onCompleted: {
            exposureCompensation = exposureCompensationDefault
        }

        ConfigurationGroup {
            id: modeSettings

            path: {
                var position = globalSettings.position === Camera.FrontFace ? "front" : "back"
                return position + "/" + globalSettings.captureMode
            }

            property int iso: 0
            property int flash: Camera.FlashOff
            property int exposureMode: Camera.ExposureManual
            property int meteringMode: Camera.MeteringMatrix
            property int timer: 0
            property int aspectRatio: -1
            property string camera2CaptureFormat: "jpeg"
            property string rawCaptureSpeedMode: "balanced"
            property string rawCaptureSize: "4096x3072"
            property string rawCaptureFocusMode: "auto"
            property string rawCaptureFocusDistance: "0"
            property int rawCaptureTimeout: 30
            property int rawCaptureFocusTimeout: 3
            property string rawCaptureFocusFailure: "capture"
            property string rawCaptureExposure: "1.0"
            property int rawCaptureIso: 0
            property string rawCaptureShutterNs: "0"
            property int rawCaptureAperture: 0
            property int rawCaptureNoiseReduction: 0
            property int rawCaptureJpegQuality: 92
            property int rawCaptureRotation: 90
            property string rawCaptureScene: "manual"
            property bool rawCaptureProgressiveJpeg: false
            property int rawCaptureColorTemperature: 0
            property int rawCaptureColorTint: 0
            property bool camera2Viewfinder: true

            onCamera2CaptureFormatChanged: normalizeCamera2Settings()

            Component.onCompleted: {
                rawCaptureIso = settingsDefaults["rawCaptureIso"]
                rawCaptureShutterNs = settingsDefaults["rawCaptureShutterNs"]
                if (rawCaptureScene === "none") {
                    rawCaptureScene = "manual"
                }
                normalizeCamera2Settings()
                if (aspectRatio === -1) {
                    if (globalSettings.captureMode === "image") {
                        aspectRatio = CameraConfigs.AspectRatio_4_3
                    } else {
                        aspectRatio = CameraConfigs.AspectRatio_16_9
                    }
                }
            }
        }
    }

    function captureModeIcon(mode) {
        switch (mode) {
        case "image": return "image://theme/icon-camera-camera-mode"
        case "video": return "image://theme/icon-camera-video"
        default:  return ""
        }
    }

    function exposureText(exposure) {
        switch (exposure) {
        case -4: return "-2"
        case -3: return "-1.5"
        case -2: return "-1"
        case -1: return "-0.5"
        case 0:  return ""
        case 1:  return "+0.5"
        case 2:  return "+1"
        case 3:  return "+1.5"
        case 4:  return "+2"
        }
    }

    function timerIcon(timer) {
        return timer > 0
                ? "image://theme/icon-camera-timer-" + timer + "s"
                : "image://theme/icon-camera-timer"
    }

    function timerText(timer) {
        return timer > 0
                ? timer + " second delay"
                : "No delay"
    }

    function rawCaptureSizeText(size) {
        return "Size " + size
    }

    function camera2SizeModel(format) {
        if (format === "raw") {
            return [ "4096x3072", "3264x2448", "3072x1728", "2560x1920",
                     "1920x1080" ]
        }
        return [ "8192x6144", "4096x3072", "4096x2304", "3264x2448",
                 "3072x1728", "2560x1920", "1920x1080", "1600x1200",
                 "1280x720", "640x480" ]
    }

    function camera2SceneModel() {
        return [ "manual", "auto", "action", "portrait", "landscape",
                 "sport", "night", "night-portrait", "theatre", "beach",
                 "snow", "sunset", "steady-photo", "fireworks", "party",
                 "candlelight", "barcode", "hdr" ]
    }

    function camera2NoiseReductionModel() {
        return [ 0, 1, 2, 3, 4 ]
    }

    function normalizeCamera2Settings() {
        var sizes = camera2SizeModel(modeSettings.camera2CaptureFormat)
        if (sizes.indexOf(modeSettings.rawCaptureSize) < 0) {
            modeSettings.rawCaptureSize = sizes[0]
        }
        if (camera2SpeedModel().indexOf(modeSettings.rawCaptureSpeedMode) < 0) {
            modeSettings.rawCaptureSpeedMode = settingsDefaults["rawCaptureSpeedMode"]
        }
        if (modeSettings.rawCaptureIso === undefined ||
                modeSettings.rawCaptureIso === null) {
            modeSettings.rawCaptureIso = settingsDefaults["rawCaptureIso"]
        }
        if (!modeSettings.rawCaptureShutterNs) {
            modeSettings.rawCaptureShutterNs = settingsDefaults["rawCaptureShutterNs"]
        }
        var isoModel = camera2IsoModel()
        var isoIndex = isoModel.indexOf(modeSettings.rawCaptureIso)
        if (isoIndex < 0) {
            isoIndex = isoModel.indexOf(parseInt(modeSettings.rawCaptureIso))
        }
        modeSettings.rawCaptureIso = isoIndex >= 0
                ? isoModel[isoIndex] : settingsDefaults["rawCaptureIso"]
        var shutterModel = camera2ShutterModel()
        var shutterIndex = shutterModel.indexOf(String(modeSettings.rawCaptureShutterNs))
        if (shutterIndex < 0) {
            shutterIndex = shutterModel.indexOf(modeSettings.rawCaptureShutterNs)
        }
        modeSettings.rawCaptureShutterNs = shutterIndex >= 0
                ? shutterModel[shutterIndex] : settingsDefaults["rawCaptureShutterNs"]
        if (camera2FocusDistanceModel().indexOf(modeSettings.rawCaptureFocusDistance) < 0) {
            modeSettings.rawCaptureFocusDistance = settingsDefaults["rawCaptureFocusDistance"]
        }
        if (camera2SceneModel().indexOf(modeSettings.rawCaptureScene) < 0) {
            modeSettings.rawCaptureScene = "manual"
        }
        if (camera2NoiseReductionModel().indexOf(modeSettings.rawCaptureNoiseReduction) < 0) {
            modeSettings.rawCaptureNoiseReduction = 0
        }
    }

    function camera2CaptureFormatText(format) {
        return format === "jpeg" ? "Direct JPEG" : "RAW render"
    }

    function camera2SpeedModel() {
        return [ "fast", "balanced", "quality" ]
    }

    function rawCaptureSpeedText(mode) {
        return "Speed " + rawCaptureSpeedLabel(mode)
    }

    function rawCaptureSpeedLabel(mode) {
        switch (mode) {
        case "fast": return "Fast"
        case "quality": return "Quality"
        default: return "Balanced"
        }
    }

    function rawCaptureFocusModeText(mode) {
        switch (mode) {
        case "auto": return "Focus auto"
        case "continuous": return "Focus continuous"
        case "manual": return "Focus manual"
        case "infinity": return "Focus infinity"
        case "none": return "Focus none"
        default: return "Focus " + mode
        }
    }

    function rawCaptureFocusDistanceText(distance) {
        return "Focus " + rawCaptureFocusDistanceLabel(distance)
    }

    function rawCaptureFocusDistanceLabel(distance) {
        var diopters = parseFloat(distance)
        if (!diopters || diopters <= 0) {
            return "Infinity"
        }

        var meters = 1.0 / diopters
        return meters >= 1.0
                ? meters.toFixed(meters >= 10 ? 0 : 1) + " m"
                : Math.round(meters * 100) + " cm"
    }

    function camera2FocusDistanceModel() {
        return [ "0", "0.25", "0.5", "0.75", "1", "1.5", "2", "3",
                 "4", "5", "7.5", "10", "15", "20" ]
    }

    function rawCaptureTimeoutText(timeout) {
        return "Capture " + timeout + " s"
    }

    function rawCaptureFocusTimeoutText(timeout) {
        return "AF " + timeout + " s"
    }

    function rawCaptureFocusFailureText(policy) {
        return policy === "abort" ? "AF abort" : "AF capture"
    }

    function rawCaptureExposureText(exposure) {
        return "Exposure x" + exposure
    }

    function rawCaptureIsoText(iso) {
        return iso > 0 ? "ISO " + iso : "ISO auto"
    }

    function rawCaptureShutterText(shutterNs) {
        return "Shutter " + rawCaptureShutterLabel(shutterNs)
    }

    function rawCaptureShutterLabel(shutterNs) {
        switch (shutterNs) {
        case "100000": return "1/10000"
        case "250000": return "1/4000"
        case "500000": return "1/2000"
        case "1000000": return "1/1000"
        case "2000000": return "1/500"
        case "4000000": return "1/250"
        case "8333333": return "1/120"
        case "16666667": return "1/60"
        case "33333333": return "1/30"
        case "66666667": return "1/15"
        case "125000000": return "1/8"
        case "250000000": return "1/4"
        case "400000000": return "0.4s"
        case "500000000": return "1/2"
        case "1000000000": return "1s"
        case "2000000000": return "2s"
        case "4000000000": return "4s"
        case "8000000000": return "8s"
        case "16000000000": return "16s"
        default:
            var ns = parseInt(shutterNs)
            if (ns > 0) {
                return ns < 1000000000
                        ? "1/" + Math.round(1000000000 / ns)
                        : (ns / 1000000000).toFixed(ns % 1000000000 === 0 ? 0 : 1) + "s"
            }
            return "auto"
        }
    }

    function camera2IsoModel() {
        return [ 0, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 19200 ]
    }

    function camera2ShutterModel() {
        var model = [ "0", "100000", "250000", "500000", "1000000",
                      "2000000", "4000000", "8333333", "16666667",
                      "33333333", "66666667", "125000000", "250000000" ]
        model.push("500000000", "1000000000", "2000000000", "4000000000",
                   "8000000000", "16000000000")
        return model
    }

    function rawCaptureApertureText(aperture) {
        return aperture > 0 ? "Aperture f/" + rawCaptureApertureLabel(aperture)
                            : "Aperture auto"
    }

    function rawCaptureApertureLabel(aperture) {
        return aperture > 0 ? (aperture / 10).toFixed(1) : "Auto"
    }

    function rawCaptureNoiseReductionText(mode) {
        return "Noise " + rawCaptureNoiseReductionLabel(mode)
    }

    function rawCaptureNoiseReductionLabel(mode) {
        switch (mode) {
        case 1: return "Fast"
        case 2: return "High"
        case 3: return "Minimal"
        case 4: return "ZSL"
        default: return "Off"
        }
    }

    function rawCaptureJpegQualityText(quality) {
        return "JPEG " + quality
    }

    function rawCaptureRotationText(rotation) {
        return rotation === 0 ? "Rotate 0" : "Rotate " + rotation
    }

    function rawCaptureSceneText(scene) {
        return "Scene " + rawCaptureSceneLabel(scene)
    }

    function rawCaptureSceneLabel(scene) {
        switch (scene) {
        case "portrait": return "Portrait"
        case "landscape": return "Landscape"
        case "sport": return "Sport"
        case "night": return "Night"
        case "auto": return "Auto"
        case "action": return "Action"
        case "night-portrait": return "Night portrait"
        case "theatre": return "Theatre"
        case "beach": return "Beach"
        case "snow": return "Snow"
        case "sunset": return "Sunset"
        case "steady-photo": return "Steady photo"
        case "fireworks": return "Fireworks"
        case "party": return "Party"
        case "candlelight": return "Candlelight"
        case "barcode": return "Barcode"
        case "hdr": return "HDR"
        default: return "Manual"
        }
    }

    function rawCaptureProgressiveJpegText(enabled) {
        return enabled ? "Progressive JPEG" : "Standard JPEG"
    }

    function rawCaptureColorTemperatureText(temperature) {
        return temperature > 0 ? "WB " + temperature + " K" : "Auto WB"
    }

    function rawCaptureColorTintText(tint) {
        return tint === 0 ? "Tint 0" : "Tint " + tint
    }

    function colorFiltersIcon(enabled) {
        return "image://theme/icon-camera-filter-" + (enabled ? "on" : "off")
    }

    function colorFiltersEnabledText(enabled) {
        return enabled
                ? "Color filters on"
                : "Color filters off"
    }

    function isoText(iso) {
        if (iso == 0) {
            return "Light sensitivity - Automatic"
        } else {
            return "Light sensitivity - ISO " + iso
        }
    }

    function meteringModeIcon(mode) {
        switch (mode) {
        case Camera.MeteringMatrix:  return "image://theme/icon-camera-metering-matrix"
        case Camera.MeteringAverage: return "image://theme/icon-camera-metering-weighted"
        case Camera.MeteringSpot:    return "image://theme/icon-camera-metering-spot"
        }
    }

    function exposureModeIcon(exposureMode) {
        switch (exposureMode) {
        case Camera.ExposureManual:         return "image://theme/icon-camera-mode-automatic"
        case Camera.ExposurePortrait:       return "image://theme/icon-camera-mode-portrait"
        case Camera.ExposureNight:          return "image://theme/icon-camera-mode-night"
        case Camera.ExposureSports:         return "image://theme/icon-camera-mode-sports"
        case Camera.ExposureHDR:            return "image://theme/icon-camera-mode-hdr"
        default:
            return "" // not supported
        }
    }

    function exposureModeText(exposureMode) {
        switch (exposureMode) {
        case Camera.ExposureManual:         return "Automatic exposure"
        case Camera.ExposurePortrait:       return "Portrait exposure"
        case Camera.ExposureNight:          return "Night exposure"
        case Camera.ExposureSports:         return "Sports exposure"
        case Camera.ExposureHDR:            return "HDR exposure"
        default:
            return "" // not supported
        }
    }

    function flashIcon(flash) {
        switch (flash) {
        case Camera.FlashAuto:              return "image://theme/icon-camera-flash-automatic"
        case Camera.FlashOff:               return "image://theme/icon-camera-flash-off"
        case Camera.FlashTorch:
        case Camera.FlashOn:                return "image://theme/icon-camera-flash-on"
        // JB#54201: Red-eye mode does not work
        // case Camera.FlashRedEyeReduction:   return "image://theme/icon-camera-flash-redeye"
        default:
            return "" // not supported
        }
    }

    function flashText(flash) {
        switch (flash) {
        case Camera.FlashAuto:       return "Flash automatic"
        case Camera.FlashOff:        return "Flash disabled"
        case Camera.FlashOn:         return "Flash enabled"
        case Camera.FlashTorch:      return "Flash on"
        case Camera.FlashRedEyeReduction: return "Flash red eye"
        default:
            return "" // not supported
        }
    }

    function whiteBalanceIcon(balance) {
        switch (balance) {
        case CameraImageProcessing.WhiteBalanceAuto:        return "image://theme/icon-camera-wb-automatic"
        case CameraImageProcessing.WhiteBalanceSunlight:    return "image://theme/icon-camera-wb-sunny"
        case CameraImageProcessing.WhiteBalanceCloudy:      return "image://theme/icon-camera-wb-cloudy"
        // case CameraImageProcessing.WhiteBalanceShade:       return "image://theme/icon-camera-wb-shade"
        // case CameraImageProcessing.WhiteBalanceSunset:      return "image://theme/icon-camera-wb-sunset"
        case CameraImageProcessing.WhiteBalanceFluorescent: return "image://theme/icon-camera-wb-fluorecent"
        case CameraImageProcessing.WhiteBalanceTungsten:    return "image://theme/icon-camera-wb-tungsten"
        default:
            return "" // not supported
        }
    }

    function whiteBalanceText(balance) {
        switch (balance) {
        case CameraImageProcessing.WhiteBalanceAuto:        return "Automatic"
        case CameraImageProcessing.WhiteBalanceSunlight:    return "Sunny"
        case CameraImageProcessing.WhiteBalanceCloudy:      return "Cloudy"
        case CameraImageProcessing.WhiteBalanceShade:       return "Shade"
        case CameraImageProcessing.WhiteBalanceSunset:      return "Sunset"
        case CameraImageProcessing.WhiteBalanceFluorescent: return "Fluorescent"
        case CameraImageProcessing.WhiteBalanceTungsten:    return "Tungsten"
        default:
            return "" // not supported
        }
    }

    function colorFilterText(filter) {
        switch (filter) {
        case CameraImageProcessing.ColorFilterNone:
            return "Normal"
        case CameraImageProcessing.ColorFilterGrayscale:
            return "Grayscale"
        case CameraImageProcessing.ColorFilterNegative:
            return "Negative"
        case CameraImageProcessing.ColorFilterSolarize:
            return "Solarize"
        case CameraImageProcessing.ColorFilterSepia:
            return "Sepia"
        case CameraImageProcessing.ColorFilterPosterize:
            return "Posterize"
        case CameraImageProcessing.ColorFilterWhiteboard:
            return "Whiteboard"
        case CameraImageProcessing.ColorFilterBlackboard:
            return "Blackboard"
        case CameraImageProcessing.ColorFilterAqua:
            return "Aqua"
        default:
            return "" // not supported
        }
    }

    function viewfinderGridIcon(grid) {
        switch (grid) {
        case "none": return "image://theme/icon-camera-grid-none"
        case "thirds": return "image://theme/icon-camera-grid-thirds"
        default: return ""
        }
    }

    function viewfinderGridText(grid) {
        switch (grid) {
        case "none":
            return "No grid"
        case "thirds":
            return "Thirds grid"
        default: return ""
        }
    }
}
