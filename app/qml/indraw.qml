/*
 * indraw.qml -- the camera page: display and gestures (docs/ARCHITECTURE.md).
 *
 * One page, as Jolla Camera: a horizontal strip (the switcher) holds the
 * gallery (GalleryView.qml) and, to its right, the camera. The camera,
 * portrait, top to bottom: the black notch band with two text buttons
 * either side of the cutout (zebra, peak / level, AE); the viewfinder, at
 * the photo's proportions; the control deck (focal / type / focus cycle
 * buttons, the histogram and the shutter with the EV wheel and the fine
 * white balance, the speed and ISO wheels), scaled down on small screens.
 *
 * The camera's behaviour is not here: the controller in the engine process
 * decides the exposure (ExposureLogic.qml is its view, "xp" commands and
 * "xpstate"), the focus sequences ("fc" / "fcstate"), the geometry of zoom,
 * loupe and taps ("vw" / "vwstate") and what a shutter press becomes ("sh").
 * This page sends gestures in display terms (its view rotation undone),
 * draws the states it is told (the wheels, the circle, the view window),
 * and keeps what is purely display: the level, the grid, zebra and peaking,
 * the menu, the gallery, the self-timer, the stack overlay. All text qsTr().
 */
import QtQuick 2.6                 // 2.6: Image.autoTransform (Qt 5.6, Sailfish 4+)
import QtQuick.Window 2.0          // Screen.pixelDensity (px per mm) for real-size spacing
import Sailfish.Silica 1.0
import Nemo.Policy 1.0             // volume keys (Resource.ScaleButton)
import Nemo.Configuration 1.0      // /apps/rawfish-ir/output: the system Settings page's values
import Nemo.KeepAlive 1.2          // keep the screen on during a stacked exposure
import Qt.labs.folderlistmodel 2.1 // the gallery: files of ~/Pictures/RAWfish-ir
import com.indraw 1.0

ApplicationWindow {
    id: app

    readonly property color uiTextColor: Theme.secondaryHighlightColor   // notch buttons
    readonly property color captionColor: Theme.highlightColor           // headers
    readonly property color deckTextColor: "white"                       // deck controls
    readonly property string uiVersion: "187"                             // shown in the deck

    // Where captures go and how they are named: dconf keys edited in the
    // system Settings app (Settings > Apps > RAWfish-ir), applied to the engine
    // at its next start. Every IndrawPreview item (the camera's, the cover's)
    // takes them through applyOutput(); the gallery reads them from the
    // camera's.
    ConfigurationGroup {
        id: outputConf
        path: "/apps/rawfish-ir/output"
        property string prefix: "photo"
        property string jpgDir: "~/Pictures/RAWfish-ir"
        property string dngDir: "~/Pictures/RAWfish-ir"
    }
    function applyOutput(item) { item.setOutput(outputConf.prefix, outputConf.jpgDir, outputConf.dngDir) }
    // Device tuning flags from the same Settings page: whether the camera menu
    // shows the focus calibration column, and a one-shot request to write the
    // device profile files (done here, since the Settings app cannot run the
    // engine), answered with the date.
    // Which optional columns the camera menu shows (Settings page).
    ConfigurationGroup {
        id: menuConf
        path: "/apps/rawfish-ir/menu"
        property bool previewColumns: false
        property string startMode: "last"            // last | simple | advanced
        property bool showScenes: false              // scenes in the camera menu
        property bool startManual: false             // start in manual focus + exposure
    }
    ConfigurationGroup {
        id: deviceConf
        path: "/apps/rawfish-ir/device"
        property bool focusCalibrationMenu: false
        property bool probeRequested: false
        property string probeDone: ""
        property bool shadingCalRequested: false   // Settings asked for the lens-shading assistant
    }

    // Cover (the app's tile on the home screen): the newest capture, or the
    // name when there is none, and the running build.
    cover: Component {
        CoverBackground {
            // The output folder as configured ([output] jpgDir): a viewfinder
            // item only to read the settings, never started.
            IndrawPreview { id: coverPaths; visible: false; Component.onCompleted: app.applyOutput(coverPaths) }
            FolderListModel {
                id: coverModel
                folder: "file://" + coverPaths.jpgDir
                nameFilters: [ "*_???.jpg", "*_???.jpeg" ]   /* JPEG captures (DNG previews are in .previews/) */
                showDirs: false
                sortField: FolderListModel.Time
                sortReversed: true
            }
            Image {
                anchors.fill: parent
                source: coverModel.count > 0 ? coverModel.get(0, "fileURL") : ""
                fillMode: Image.PreserveAspectCrop
                autoTransform: true
                sourceSize.width: 512
                asynchronous: true
                opacity: 0.85
            }
            Label {
                anchors { left: parent.left; bottom: parent.bottom; margins: Theme.paddingMedium }
                text: "RAWfish-ir"; color: Theme.primaryColor
                font { pixelSize: Theme.fontSizeMedium; bold: true }
                style: Text.Outline; styleColor: "black"
            }
            Label {
                anchors { right: parent.right; bottom: parent.bottom; margins: Theme.paddingMedium }
                text: "ui " + app.uiVersion; color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeTiny
            }
        }
    }

    // One page, as Jolla Camera: the gallery and the camera are the two items
    // of one horizontal strip (the camera page's switcher), not two pages.
    initialPage: cameraPage

    Component {
        id: cameraPage
        Page {
            id: page
            showNavigationIndicator: false           // the dots fought the notch buttons
            // The root page (the gallery is in the switcher, not a page before
            // this one): no page gesture back.
            backNavigation: false
            // Every orientation: the image and the touch mapping share one
            // transform (viewRotation / tapAt), the layout follows (deck
            // under the viewfinder in portrait, to its right in landscape).
            allowedOrientations: Orientation.All

            function screenRotation() {
                switch (page.orientation) {
                case Orientation.Portrait:          return 0
                case Orientation.Landscape:         return 90
                case Orientation.PortraitInverted:  return 180
                case Orientation.LandscapeInverted: return 270
                default:                            return 0
                }
            }
            function viewRotation() {
                return (viewfinder.sensorOrientation - screenRotation() + 360) % 360
            }
            property bool portrait: page.orientation === Orientation.Portrait
                                    || page.orientation === Orientation.PortraitInverted
            readonly property bool lefty: page.orientation === Orientation.LandscapeInverted

            property real notchHeight: Theme.itemSizeMedium / 2
            // The streamed frame's proportions (long side / short side): the
            // photo's, since the engine picks the preview to match.
            readonly property real frameAspect: viewfinder.frameSize.width > 0 && viewfinder.frameSize.height > 0
                ? Math.max(viewfinder.frameSize.width, viewfinder.frameSize.height) / Math.min(viewfinder.frameSize.width, viewfinder.frameSize.height)
                : 4 / 3
            // The deck's minimum height in portrait: its caption row, the side
            // buttons, and a column tall enough for the histogram (34 %) above
            // the shutter block (two button rows, the shutter, their spacing).
            readonly property real deckMm: Screen.pixelDensity > 0 ? Screen.pixelDensity : 16
            readonly property real deckMinNormal: Theme.fontSizeTiny + Theme.paddingSmall + Theme.itemSizeSmall / 2
                + (2 * Theme.itemSizeSmall + Theme.itemSizeLarge + 10 * deckMm + Theme.paddingMedium) / 0.66
            // A screen with no cutout to work around: the band's four words
            // (zebra, peak, level, AE) then lie over the top of the viewfinder,
            // spread evenly, and the viewfinder starts at the top of the screen.
            // The device profile says whether the screen has a notch (caps
            // notch: 1 / 0); for a phone it does not know (-1), a guess from the
            // screen's proportions: no notch up to 1.9:1 (the XA2's 16:9 has
            // none, the JP2's tall screen has one).
            readonly property int screenNotch: viewfinder.caps.notch !== undefined ? viewfinder.caps.notch : -1
            readonly property bool bandOverlay: portrait
                && (screenNotch === 0 || (screenNotch !== 1 && screenLong / screenShort < 1.9))
            readonly property real bandSpace: bandOverlay ? 0 : notchHeight
            // A compact deck, when the photo at full width leaves less room than
            // the normal deck needs (the XA2; the JP2 in 4:3 has the room and
            // keeps its deck): a shorter histogram, tighter button rows, smaller
            // left buttons, wheels shortened with the deck. The deck scale still
            // applies on top if even that does not fit.
            readonly property bool compactDeck: portrait
                && (screenLong - bandSpace - screenShort * frameAspect) < deckMinNormal
            readonly property real deckMinHeight: compactDeck
                ? Theme.fontSizeTiny + Theme.paddingSmall + Theme.itemSizeSmall / 2
                  + (2 * Theme.itemSizeSmall + Theme.itemSizeLarge + 5 * deckMm + Theme.paddingSmall) / 0.80
                : deckMinNormal
            // The deck's scale. Sailfish sizes its controls to the same PHYSICAL
            // size on every phone, so on a smaller screen (Xperia XA2) the deck
            // takes a larger share of it. The deck is scaled down uniformly
            // (text, buttons, spacing together) until the viewfinder gets its
            // full width at the photo's proportions, down to half its size;
            // only beyond that does the viewfinder shrink. Computed from the
            // screen's sides, so the same in portrait and landscape.
            readonly property real screenLong: Math.max(page.width, page.height)
            readonly property real screenShort: Math.min(page.width, page.height)
            readonly property real deckScale: Math.max(0.5, Math.min(1.0,
                (screenLong - bandSpace - screenShort * frameAspect) / deckMinHeight))

            // Does this camera give RAW at all? (The JP2's front and wide cameras
            // offer no RAW16 stream: raw=0,0 in the caps.) Without it the RAW
            // type and the Σ speeds (a stack is a DNG) are not offered, and a
            // RAW type chosen before is set aside until a camera with RAW comes
            // back (ui 169). Unknown (no caps yet): assumed available.
            readonly property bool rawAvailable: viewfinder.caps.rawW === undefined || viewfinder.caps.rawW > 0
            property string imageTypeSetAside: ""         // "raw" set aside by a camera without RAW
            onRawAvailableChanged: {
                if (!rawAvailable && imageType === "raw") { imageTypeSetAside = "raw"; imageType = "jpg" }
                else if (rawAvailable && imageTypeSetAside !== "") { imageType = imageTypeSetAside; imageTypeSetAside = "" }
            }
            // UI state. Internal values are plain keys; qsTr() only on display.
            property string imageType: "jpg"             // raw | jpg | jpg yuv
            // Starts in continuous focus (the "auto" label): the camera follows
            // the scene by itself until the user asks for more. ("auto" here is
            // Camera2's AUTO, one scan then hold: the "once" label.)
            property string focusMode: "cont"            // auto | cont | macro | manual
            function imageTypeLabel(t) {
                switch (t) {
                case "raw":     return qsTr("raw")
                case "jpg yuv": return qsTr("jpg yuv")
                default:        return qsTr("jpg")
                }
            }
            // Labels (UI only; the engine keeps its Camera2 names):
            //   "once"   = one scan where you tap, then hold (Camera2 AUTO)
            //   "auto"   = continuous focus by the HAL; a tap then only meters
            //              the light there (sun instead of circle)
            //   distance = manual, set with the volume keys
            function focusModeLabel(f) {
                switch (f) {
                case "cont":   return qsTr("auto")
                case "macro":  return qsTr("macro")
                case "manual": return page.focusDistanceLabel()
                default:       return qsTr("once")
                }
            }

            // Manual focus distances (metres) stepped by the volume buttons;
            // the last one is infinity. Sent live to the preview ("focus 0.5m").
            // Manual focus is stepped in DIOPTERS (1/m), the lens's own unit:
            // equal steps are equal lens moves, fine near infinity where metres
            // would jump. 0.05 D per press, 0 D = infinity, up to 10 D (0.10 m);
            // holding a volume key auto-repeats, so the whole range takes a few
            // seconds and a calibration point can be placed precisely.
            // The focus behaviour is the controller's (sailfish/ctl_focus.c,
            // ARCHITECTURE.md step 4): the page sends the choices ("fc ...") and
            // draws its state ("fcstate": viewfinder.focusState).
            readonly property var fcs: viewfinder.focusState
            readonly property real focusDiopters: fcs.diopters !== undefined ? fcs.diopters : 1.0
            readonly property int  circleAf: fcs.circle !== undefined ? fcs.circle : 0   // 1 searching, 4 sharp, 5 unsure, 6 failed, 7 sun
            readonly property bool focusSearching: !!fcs.searching
            readonly property bool lightRef: !!fcs.light
            function focusDistanceLabel() {
                if (focusDiopters <= 0) return "\u221e"
                var m = 1 / focusDiopters
                return (m >= 10 ? m.toFixed(1) : m.toFixed(2)) + qsTr(" m")
            }
            function stepFocus(dir) { viewfinder.sendCommand("fc step " + dir) }   // > 0 = farther
            // --- Geometry: the controller's (sailfish/ctl_view.c, step 5) ----
            // The page works in DISPLAY-window coordinates (0..1 across what the
            // viewfinder shows), undoing the view rotation; the controller keeps
            // the zoom / loupe / frame / display windows, sends the engine its
            // "loupe" crops, maps taps to the viewfinder frame, and reports what
            // to show ("vwstate": the item applies the view window itself) and
            // where the reference point is (refU, refV) for the circle.
            readonly property var vws: viewfinder.viewState
            property bool loupeGesture: false
            onLoupeGestureChanged: {
                viewfinder.sendCommand("vw gesture " + (loupeGesture ? "on" : "off"))
                // The exposure is held only while the finger moves (the loupe's
                // sweep must not flicker), not for the whole loupe (ui 126).
                if (exposure.fullAuto && !page.lightRef) viewfinder.sendCommand(loupeGesture ? "aelock on" : "aelock off")
            }
            // Display (item pixels) <-> the display window's own 0..1 axes,
            // through the view rotation.
            function unrotate(u, v) {
                var r = page.viewRotation()
                if (r === 90)  return { u: v, v: 1 - u }
                if (r === 180) return { u: 1 - u, v: 1 - v }
                if (r === 270) return { u: 1 - v, v: u }
                return { u: u, v: v }
            }
            function rotate(u0, v0) {
                var r = page.viewRotation()
                if (r === 90)  return { u: 1 - v0, v: u0 }
                if (r === 180) return { u: 1 - u0, v: 1 - v0 }
                if (r === 270) return { u: v0, v: 1 - u0 }
                return { u: u0, v: v0 }
            }
            // The circle, where the controller says the reference is.
            function placeCircle() {
                if (!vws.hasRef) return
                var p = rotate(vws.refU, vws.refV)
                meterCircle.x = p.u * viewfinderZone.width - meterCircle.width / 2
                meterCircle.y = p.v * viewfinderZone.height - meterCircle.height / 2
            }
            onVwsChanged: placeCircle()
            // A tap on the viewfinder: the reference point, in display-window
            // coordinates; the controller maps it and runs the focus / light
            // sequence, then reports the circle to draw.
            function tapAt(px, py) {
                meterCircle.opacity = 1.0
                var q = unrotate(px / viewfinderZone.width, py / viewfinderZone.height)
                viewfinder.sendCommand("vw tap " + q.u.toFixed(4) + " " + q.v.toFixed(4))
            }
            // Pans the loupe by a display move (pixels): a fraction of the display
            // window, through the view rotation.
            function panLoupe(dxPx, dyPx) {
                var q = unrotate(0.5 + dxPx / viewfinderZone.width, 0.5 + dyPx / viewfinderZone.height)
                viewfinder.sendCommand("vw pan " + (q.u - 0.5).toFixed(4) + " " + (q.v - 0.5).toFixed(4))
            }

            // Fine white balance (the small + / - around the shutter): the step
            // applied, and what it added shown for a moment in place of the
            // readout ("+5 % red/blue", "+5 % green").
            property bool wbKeptOffered: false          // the start-up banner, once per app run
            // "Start in manual focus and exposure" (Settings page): at the first
            // start of the engine in this app run, focus manual at the last
            // distance and both wheels pinned at the last ISO and speed. Waits a
            // moment so that, with nothing saved yet, the wheels are pinned
            // where the automatic exposure has settled. Advanced mode only (the
            // simple mode has no manual).
            Timer {
                id: startManualTimer
                interval: 1500
                onTriggered: {
                    if (!menuConf.startManual || !settingsMenu.advanced) return
                    var d = Number(viewfinder.setting("lastFocusD", -1))
                    var iso = Number(viewfinder.setting("lastIso", 0))
                    var ns = Number(viewfinder.setting("lastShutterNs", 0))
                    if (d >= 0) viewfinder.sendCommand("fc distance " + d.toFixed(3))
                    page.focusMode = "manual"
                    if (iso > 0) exposure.setIso(iso); else exposure.touchIso()
                    if (ns > 0) exposure.setShutterNs(ns); else exposure.touchShutter()
                }
            }
            // The last values, kept for that option: saved when the app goes to
            // the background or closes (not at every change of the live values).
            Connections {
                target: Qt.application
                onActiveChanged: if (!Qt.application.active && viewfinder.running) {
                    viewfinder.setSetting("lastFocusD", page.focusDiopters)
                    viewfinder.setSetting("lastIso", exposure.displayIso)
                    viewfinder.setSetting("lastShutterNs", exposure.displayShutterNs)
                }
            }
            property string fineShown: ""
            property string fineText: ""
            function fineWb(what, d) {
                if (what === "warmth") { settingsMenu.stepWarmth(d); page.fineText = (d > 0 ? "+" : "−") + (5 * settingsMenu.fineWarmthStep) + "% " + qsTr("warm") }
                else { settingsMenu.stepTint(d); page.fineText = (d > 0 ? "+" : "−") + settingsMenu.fineTintStep + "% " + qsTr("magenta") }
                page.fineShown = what
                fineTimer.restart()
            }
            function fineReset(what) {
                if (settingsMenu.wbFixed !== "") {             // a measured white: back to the automatic one
                    settingsMenu.clearWbFixed()
                    page.fineText = qsTr("auto"); page.fineShown = what
                } else {
                    if (what === "warmth") settingsMenu.resetWarmth(); else settingsMenu.resetTint()
                    page.fineText = qsTr("reset"); page.fineShown = what
                }
                fineTimer.restart()
            }
            // The white-balance pick: a 2.1 s press on a readout; the ring fills,
            // releasing early cancels; then the engine measures the white on the
            // spot (or the middle of the view) and keeps it. The measured zone is
            // drawn meanwhile (wbZone), until the measure is over.
            readonly property int pickHoldMs: 2100
            property bool pickMeasuring: false
            property real pickProgress: 0
            property double pickStartMs: 0
            function pickHoldStart() { page.pickStartMs = Date.now(); page.pickProgress = 0.001; pickHold.start() }
            function pickHoldStop() { pickHold.stop(); page.pickProgress = 0 }
            Timer {
                id: pickHold
                interval: 40; repeat: true
                onTriggered: {
                    page.pickProgress = Math.min(1, (Date.now() - page.pickStartMs) / page.pickHoldMs)
                    if (page.pickProgress >= 1) {
                        stop(); page.pickProgress = 0
                        page.pickMeasuring = true
                        viewfinder.sendCommand("wb pick")
                        page.fineText = qsTr("measuring…"); page.fineShown = "warmth"; fineTimer.restart()
                    }
                }
            }
            Connections {
                target: viewfinder
                onWbPicked: {
                    page.pickMeasuring = false
                    if (ok) { settingsMenu.setWbFixed(gains); page.fineText = qsTr("WB set") }
                    else page.fineText = why === "clipped" ? qsTr("too bright: no colour left")
                                       : why === "dark" ? qsTr("too dark to measure")
                                       : qsTr("could not measure")
                    page.fineShown = "warmth"
                    fineTimer.interval = 2500; fineTimer.restart()
                }
            }
            Timer { id: fineTimer; interval: 900; onTriggered: { page.fineShown = ""; interval = 900 } }
            // The tint as a Duv in thousandths: "+4" green side, "−4" magenta side.
            function duvLabel(duv) {
                var t = Math.round(duv * 1000)
                return (t > 0 ? "+" : "") + t + " " + qsTr("tint")
            }

            // The AE button (both orientations): automatic exposure, no circle;
            // a loupe being moved keeps the exposure still, as always.
            function backToAuto() {
                exposure.resetAuto()
                page.clearTap()
                if (page.loupeGesture) viewfinder.sendCommand("aelock on")
            }

            // Remove the reference point (the controller frees the exposure and
            // restates the focus mode; the circle goes).
            function clearTap() {
                meterCircle.opacity = 0
                viewfinder.sendCommand("fc clear")
            }

            // The focus button's mode, to the controller (which applies it to the
            // preview and drops the reference).
            function applyFocus() {
                if (!viewfinder.running) return
                viewfinder.sendCommand("fc mode " + (page.focusMode === "cont" ? "cont" : page.focusMode === "manual" ? "manual" : "once"))
            }
            onFocusModeChanged: { applyFocus(); meterCircle.opacity = 0 }
            Connections {
                target: viewfinder
                // A (re)started engine -- another camera, another size, back from
                // the background -- is a new session that knows no circle and no
                // loupe: the page starts clean too, so the two never disagree.
                onRunningChanged: if (viewfinder.running) {
                    meterCircle.opacity = 0                           // the controller starts clean too
                    if (page.loupeOpen) page.loupe = 1
                    page.applyZoom(); page.applyFocus()
                    viewfinder.sendCommand("fc scene " + (page.sceneOn ? "on" : "off"))
                    if (orientLoader.item && orientLoader.item.valid) viewfinder.sendCommand("orient " + orientLoader.item.degrees)
                    if (!page.wbKeptOffered) {
                        page.wbKeptOffered = true; wbKeptBanner.offer(); startManualTimer.start()
                        viewfinder.installExamples()          // the commented reference files, refreshed
                    }
                }
            }

            // Volume buttons: the camera resource class takes them from the
            // system volume control while the app is active. In distance focus
            // mode they step the distance (up = farther); otherwise nothing yet.
            Permissions {
                enabled: Qt.application.active
                autoRelease: true
                applicationClass: "camera"
                Resource { type: Resource.ScaleButton; optional: true }
            }
            focus: true
            // Both volume keys within 250 ms = screenshot (the SFOS convention,
            // which the system can no longer do once we hold the keys); it
            // overrides the single-press actions, which are delayed by that
            // window and dropped if the other key arrives.
            property bool volUpHeld: false
            property bool volDownHeld: false
            property int pendingStep: 0
            Timer {
                id: volumeTimer
                interval: 120                   // shorter: the keys may be spammed
                onTriggered: { if (page.pendingStep !== 0 && page.focusMode === "manual") page.stepFocus(page.pendingStep); page.pendingStep = 0 }
            }
            function volumePress(dir) {
                if (page.stacking) return              // locked during a stack
                var other = dir > 0 ? page.volDownHeld : page.volUpHeld
                if (dir > 0) page.volUpHeld = true; else page.volDownHeld = true
                if (other) { volumeTimer.stop(); page.pendingStep = 0; page.screenshot(); return }
                page.pendingStep = dir
                volumeTimer.restart()
            }
            function screenshot() {
                var d = new Date()
                function p2(n) { return (n < 10 ? "0" : "") + n }
                var name = "Screenshot_" + d.getFullYear() + p2(d.getMonth() + 1) + p2(d.getDate())
                           + "_" + p2(d.getHours()) + p2(d.getMinutes()) + p2(d.getSeconds()) + ".png"
                var path = StandardPaths.pictures + "/Screenshots/" + name
                // The app grabs its own window: lipstick's screenshot service
                // is unreachable from inside the Sailjail sandbox (D-Bus is
                // filtered; it answered ServiceUnknown), while ~/Pictures is
                // writable. The outcome goes to the engine log ("# ..." lines).
                var ok = viewfinder.saveScreenshot(path)
                if (ok) flashAnim.restart()                 // after the grab, so the flash is not in it
                viewfinder.sendCommand("# screenshot " + (ok ? "ok " : "failed ") + path)
            }
            // Holding a key auto-repeats the step (fast travel along the scale).
            Keys.onVolumeUpPressed:   { event.accepted = true; if (event.isAutoRepeat) { if (page.focusMode === "manual") page.stepFocus(1) } else page.volumePress(1) }
            Keys.onVolumeDownPressed: { event.accepted = true; if (event.isAutoRepeat) { if (page.focusMode === "manual") page.stepFocus(-1) } else page.volumePress(-1) }
            Keys.onReleased: {
                if (event.key === Qt.Key_VolumeUp) { event.accepted = true; page.volUpHeld = false }
                if (event.key === Qt.Key_VolumeDown) { event.accepted = true; page.volDownHeld = false }
            }

            // Exposure state machine (auto / priority / manual), see ExposureLogic.qml.
            ExposureLogic {
                id: exposure
                viewfinder: viewfinder
                imageType: page.imageType
                previewFps: settingsMenu.sigmaPreviewFps
                // No exposure change while the loupe moves without a reference
                // (the controller pauses it by itself during a focus search).
                paused: page.loupeGesture && !page.lightRef
            }

            // Standard 1/3-stop ISO and shutter notches, bounded by the sensor
            // capabilities the engine reports (caps); shutter values are
            // nanoseconds. Above the single-exposure ceiling the notches are
            // stacked exposures (Σ), up to 16 s photo-equivalent.
            readonly property var isoSteps: [25,32,40,50,64,80,100,125,160,200,250,320,400,500,640,800,
                                             1000,1250,1600,2000,2500,3200,4000,5000,6400,8000,10000,
                                             12800,16000,20000,25600,32000,40000,51200,64000,80000,102400]
            // Single exposures in 1/3 stops up to the ceiling (1/2 s), then the
            // stacked (Σ) positions: photo-equivalent 1 s .. 8 min, done by
            // adding sub-frames of the ceiling length (docs/UI-NOTES.md).
            readonly property var shutterSteps: [1/8000,1/6400,1/5000,1/4000,1/3200,1/2500,1/2000,1/1600,
                                                 1/1250,1/1000,1/800,1/640,1/500,1/400,1/320,1/250,1/200,
                                                 1/160,1/125,1/100,1/80,1/60,1/50,1/40,1/30,1/25,1/20,1/15,
                                                 1/13,1/10,1/8,1/6,1/5,1/4,0.3,0.4,0.5,0.6,0.8]
            // (0.6 and 0.8 s, ui 163: a phone whose HAL really delivers more
            // than 0.5 s -- the XA2's 0.61 s -- gets them; rebuildModels keeps
            // only the steps up to the phone's single-exposure ceiling.)
            readonly property var stackSteps: [1, 2, 4, 8, 15, 30, 60, 120, 240, 480]
            property var isoModel: isoSteps
            property var shutterModel: shutterSteps.map(function(s) { return Math.round(s * 1e9) })
                                         .concat(stackSteps.map(function(s) { return s * 1e9 }))
            function rebuildModels() {
                var c = viewfinder.caps
                if (!c || !(c.isoMin > 0)) return
                var iso = isoSteps.filter(function(v) { return v >= c.isoMin && v <= c.isoMax })
                if (iso.length === 0 || iso[0] !== c.isoMin) iso.unshift(c.isoMin)
                if (iso[iso.length - 1] !== c.isoMax) iso.push(c.isoMax)
                isoModel = iso
                var sh = shutterSteps.map(function(s) { return Math.round(s * 1e9) })
                            .filter(function(ns) { return ns >= c.expMinNs && ns <= exposure.shutterMaxNs })
                // the Σ speeds only where the camera gives RAW (a stack is a DNG)
                shutterModel = page.rawAvailable ? sh.concat(stackSteps.map(function(s) { return s * 1e9 })) : sh
            }
            Connections { target: viewfinder; onCapsChanged: page.rebuildModels() }

            // Labels. Shutter: "1/125", "0.5", "Σ 2" (stacked, longer than the
            // single-exposure ceiling). Any real value is formatted, so the
            // wheels can show the live intermediate exposure in auto.
            // "1/125", "0.5s", and for stacks "Σ2m (5:12)": the photo-equivalent
            // time, then in brackets the REAL time the shot will take (frames x
            // measured time per frame, with the denoise factor), since a stack
            // is sub-frames plus the HAL's latency between them. The Σ is drawn
            // 30 % smaller than the digits.
            function timeText(s) {
                if (s < 0.29) return "1/" + Math.round(1 / s)     // up to 1/4 as a fraction
                //: seconds on the shutter wheel; keep it short (about 4 characters)
                if (s < 60) return qsTr("%1s").arg(s < 10 ? (Math.round(s * 10) / 10) : Math.round(s))
                //: minutes on the shutter wheel; keep it short
                if (s < 3600) return qsTr("%1m").arg(Math.round(s / 60))
                //: hours on the shutter wheel; keep it short
                return qsTr("%1h").arg(Math.round(s / 3600))
            }
            function clockText(s) {
                var m = Math.floor(s / 60), r = Math.round(s - m * 60)
                return m + ":" + (r < 10 ? "0" : "") + r
            }
            // Real duration of a stack: frames x (sub-frame + HAL latency).
            function stackSeconds(ns) {
                var frames = Math.ceil(ns / exposure.shutterMaxNs) * settingsMenu.stackDenoise
                return frames * (exposure.shutterMaxNs / 1e9 + settingsMenu.frameLatencyMs / 1000)
            }
            // The wheel's three parts for a speed: the value ("2m"), the small
            // Σ before it for a stack, and under it the real shot time "(2:48)".
            function shutterLabel(ns) { return timeText(ns / 1e9) }
            function shutterPrefix(ns) { return exposure.isStacked(ns) ? "\u03a3" : "" }
            function shutterSub(ns) { return exposure.isStacked(ns) ? "(" + clockText(stackSeconds(ns)) + ")" : "" }
            function isoLabel(v) { return String(Math.round(v)) }
            function evLabel(steps) {
                var v = steps * (viewfinder.caps.evStep ? viewfinder.caps.evStep : 1/3)
                return (v > 0 ? "+" : "") + Number(v).toFixed(1)
            }
            // EV wheel notches: the HAL's compensation steps within its range.
            property var evModel: {
                var c = viewfinder.caps, m = []
                var lo = (c && c.evMin !== undefined) ? c.evMin : -6
                var hi = (c && c.evMax !== undefined) ? c.evMax : 6
                for (var i = lo; i <= hi; ++i) m.push(i)
                return m
            }
            // Digital zoom (crop region on the preview AND the photo -- the
            // JPEG; a RAW stays full-frame). Bounded by caps.zoomMax.
            property real zoomValue: 1.0
            readonly property real zoomMax: viewfinder.caps.zoomMax > 1 ? viewfinder.caps.zoomMax : 1
            function applyZoom() { if (viewfinder.running) viewfinder.sendCommand("vw zoom " + zoomValue.toFixed(2)) }
            onZoomValueChanged: applyZoom()
            // The zoom buttons (the centre + and - of the deck): a tap steps by
            // 0.25, a double tap within 0.15 s goes to the end (max / x1, see
            // zoomTap), and a HOLD zooms
            // progressively:
            //  - by proportion, not by addition: as long from x1 to x2 as from x2
            //    to x4 -- how a magnification is seen (as the focus moves in
            //    diopters, not metres);
            //  - slowly at first, to aim, faster the longer the hold: half a
            //    doubling per second at the start, two after 2 s. On the JP2
            //    (x4 = two doublings) the whole range takes ~1.8 s of zooming
            //    (after the ~0.4 s it takes the press to count as a hold).
            // A new value goes out every 66 ms at most (~15 per second): enough
            // for a smooth zoom, without flooding the camera with requests.
            readonly property real zoomTapStep: 0.25
            // A double tap (to the max with +, back to x1 with -) is told apart
            // here, with its own window of 0.15 s: the system's (~0.4 s) took
            // two deliberate taps for a double tap and jumped to the end. Two
            // taps further apart are two steps.
            readonly property int zoomDoubleTapMs: 150
            property double zoomLastTap: 0
            property int zoomLastDir: 0
            function zoomTap(dir) {
                var now = Date.now()
                if (dir === zoomLastDir && now - zoomLastTap <= zoomDoubleTapMs) {
                    zoomValue = dir > 0 ? zoomMax : 1.0              // a double tap: to the end
                    zoomLastTap = 0
                } else {
                    zoomValue = dir > 0 ? Math.min(zoomMax, zoomValue + zoomTapStep)
                                        : Math.max(1.0, zoomValue - zoomTapStep)
                    zoomLastTap = now
                }
                zoomLastDir = dir
            }
            property int zoomHoldDir: 0                 // +1 in, -1 out, 0 not held
            property double zoomHoldStart: 0
            property double zoomHoldLast: 0
            function zoomHoldBegin(dir) {
                zoomHoldDir = dir; zoomHoldStart = zoomHoldLast = Date.now(); zoomHoldTimer.start()
            }
            function zoomHoldEnd() { zoomHoldDir = 0; zoomHoldTimer.stop() }
            Timer {
                id: zoomHoldTimer
                interval: 66; repeat: true
                onTriggered: {
                    var now = Date.now(), t = (now - page.zoomHoldStart) / 1000, dt = (now - page.zoomHoldLast) / 1000
                    page.zoomHoldLast = now
                    var rate = 0.5 + Math.min(1.5, 0.75 * t)            // doublings per second
                    var z = page.zoomValue * Math.pow(2, page.zoomHoldDir * rate * dt)
                    z = Math.max(1.0, Math.min(page.zoomMax, z))
                    if (z !== page.zoomValue) page.zoomValue = z
                    if (z <= 1.0 || z >= page.zoomMax) page.zoomHoldEnd()   // at a stop
                }
            }
            // Focus loupe (unpinch): the preview only, never the photo (see
            // "Coordinates"). A red frame marks focus-check mode. While a finger
            // moves it the exposure does not change (unless a light reference
            // circle measures a spot: a tap in the loupe meters there, precisely).
            property real loupe: 1.0
            readonly property bool loupeOpen: page.loupe > 1.01
            onLoupeChanged: viewfinder.sendCommand("vw loupe " + page.loupe.toFixed(3))
            // Rule-of-thirds grid: the menu's Grid column owns it (persisted there).
            readonly property bool gridOn: settingsMenu.gridOn
            property bool levelOn: false                  // the level (notch button)
            // The accelerometer, optional (LevelSensor.qml): roll and pitch.
            Loader {
                id: levelLoader
                source: "LevelSensor.qml"
                onLoaded: item.active = Qt.binding(function() { return page.levelOn && Qt.application.active })
            }
            readonly property bool levelAvailable: levelLoader.status === Loader.Ready
            // A long press on "level" with the phone held upright records the
            // sensor's reading as the zero (ui 175).
            // The sensor runs only while the level is on: it is switched on first
            // and the reading taken 0.8 s later (the filter settled).
            function zeroLevel() {
                if (!levelAvailable) return
                levelOn = true
                levelZeroTimer.restart()
            }
            Timer {
                id: levelZeroTimer
                interval: 800
                onTriggered: {
                    var it = levelLoader.item
                    if (it && it.valid) { settingsMenu.setLevelZero(it.roll, it.pitch); page.fineText = qsTr("level zeroed here") }
                    else page.fineText = qsTr("level: no reading")
                    page.fineShown = "warmth"; fineTimer.interval = 2500; fineTimer.restart()
                }
            }
            // Diagnostics (ui 172): two seconds after the level is switched on,
            // one line in the log says whether QtSensors loaded and whether the
            // accelerometer delivers (the XA2 showed no level: this tells why).
            Timer {
                id: levelDiag
                interval: 2000
                onTriggered: {
                    var it = levelLoader.item
                    viewfinder.sendCommand("# level: loader " + (levelLoader.status === Loader.Ready ? "ready" : levelLoader.status === Loader.Error ? "ERROR" : "status " + levelLoader.status)
                                           + (it ? ", " + it.readings + " readings in 2 s, valid " + it.valid + ", roll " + it.roll.toFixed(1) + " pitch " + it.pitch.toFixed(1) : ", no sensor item"))
                }
            }
            onLevelOnChanged: if (levelOn) levelDiag.restart()
            // The phone's physical rotation (gravity), for the photos' orientation:
            // whatever the OS does with its screen, a photo comes out upright on
            // the ground. Sent to the engine when it changes; it reads it at each
            // shot. Optional (QtSensors): without it, the sensor mounting only.
            Loader {
                id: orientLoader
                source: "OrientationSensor.qml"
                onLoaded: item.active = Qt.binding(function() { return Qt.application.active })
            }
            Connections {
                target: orientLoader.item
                onDegreesChanged: if (viewfinder.running) viewfinder.sendCommand("orient " + orientLoader.item.degrees)
            }
            // A scene mode hands AE/AWB/AF to the HAL: the atomic controls grey
            // out -- except FACE_PRIORITY (1), which Camera2 defines as keeping
            // the AE/AWB/AF modes the app set.
            readonly property bool sceneOn: settingsMenu.showScenes && settingsMenu.sceneMode > 1
                                            && settingsMenu.sceneAvailable.indexOf(settingsMenu.sceneMode) >= 0
            onSceneOnChanged: { viewfinder.sendCommand("fc scene " + (sceneOn ? "on" : "off")); if (sceneOn) exposure.stopSpot() }
            // A stacked exposure (Σ) is a RAW-only feature: the TYPE button is
            // forced to raw and greyed while a Σ speed is selected (a JPEG of a
            // many-frame stack is not meaningful). Remember the type to restore.
            readonly property bool stackSelected: !exposure.fullAuto && exposure.isStacked(exposure.displayShutterNs)
            property string typeBeforeStack: "jpg"
            onStackSelectedChanged: {
                if (stackSelected) { if (imageType !== "raw") { typeBeforeStack = imageType; imageType = "raw" } }
                else if (imageType === "raw" && typeBeforeStack !== "raw") imageType = typeBeforeStack
            }
            // The HAL's camera ids, remembered from the first caps line (a
            // restart on another camera reports the same list).
            property var cameraIds: []
            Connections {
                target: viewfinder
                onCapsChanged: {
                    if (viewfinder.caps.cameras && viewfinder.caps.cameras.length > 0)
                        page.cameraIds = viewfinder.caps.cameras
                    page.applyAidThresholds()
                    // The caps line is on stdout, not in the engine's log: echo
                    // the parts worth having there (sizes, scenes, thresholds).
                    viewfinder.sendCommand("# caps jpeg_sizes=" + viewfinder.caps.jpegSizes
                                           + " scene_modes=" + viewfinder.caps.sceneModes
                                           + " peak_edge=" + viewfinder.caps.peakEdge + " peak_width=" + viewfinder.caps.peakWidth
                                           + " focal_mm=" + viewfinder.caps.focalMm + " focal_eq=" + viewfinder.caps.focalEq
                                           + " raw=" + viewfinder.caps.rawW + "x" + viewfinder.caps.rawH)
                }
                // Another camera lists other sizes: forget the chosen one, with
                // no restart of its own (the camera change restarts the engine
                // on "max" already); its caps then settle the format's size,
                // restarting only if "max" was not it. (Up to ui 156 setSize("")
                // restarted a second time: two engine starts per switch.)
                onCameraIdChanged: { settingsMenu.captureSize = ""; settingsMenu.save("captureSize", ""); viewfinder.captureSize = Qt.size(0, 0) }
            }
            // Zebra clips at a different level for raw and for jpg: the RAW has
            // its own latitude, so "about to clip" is nearer the white level.
            // The viewfinder is 8-bit, so the raw threshold (per-mille of white)
            // is mapped onto 0..255. Peaking uses the device's edge threshold.
            function applyAidThresholds() {
                var c = viewfinder.caps
                var pk = (1 + settingsMenu.peakTweak / 100), zb = (1 + settingsMenu.zebraTweak / 100)
                viewfinder.peakEdge = Math.round((c.peakEdge > 0 ? c.peakEdge : 40) * pk)
                viewfinder.peakWidth = c.peakWidth > 0 ? c.peakWidth : 2
                // Zebra: the tweak moves the headroom (255 - level), not the level.
                var level = page.imageType === "raw" ? (c.zebraRaw > 0 ? c.zebraRaw : 992) / 1000 * 255
                                                     : (c.zebraJpeg > 0 ? c.zebraJpeg : 250)
                viewfinder.zebraLevel = Math.round(255 - (255 - level) * zb)
            }
            onImageTypeChanged: applyAidThresholds()

            // Build and send the capture command from the exposure state: in
            // full auto the engine's AE (with the EV wheel's compensation)
            // exposes the photo; otherwise the pair the preview is showing.
            // The shutter: self-timer first (menu), then one capture, or the
            // bracketing burst (3 frames at -N, 0, +N stops, every frame a real
            // exposure around the current pair; in full auto the live pair is
            // the centre). Stacked speeds and RAW+bracket are sent as is.
            property int countdown: 0
            // During a stacked exposure the screen stays on (Jolla Camera does
            // this for video): the display is black with the progress, which
            // costs little on an OLED, and the user always sees the shot is
            // still running instead of a dark phone that might be touched.
            DisplayBlanking { preventBlanking: page.stacking }

            // A running stack: frames done / total, for the overlay.
            property int stackFrame: 0
            property int stackTotal: 0
            property int stackElapsedMs: 0
            readonly property bool stacking: viewfinder.stacking
            Timer {
                id: selfTimer
                interval: 1000; repeat: true
                onTriggered: { page.countdown--; if (page.countdown <= 0) { stop(); page.shoot() } }
            }
            function capture() {
                if (settingsMenu.timerSec > 0 && page.countdown === 0) {
                    page.countdown = settingsMenu.timerSec
                    selfTimer.restart()
                    return
                }
                page.shoot()
            }
            // A press of the shutter: the controller decides what it becomes
            // (sailfish/ctl_shoot.c, step 6) -- the held pair with the photo,
            // the manual distance, a stack for a Σ speed, a burst or three
            // shots for a bracket -- from the settings sent here.
            function shoot() {
                var isRaw = (page.imageType === "raw")
                viewfinder.sendCommand("sh name=" + viewfinder.capturePrefix
                                       + " type=" + (isRaw ? "raw" : "jpg")
                                       + " q=" + settingsMenu.effQuality
                                       + settingsMenu.photoProcessingFields()
                                       + " bracket=" + settingsMenu.bracket
                                       + " denoise_sigma=" + settingsMenu.stackDenoise)
            }

            // --- Gallery: the first item of the switcher strip (below), as in
            // Jolla Camera: the camera is the second; a left-to-right swipe
            // anywhere on the camera slides to the gallery, and from the
            // newest capture a right-to-left swipe slides back, all in one
            // continuous movement under the finger. The camera stays running
            // under the gallery so coming back is instant. The gallery's
            // dismiss button comes back through returnToCaptureMode(). ---
            function returnToCaptureMode() { switcher.currentIndex = 1 }
            // The switcher: [ gallery | camera ], Silica's PagedView, no wrap,
            // opening on the camera. Nested with the gallery's own PagedView, a
            // swipe past the newest capture carries on into the camera (and
            // back) in one gesture. Off while a horizontal drag means something
            // else on the camera (loupe pan, menu, a stack) or the gallery's
            // picture is zoomed / a video plays.
            PagedView {
                id: switcher
                anchors.fill: parent
                wrapMode: PagedView.NoWrap
                currentIndex: 1
                // A sideways drag must cover ~1 cm before the strip takes it:
                // a slanting downward swipe on the viewfinder reaches the menu's
                // ~0.5 cm first (swipeArea then keeps it), a flat one becomes
                // the strip's, which then follows the finger to the gallery.
                dragThreshold: Math.round(10 * page.deckMm)
                interactive: !page.loupeOpen && !page.stacking && !settingsMenu.open
                             && !(galleryLoader.item && galleryLoader.item.positionLocked)
                // Leaving the camera for the gallery: the gallery is put on the
                // newest capture before it shows (Jolla's _positionViewAtBeginning).
                onMovingChanged: if (moving && currentIndex === 1 && galleryLoader.item) galleryLoader.item.toNewest()
                model: VisualItemModel {
                    Item {
                        id: galleryItem
                        width: page.width; height: page.height
                        Rectangle { anchors.fill: parent; color: "black" }
                        // The gallery gets the camera's item (its folders) AT
                        // creation, not after: given later, the folder models
                        // were created on an empty folder and only filled at the
                        // next new file -- the gallery said "No captures yet"
                        // until a photo was taken (ui 142-145).
                        Loader {
                            id: galleryLoader
                            anchors.fill: parent
                            asynchronous: true
                            Component.onCompleted: setSource("GalleryView.qml", { viewfinder: viewfinder, cameraPage: page })
                        }
                    }
                    Item {
                        id: cameraContent
                        width: page.width; height: page.height
                        // black behind a viewfinder narrower than the screen
                        Rectangle { anchors.fill: parent; color: "black"; z: -1 }

            // --- Notch band: two buttons left of the cutout, two right. ---
            Rectangle {
                id: notchBand
                // Black above a notch; over the viewfinder (no notch), no
                // background at all: just the four words on the image.
                color: page.bandOverlay ? "transparent" : "black"
                z: page.bandOverlay ? 5 : 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: page.portrait ? page.notchHeight : 0
                // The empty centre for the cutout; none without a notch: the four
                // words then spread evenly over the width.
                property real notchGap: page.bandOverlay ? 0 : width * 0.40   // ui 164: 0.28 put peak and level at the JP2's notch edges

                Row {
                    anchors.left: parent.left
                    width: (parent.width - parent.notchGap) / 2
                    height: parent.height
                    visible: page.portrait
                    Repeater {
                        model: [ qsTr("zebra"), qsTr("peak") ]
                        Item {
                            width: parent.width / 2; height: parent.height
                            readonly property bool on: index === 0 ? viewfinder.zebra : viewfinder.peaking
                            Label {
                                anchors.centerIn: parent; text: modelData
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: aidTap.pressed ? Theme.highlightColor : app.uiTextColor
                                font.bold: parent.on              // bold = active
                                opacity: parent.on ? 1.0 : Theme.opacityHigh
                            }
                            MouseArea {
                                id: aidTap
                                anchors.fill: parent
                                onClicked: if (index === 0) viewfinder.zebra = !viewfinder.zebra
                                           else viewfinder.peaking = !viewfinder.peaking
                            }
                        }
                    }
                }
                Row {
                    anchors.right: parent.right
                    width: (parent.width - parent.notchGap) / 2
                    height: parent.height
                    visible: page.portrait
                    Repeater {
                        model: [ qsTr("level"), qsTr("AE") ]    // AE = reset full auto-exposure
                        Item {
                            width: parent.width / 2; height: parent.height
                            Label {
                                anchors.centerIn: parent; text: modelData
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: aeTap.pressed ? Theme.highlightColor : app.uiTextColor
                                font.bold: index === 0 && page.levelOn
                                // The AE button dims while everything is already auto;
                                // the level one when the device has no accelerometer.
                                opacity: (index === 1 && exposure.fullAuto && exposure.evSteps === 0)
                                         ? Theme.opacityLow
                                         : (index === 0 && (!page.levelOn || !page.levelAvailable) ? Theme.opacityHigh : 1.0)
                            }
                            MouseArea {
                                id: aeTap
                                anchors.fill: parent
                                onClicked: if (index === 1) { page.backToAuto() }
                                           else if (page.levelAvailable) page.levelOn = !page.levelOn
                                onPressAndHold: if (index === 0) page.zeroLevel()
                            }
                        }
                    }
                }
            }

            // --- Viewfinder zone (the photo's proportions). ---
            Rectangle {
                id: viewfinderZone
                color: "black"
                clip: true
                anchors.top: page.bandOverlay ? parent.top : notchBand.bottom
                anchors.horizontalCenter: page.portrait ? parent.horizontalCenter : undefined
                // Landscape: image on the left, deck on the right -- and the
                // other way round in inverted landscape (phone turned the other
                // way: left-handed use), since the deck belongs under the thumb.
                anchors.left: (page.portrait || page.lefty) ? undefined : parent.left
                anchors.right: page.lefty ? parent.right : undefined
                // The photo's proportions (the streamed frame's, 4:3 until the
                // first one), as large as the deck leaves room for: portrait,
                // full width unless the deck would get less than its minimum
                // height -- then narrower, centred, black on the sides (a 16:9
                // screen such as the Xperia XA2's); landscape, full height on the
                // left unless the deck would get less than its minimum width.
                // The deck is scaled first (deckScale, down to half), the image
                // shrinks only beyond that.
                readonly property real fullPortraitH: parent.width * page.frameAspect
                readonly property real roomPortraitH: parent.height - page.bandSpace - page.deckMinHeight * page.deckScale
                height: page.portrait ? Math.min(fullPortraitH, roomPortraitH)
                                      : Math.min(parent.height, (parent.width - page.deckMinHeight * page.deckScale) / page.frameAspect)
                width: page.portrait ? height / page.frameAspect : height * page.frameAspect

                IndrawPreview {
                    id: viewfinder
                    anchors.fill: parent
                    cameraId: "0"
                    previewSize: Qt.size(0, 0)        // engine picks (widest <= 1280)
                    captureSize: Qt.size(0, 0)        // engine picks the largest JPEG/RAW
                    fill: true                        // fill the full-width zone
                    // The camera's mounting angle, from the engine (caps); the
                    // back camera is 90, the front 270 on the JP2 (the hard-coded
                    // 90 left the front preview upside down).
                    sensorOrientation: viewfinder.caps.orientation !== undefined
                                       ? viewfinder.caps.orientation : 90
                    // Output settings before the first start, and whenever the
                    // Settings app changes them (effective at the next start).
                    // A pending "write the device profile" request is served
                    // first: probe is a short separate process.
                    Component.onCompleted: {
                        app.applyOutput(viewfinder)
                        if (deviceConf.probeRequested) {
                            var ok = viewfinder.runProbe()
                            deviceConf.probeRequested = false
                            deviceConf.probeDone = ok ? Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm") : qsTr("failed, see the log")
                        }
                    }
                    // The lens-shading assistant asked for in Settings: pushed
                    // over the camera once the engine runs (it drives the live
                    // engine). The flag is cleared at once so a crash or a
                    // back-swipe does not reopen it forever.
                    onRunningChanged: {
                        if (running && deviceConf.shadingCalRequested) {
                            deviceConf.shadingCalRequested = false
                            pageStack.push(Qt.resolvedUrl("ShadingAssistant.qml"), { viewfinder: viewfinder, cameraPage: page })
                        }
                    }
                    Connections {
                        target: outputConf
                        onPrefixChanged: app.applyOutput(viewfinder)
                        onJpgDirChanged: app.applyOutput(viewfinder)
                        onDngDirChanged: app.applyOutput(viewfinder)
                    }
                    // Histogram/meter source: the RAW16 meter for raw (linear,
                    // the latitude of the file), the viewfinder for jpg.
                    histogramSource: page.imageType === "raw" ? "raw" : "vf"
                    rotation: page.viewRotation()
                    displayRotation: page.viewRotation()   // so fill fits the rotated frame
                    // Running whenever the app is in front: also under the
                    // gallery, so coming back is instant.
                    active: Qt.application.active
                    // Flash the moment the engine says the exposure is done.
                    onCaptureFinished: flashAnim.restart()
                    // Stack progress: frame of n, elapsed; the overlay shows the
                    // time left from the measured per-frame time.
                    onStackStarted: { page.stackFrame = 0; page.stackTotal = n; page.stackElapsedMs = 0 }
                    onStackProgress: { page.stackFrame = frame; page.stackTotal = n; page.stackElapsedMs = elapsedMs }
                    onStackDone: {
                        page.stackFrame = 0; page.stackTotal = 0
                        // Remember the real time per frame for the wheel's estimate.
                        if (frames > 0 && ms > 0) {
                            var perFrame = ms / frames, sub = exposure.shutterMaxNs / 1e6
                            settingsMenu.frameLatencyMs = Math.max(0, Math.round(perFrame - sub))
                            settingsMenu.save("frameLatencyMs", settingsMenu.frameLatencyMs)
                        }
                    }
                    // The DNG's preview (NAME_vf.jpg) now comes from the engine's
                    // render of the DNG's own pixels (kind=preview, converted in
                    // C++); nothing to do here for "dng" any more.
                }

                // Capture flash: a white overlay flashed briefly on shutter.
                Rectangle {
                    id: captureFlash
                    anchors.fill: parent
                    color: "white"
                    opacity: 0
                    NumberAnimation {
                        id: flashAnim
                        target: captureFlash; property: "opacity"
                        from: 0.8; to: 0.0; duration: 220
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: !viewfinder.running
                    color: Theme.highlightColor
                    text: viewfinder.errorString.length > 0
                          ? viewfinder.errorString : qsTr("Starting camera…")
                }

                // Unpinch = focus loupe (display only, 1x..12x); pinch back to 1
                // releases it. The red frame says "you are judging focus, the
                // photo is not zoomed".
                Rectangle {
                    anchors.fill: parent
                    visible: page.loupe > 1.01
                    color: "transparent"
                    border { color: "#e03030"; width: Math.max(4, Math.round(4 * Theme.pixelRatio)) }
                    // A red diagonal says "loupe on" even where the frame is off-screen.
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.sqrt(parent.width * parent.width + parent.height * parent.height)
                        height: Math.max(2, Math.round(2 * Theme.pixelRatio))
                        rotation: -Math.atan2(parent.height, parent.width) * 180 / Math.PI
                        color: "#e03030"; opacity: 0.6
                    }
                    Label {
                        anchors { top: parent.top; right: parent.right; margins: Theme.paddingSmall }
                        text: qsTr("focus help ×%1").arg(page.loupe.toFixed(1))
                        color: "#e03030"; font.pixelSize: Theme.fontSizeSmall
                    }
                }

                // The level: a thin line on the true horizon. It turns with the
                // phone's roll and moves up/down with its tilt (pitch) by the
                // angle seen through the lens (from the equivalent focal
                // length), so it sits where the real horizon is; two fixed
                // marks at the centre are the reference. Green when the phone is
                // level within 1 degree: a cross, its horizontal arm and H value
                // yellow for the roll, its vertical arm and V value yellow for the
                // pitch, all green when both; white otherwise. A fixed small cross
                // at the centre is the reference.
                Item {
                    id: levelOverlay
                    anchors.fill: parent
                    visible: page.levelOn && levelLoader.item && levelLoader.item.valid
                    // The readings minus the recorded zero (settings; long press on "level").
                    readonly property real roll: levelLoader.item ? levelLoader.item.roll - settingsMenu.levelRoll0 : 0
                    readonly property real pitch: levelLoader.item ? levelLoader.item.pitch - settingsMenu.levelPitch0 : 0
                    // Roll relative to the displayed page (the UI rotates with the phone).
                    // Roll relative to the displayed page, folded to -90..90: a
                    // horizon line has no direction, so a phone turned 180 deg
                    // (or the other way round in landscape) is level too. Wrapped
                    // to -180..180 only, the three non-portrait orientations sat
                    // at +-180 when level and never turned green.
                    readonly property real screenRoll: {
                        var r = roll - page.screenRotation()
                        return r - 180 * Math.round(r / 180)
                    }
                    // Each axis has its own colour: the horizontal arm and the H
                    // value turn yellow when the roll is level, wherever the pitch
                    // puts the cross; the vertical arm and the V value when the
                    // pitch is straight; all green when both are. White otherwise.
                    readonly property bool rollOk: Math.abs(screenRoll) < 1.0
                    readonly property bool pitchOk: Math.abs(pitch) < 1.0
                    readonly property bool isLevel: rollOk && pitchOk
                    // Pixels per degree along the vertical of the view: its field of
                    // view from the 35 mm-equivalent focal length (36 mm side
                    // vertical in portrait, 24 mm in landscape).
                    readonly property real vfovDeg: {
                        var f = viewfinder.caps.focalEq > 0 ? viewfinder.caps.focalEq : 26
                        var side = page.portrait ? 36 : 24
                        return 2 * Math.atan(side / 2 / f) * 180 / Math.PI
                    }
                    readonly property real pxPerDeg: height / vfovDeg
                    readonly property color inkRoll: isLevel ? "#40ff60" : rollOk ? "#ffe040" : "white"
                    readonly property color inkPitch: isLevel ? "#40ff60" : pitchOk ? "#ffe040" : "white"
                    readonly property real lineW: Math.max(1, Math.round(Theme.pixelRatio))
                    // Arms longer than the screen whatever the rotation (the
                    // diagonal), so the cross always runs edge to edge (ui 174).
                    readonly property real armLen: Math.sqrt(width * width + height * height)
                    // The fixed reference: a small cross at the centre, discreet.
                    Rectangle { width: parent.width * 0.08; height: levelOverlay.lineW; x: parent.width / 2 - width / 2; y: parent.height / 2 - height / 2; color: "white"; opacity: 0.45 }
                    Rectangle { width: levelOverlay.lineW; height: parent.width * 0.08; x: parent.width / 2 - width / 2; y: parent.height / 2 - height / 2; color: "white"; opacity: 0.45 }
                    // The moving cross (ui 173): rotated by the roll, shifted by the
                    // pitch at the image's own scale; its arms run past the edges.
                    Item {
                        id: levelCross
                        width: levelOverlay.armLen * 2; height: width
                        x: parent.width / 2 - width / 2
                        y: parent.height / 2 - height / 2 + levelOverlay.pitch * levelOverlay.pxPerDeg
                        rotation: -levelOverlay.screenRoll
                        Rectangle {                            // horizontal arm (roll)
                            width: parent.width; height: levelOverlay.lineW
                            y: parent.height / 2 - height / 2
                            color: levelOverlay.inkRoll; opacity: 0.9
                        }
                        Rectangle {                            // vertical arm (pitch)
                            width: levelOverlay.lineW; height: parent.height
                            x: parent.width / 2 - width / 2
                            color: levelOverlay.inkPitch; opacity: 0.9
                        }
                    }
                    // The degrees at the bottom of the image (ui 174): "H x.x°"
                    // and "V x.x°", each in its axis's colour.
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Theme.paddingMedium
                        spacing: Theme.paddingLarge * 2
                        Label {
                            text: qsTr("H %1°").arg((levelOverlay.screenRoll >= 0 ? "" : "\u2212") + Math.abs(levelOverlay.screenRoll).toFixed(1))   // H: the roll
                            color: levelOverlay.inkRoll
                            font.pixelSize: Theme.fontSizeSmall
                            style: Text.Outline; styleColor: "#80000000"
                        }
                        Label {
                            text: qsTr("V %1°").arg((levelOverlay.pitch >= 0 ? "" : "\u2212") + Math.abs(levelOverlay.pitch).toFixed(1))   // V: the pitch
                            color: levelOverlay.inkPitch
                            font.pixelSize: Theme.fontSizeSmall
                            style: Text.Outline; styleColor: "#80000000"
                        }
                    }
                }

                // Rule-of-thirds grid (the menu's Grid column).
                Repeater {
                    model: page.gridOn ? 4 : 0
                    Rectangle {
                        readonly property bool vertical: index < 2
                        x: vertical ? parent.width * (index + 1) / 3 : 0
                        y: vertical ? 0 : parent.height * (index - 1) / 3
                        width: vertical ? 1 : parent.width
                        height: vertical ? parent.height : 1
                        color: "white"; opacity: 0.5
                    }
                }

                // Gallery opener: a left-to-right swipe INSIDE the image drags
                // the gallery in from the left (the system's edge swipes, from
                // outside the image, are untouched). A single finger moving
                // mostly horizontally for more than a thumb's width counts; a
                // tap or a vertical move is left to the other handlers.
                // Two fingers = the loupe (PinchArea); one finger = the swipe
                // (its child MouseArea): the standard nesting so both work.
                // Two fingers: spreading/closing = zoom, always about the centre
                // (the fingers' own drift is ignored while the distance changes);
                // moving together at a steady distance = pan. One gesture can
                // do both in turn: zoom first, then walk around.
                // Two fingers. A gesture is classified ONCE: if the spread changes
                // by more than 15 % it is a zoom (about the centre, the fingers'
                // drift ignored) until the fingers lift; otherwise, as soon as
                // the centre has moved a little, it is a pan. Deciding at every
                // update on a 1 % wobble made most pans look like zooms.
                PinchArea {
                    id: pinchArea
                    anchors.fill: parent
                    property bool active: false
                    property string mode: ""                 // "" | "zoom" | "pan"
                    property real startLoupe: 1.0
                    property real lastCx: 0
                    property real lastCy: 0
                    onPinchStarted: {
                        active = true; mode = ""; startLoupe = page.loupe
                        page.loupeGesture = true
                        lastCx = pinch.center.x; lastCy = pinch.center.y
                        swipeArea.cancelSwipe()              // a second finger ends any swipe
                    }
                    onPinchFinished: { active = false; mode = ""; page.loupeGesture = false }
                    onPinchUpdated: {
                        if (mode === "") {
                            var spread = Math.abs(pinch.scale - 1.0)
                            var moved = Math.abs(pinch.center.x - pinch.startCenter.x) + Math.abs(pinch.center.y - pinch.startCenter.y)
                            if (spread > 0.15) mode = "zoom"
                            else if (moved > Theme.paddingLarge && page.loupe > 1.01) mode = "pan"
                        }
                        if (mode === "zoom") {
                            page.loupe = Math.max(1.0, Math.min(12.0, startLoupe * pinch.scale))
                        } else if (mode === "pan") {
                            page.panLoupe(pinch.center.x - lastCx, pinch.center.y - lastCy)
                        }
                        lastCx = pinch.center.x; lastCy = pinch.center.y
                    }
                    // One finger: a tap = meter/focus; a downward drag pulls the
                    // settings menu down (anywhere in the image, so it never
                    // competes with the system's top-edge gesture); a sideways
                    // drag is not handled here -- the switcher takes it and
                    // brings the gallery in under the finger.
                    MouseArea {
                        id: swipeArea
                        anchors.fill: parent
                        // Who gets a drag: the gallery strip (the switcher) takes a
                        // sideways one only after ~1 cm (its dragThreshold); a drag
                        // going down ~0.5 cm, at least 3/4 as much down as sideways
                        // (~37 deg or steeper), is the menu, and from then on this
                        // area keeps it (preventStealing). In the loupe a drag is a
                        // pan and is always kept.
                        preventStealing: page.loupeOpen || dragging === "menu"
                        readonly property real menuMin: 5 * page.deckMm
                        property real startX: 0
                        property real startY: 0
                        property real lastX: 0
                        property real lastY: 0
                        property string dragging: ""          // "" | "menu" | "pinch" | "pan"
                        // A swipe interrupted (second finger, system): the menu goes
                        // back where it was, nothing is left half-way.
                        function cancelSwipe() {
                            if (dragging === "menu") settingsMenu.settle(settingsMenu.open)
                            dragging = "pinch"
                        }
                        onPressed: { startX = mouse.x; startY = mouse.y; dragging = "" }
                        onPositionChanged: {
                            var dx = mouse.x - startX, dy = mouse.y - startY
                            if (pinchArea.active) { cancelSwipe(); return }        // two fingers: never a swipe
                            if (page.loupe > 1.01) {
                                // Loupe on: one finger pans the window (the gallery
                                // and menu swipes are off, so the finger is free);
                                // a short touch without movement stays a tap.
                                if (dragging === "" && Math.abs(dx) + Math.abs(dy) > Theme.paddingLarge) { dragging = "pan"; lastX = mouse.x; lastY = mouse.y; page.loupeGesture = true }
                                if (dragging === "pan") { page.panLoupe(mouse.x - lastX, mouse.y - lastY); lastX = mouse.x; lastY = mouse.y }
                                return
                            }
                            if (dragging === "" && dy > menuMin && dy >= 0.75 * Math.abs(dx)) dragging = "menu"
                            if (dragging === "menu") settingsMenu.y = viewfinderZone.y + Math.max(-settingsMenu.height, Math.min(0, -settingsMenu.height + dy))
                        }
                        onReleased: {
                            if (dragging === "pan") page.loupeGesture = false
                            if (dragging === "pinch" || dragging === "pan") { dragging = ""; return }
                            if (dragging === "menu")
                                settingsMenu.settle(settingsMenu.y - viewfinderZone.y > -settingsMenu.height * 0.8)   // 20 % pulled: open
                            else if (Math.abs(mouse.x - startX) + Math.abs(mouse.y - startY) <= Theme.paddingLarge)
                                page.tapAt(mouse.x, mouse.y)       // a tap = meter/focus here
                            // (a drag that never said menu or gallery is nothing, not a tap)
                            dragging = ""
                        }
                        onCanceled: { if (dragging === "menu") settingsMenu.settle(settingsMenu.open); if (dragging === "pan") page.loupeGesture = false; dragging = "" }
                    }
                }

                // The zone a white-balance pick measures, while the readout is held
                // and until the measure is over: the reference circle's spot when one
                // is set, else the middle of what is shown -- both 12 % of the view
                // (the engine's box; a spot set before a later zoom is approximate).
                Rectangle {
                    id: wbZone
                    visible: page.pickProgress > 0 || page.pickMeasuring
                    width: Math.min(viewfinderZone.width, viewfinderZone.height) * 0.12 * 1.414
                    height: width; radius: width / 2
                    color: "transparent"
                    border.color: "#80d8ff"; border.width: 2
                    x: (page.vws.hasRef ? page.rotate(page.vws.refU, page.vws.refV).u : 0.5) * viewfinderZone.width - width / 2
                    y: (page.vws.hasRef ? page.rotate(page.vws.refU, page.vws.refV).v : 0.5) * viewfinderZone.height - height / 2
                }

                // Metering/focus circle: shown where the user tapped, for a
                // moment. In auto/semi-auto a tap meters AND focuses there; in
                // full manual exposure it focuses only (the engine is told so).
                // Grey bands for what the photo will cut. Since ui 144 the preview
                // has the photo's proportions and the viewfinder IS the photo: no
                // bands. Only if the camera offered no preview of that shape is
                // the photo's format (4:3 or 16:9) wider than the frame: then the
                // bands show the share cut along the display axis that is the
                // sensor's short side.
                readonly property real formatAspect: settingsMenu.format === "16:9" ? 16 / 9 : 4 / 3
                Repeater {
                    model: viewfinderZone.formatAspect > page.frameAspect * 1.02 ? 2 : 0
                    Rectangle {
                        readonly property real cut: (1 - page.frameAspect / viewfinderZone.formatAspect) / 2
                        color: "black"; opacity: 0.55
                        x: page.portrait ? (index === 0 ? 0 : parent.width * (1 - cut)) : 0
                        y: page.portrait ? 0 : (index === 0 ? 0 : parent.height * (1 - cut))
                        width: page.portrait ? parent.width * cut : parent.width
                        height: page.portrait ? parent.height : parent.height * cut
                    }
                }
                // A phone limited by its device profile (raw_stream / jpeg_max)
                // says so for six seconds at every start: a limited phone
                // is a phone still to understand, not a setting to forget.
                Label {
                    id: limitsNotice
                    anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingLarge * 3 }
                    visible: text !== "" && limitsTimer.running
                    text: {
                        var l = String(viewfinder.caps.limits || "")
                        if (l.indexOf("raw_off") >= 0 && l.indexOf("jpeg_max") >= 0) return qsTr("Device profile: no RAW, JPEG size limited")
                        if (l.indexOf("raw_off") >= 0) return qsTr("Device profile: RAW stream off")
                        if (l.indexOf("jpeg_max") >= 0) return qsTr("Device profile: JPEG size limited")
                        return ""
                    }
                    color: Theme.highlightColor; font.pixelSize: Theme.fontSizeSmall
                    style: Text.Outline; styleColor: "black"
                    onTextChanged: if (text !== "") limitsTimer.restart()
                    Timer { id: limitsTimer; interval: 6000 }
                }
                // Stacking is still being validated on the device: say so while
                // a Σ speed is selected (and not during the shot's black screen).
                Column {
                    anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.paddingLarge }
                    visible: page.stackSelected && !page.stacking
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Σ stacking: experimental feature")
                        color: Theme.highlightColor; font.pixelSize: Theme.fontSizeSmall
                        style: Text.Outline; styleColor: "black"
                    }
                    // The Σ preview at its limits (see the user guide, "Σ").
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: text !== ""
                        text: exposure.pvTooDark ? qsTr("too dark to measure: ISO and time at their limits")
                            : exposure.pvDeficit > 0.05 ? qsTr("preview %1 EV darker than the photo: histogram too dark, meter compensated").arg(exposure.pvDeficit.toFixed(1))
                            : exposure.pvSlow ? qsTr("ISO limit: preview slowed to %1 frames/s").arg(Math.max(1, Math.round(1000 / exposure.pvMs)))
                            : ""
                        color: exposure.pvTooDark ? "#ff6060" : exposure.pvDeficit > 0.05 ? "#ffb060" : Theme.secondaryHighlightColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        style: Text.Outline; styleColor: "black"
                    }
                }
                // At start, a white balance measured last time is applied for a
                // few seconds with a button to keep it; without a click it goes
                // and the automatic white balance is back (a forgotten measured
                // white would colour every later photo).
                Rectangle {
                    id: wbKeptBanner
                    property bool shown: false
                    property real remaining: 1.0               // the countdown, 1 -> 0
                    readonly property int countdownMs: 6000
                    anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: Theme.paddingLarge }
                    visible: shown && settingsMenu.wbFixed !== ""
                    width: wbKeptLabel.width + 2 * Theme.paddingLarge; height: wbKeptLabel.height + Theme.paddingLarge
                    radius: Theme.paddingSmall; color: Qt.rgba(0, 0, 0, 0.6)
                    border.color: "#80d8ff"; border.width: 1
                    Label {
                        id: wbKeptLabel
                        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: Theme.paddingSmall }
                        text: qsTr("Keep previous manual WB")
                        color: "#80d8ff"; font.pixelSize: Theme.fontSizeSmall
                    }
                    Rectangle {                                 // the bar empties as time runs out
                        anchors { left: parent.left; bottom: parent.bottom; margins: 2 }
                        height: Math.max(2, Math.round(Theme.paddingSmall / 2))
                        width: (parent.width - 4) * wbKeptBanner.remaining
                        color: "#80d8ff"
                    }
                    MouseArea { anchors.fill: parent; onClicked: { wbKeptTimer.stop(); wbKeptBanner.shown = false } }
                    Timer {
                        id: wbKeptTimer
                        interval: 50; repeat: true
                        property double startMs: 0
                        onTriggered: {
                            wbKeptBanner.remaining = Math.max(0, 1 - (Date.now() - startMs) / wbKeptBanner.countdownMs)
                            if (wbKeptBanner.remaining <= 0) { stop(); wbKeptBanner.shown = false; settingsMenu.clearWbFixed() }
                        }
                    }
                    function offer() {
                        if (settingsMenu.wbFixed === "" || !settingsMenu.advanced) return
                        remaining = 1.0; shown = true; wbKeptTimer.startMs = Date.now(); wbKeptTimer.start()
                    }
                }
                // Not done yet: the home JPEG from the raw YUV.
                Label {
                    anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.paddingLarge }
                    visible: page.imageType === "jpg yuv"
                    text: qsTr("jpg yuv: not yet implemented (a HAL JPEG is taken)")
                    color: Theme.highlightColor; font.pixelSize: Theme.fontSizeSmall
                    style: Text.Outline; styleColor: "black"
                }

                // A stack in progress: the preview is paused by the engine, so
                // show black (nothing stale, and an OLED saves power) with the
                // progress and an abort button; abort keeps the frames so far.
                Rectangle {
                    anchors.fill: parent
                    visible: page.stacking
                    color: "black"
                    MouseArea { anchors.fill: parent }       // nothing underneath reacts
                    Column {
                        anchors.centerIn: parent
                        spacing: Theme.paddingLarge
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "\u03a3 " + page.timeText(exposure.displayShutterNs / 1e9)
                            color: "white"; font.pixelSize: Theme.fontSizeLarge
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            readonly property real perFrameMs: page.stackFrame > 0 ? page.stackElapsedMs / page.stackFrame
                                                             : exposure.shutterMaxNs / 1e6 + settingsMenu.frameLatencyMs
                            readonly property real leftS: Math.max(0, (page.stackTotal - page.stackFrame) * perFrameMs / 1000)
                            text: page.stackTotal > 0
                                  ? qsTr("%1 / %2 frames · %3 left").arg(page.stackFrame).arg(page.stackTotal).arg(page.clockText(leftS))
                                  : qsTr("starting…")
                            color: Theme.secondaryHighlightColor; font.pixelSize: Theme.fontSizeMedium
                        }
                        Rectangle {                              // progress bar
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: viewfinderZone.width * 0.6; height: Math.max(2, Math.round(2 * Theme.pixelRatio))
                            color: Theme.lightPrimaryColor; opacity: Theme.opacityLow
                            Rectangle { height: parent.height; width: page.stackTotal > 0 ? parent.width * page.stackFrame / page.stackTotal : 0
                                        color: Theme.highlightColor }
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("abort — keep what is done")
                            color: Theme.highlightColor; font.pixelSize: Theme.fontSizeMedium
                            MouseArea { anchors.fill: parent; anchors.margins: -Theme.paddingLarge; onClicked: viewfinder.abortCapture() }
                        }
                    }
                }

                // Self-timer countdown.
                Label {
                    anchors.centerIn: parent
                    visible: page.countdown > 0
                    text: page.countdown
                    font { pixelSize: Theme.fontSizeHuge * 2; bold: true }
                    color: "white"; style: Text.Outline; styleColor: "black"
                }

                // Styled by the AF outcome: dotted while searching, dashed when
                // the lens locked without focus (impossible), solid when sharp.
                // The circle STAYS (the reference must not vanish before the
                // photo): full while the AF works, dimmer once settled; a tap on
                // it removes it and hands the regions back to the HAL's default
                // (centre); the AE button removes it too.
                FocusCircle {
                    id: meterCircle
                    // What the controller says of its search: 1 searching, 4 sharp,
                    // 5 locked but unsure, 6 failed, 7 a light-only reference (sun).
                    afState: page.circleAf
                    sun: page.circleAf === 7
                    visible: opacity > 0
                    opacity: 0
                    Behavior on opacity { NumberAnimation { duration: 300 } }
                    readonly property bool settled: afState === 2 || afState === 4 || afState === 5 || afState === 6
                    onSettledChanged: if (opacity > 0) opacity = settled ? 0.6 : 1.0
                    MouseArea { anchors.fill: parent; onClicked: page.clearTap() }
                    // Manual or semi-automatic exposure (a wheel touched): the light
                    // at the circle as an absolute EV (at ISO 100), above it -- a spot
                    // light meter, the way the distance shows below it in manual focus.
                    Label {
                        anchors { bottom: parent.top; horizontalCenter: parent.horizontalCenter }
                        visible: !exposure.fullAuto && !isNaN(exposure.meteredEv100)
                        text: "EV " + exposure.meteredEv100.toFixed(1)
                        color: "white"; font.pixelSize: Theme.fontSizeSmall
                        style: Text.Outline; styleColor: "black"
                    }
                    // Manual focus: the distance, under the circle.
                    Label {
                        anchors { top: parent.bottom; horizontalCenter: parent.horizontalCenter }
                        visible: page.focusMode === "manual"
                        text: page.focusSearching ? "…" : page.focusDistanceLabel()
                        color: "white"; font.pixelSize: Theme.fontSizeSmall
                        style: Text.Outline; styleColor: "black"
                    }
                }
            }

            // --- Swipe-down settings menu, over the viewfinder. ---
            SettingsMenu {
                id: settingsMenu
                viewfinder: viewfinder
                focusCalibrationMenu: deviceConf.focusCalibrationMenu
                previewColumns: menuConf.previewColumns
                startMode: menuConf.startMode
                showScenes: menuConf.showScenes
                imageType: page.imageType
                onPeakTweakChanged: page.applyAidThresholds()
                onZebraTweakChanged: page.applyAidThresholds()
                // Simple mode is JPEG only.
                // Simple mode is JPEG only, and has no wheels: a priority or
                // manual exposure left over would be invisible, so back to auto.
                onAdvancedChanged: if (!advanced) {
                    if (page.imageType !== "jpg") page.imageType = "jpg"
                    exposure.resetAuto()
                }
                // Resolution / format: the JPEG size is an argument of the
                // engine; the viewfinder restarts it with the chosen one.
                function applyCaptureSize(wxh, restart) {
                    if (wxh === "") viewfinder.captureSize = Qt.size(0, 0)
                    else { var p = wxh.split("x"); viewfinder.captureSize = Qt.size(Number(p[0]), Number(p[1])) }
                    if (restart) viewfinder.restart()
                }
                onCaptureSizeChosen: applyCaptureSize(wxh, true)
                Component.onCompleted: {
                    load()
                    applyCaptureSize(captureSize, false)  // the saved size, before the first start
                }
                // Over the viewfinder (same geometry), pulled down from inside
                // the image; tap outside the columns closes it (closeRequested).
                width: viewfinderZone.width
                height: viewfinderZone.height
                x: viewfinderZone.x
                property bool open: false
                y: viewfinderZone.y - height
                // settle(): the ONE way the menu is put at rest, open or closed.
                // Setting `open` alone did nothing when it did not change (a
                // cancelled drag on a closed menu left it half-way, stuck).
                function settle(want) { open = want; y = want ? viewfinderZone.y : viewfinderZone.y - height }
                onOpenChanged: y = open ? viewfinderZone.y : viewfinderZone.y - height
                z: 6
                visible: y > viewfinderZone.y - height
                Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
                onCloseRequested: settle(false)
            }

            // --- Control deck (portrait): caption header + 4-column grid. ---
            Rectangle {
                id: deck
                color: "black"                   // black deck background
                // Locked while a stack runs: a stray touch must not change a
                // wheel or fire the shutter (the abort is on the viewfinder).
                enabled: !page.stacking
                opacity: page.stacking ? Theme.opacityLow : 1.0
                // Laid out at its natural size (the room it gets divided by the
                // scale), then scaled from its top-left corner: everything in it
                // shrinks together, touch areas included.
                readonly property real roomX: page.portrait ? 0 : (page.lefty ? 0 : viewfinderZone.x + viewfinderZone.width)
                readonly property real roomY: page.portrait ? viewfinderZone.y + viewfinderZone.height : 0
                readonly property real roomW: page.portrait ? parent.width : parent.width - viewfinderZone.width
                readonly property real roomH: parent.height - roomY
                x: roomX; y: roomY
                width: roomW / page.deckScale; height: roomH / page.deckScale
                scale: page.deckScale
                transformOrigin: Item.TopLeft

                // Column proportions, RAWfish: 20 / 41 / 19 / rest of usable width.
                readonly property real sideMargin: Math.max(Theme.paddingMedium, width * 0.015)
                readonly property real columnGap: Math.max(1, Math.round(Theme.pixelRatio))
                readonly property real usableWidth: width - 2 * sideMargin - 3 * columnGap
                // The centre column a little wider than RAWfish's (41 %): the
                // fine white-balance buttons and readouts sit either side of
                // the shutter; the wheels and the left buttons each give a bit.
                readonly property real leftW:   Math.round(usableWidth * 0.18)
                readonly property real centerW: Math.round(usableWidth * 0.46)
                readonly property real speedW:  Math.round(usableWidth * 0.18)
                readonly property real isoW:    usableWidth - leftW - centerW - speedW

                // Tiny UI-version badge (so the running build is identifiable),
                // kept clear of the screen's rounded corner (ui 177: was 2 px
                // from the edge, cut by the JP2's corner).
                Label {
                    anchors { right: parent.right; bottom: parent.bottom; bottomMargin: 2
                              rightMargin: Math.round(2.5 * page.deckMm) }
                    text: "ui " + app.uiVersion
                    font.pixelSize: Theme.fontSizeTiny
                    color: app.uiTextColor
                    opacity: 0.6
                    z: 10
                }

                // Landscape: no notch band, so the four text buttons (zebra,
                // peak, grid, AE) sit in a row at the top of the deck.
                Row {
                    id: sideButtons
                    visible: !page.portrait
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    height: visible ? Theme.itemSizeSmall / 2 : 0
                    Repeater {
                        model: [ qsTr("zebra"), qsTr("peak"), qsTr("level"), qsTr("AE") ]
                        Item {
                            width: sideButtons.width / 4; height: sideButtons.height
                            readonly property bool on: index === 0 ? viewfinder.zebra
                                                     : index === 1 ? viewfinder.peaking
                                                     : index === 2 ? page.levelOn : false
                            Label {
                                anchors.centerIn: parent; text: modelData
                                font.pixelSize: Theme.fontSizeExtraSmall; font.bold: parent.on
                                color: app.uiTextColor
                                opacity: parent.on ? 1.0 : Theme.opacityHigh
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    if (index === 0) viewfinder.zebra = !viewfinder.zebra
                                    else if (index === 1) viewfinder.peaking = !viewfinder.peaking
                                    else if (index === 2) { if (page.levelAvailable) page.levelOn = !page.levelOn }
                                    else page.backToAuto()          // same as in portrait
                                }
                                onPressAndHold: if (index === 2) page.zeroLevel()
                            }
                        }
                    }
                }

                // Caption header row (CAM / EV / SPEED / ISO).
                Row {
                    id: header
                    visible: !settingsMenu.simpleHides("headers")
                    anchors { left: parent.left; right: parent.right; top: sideButtons.bottom
                              leftMargin: deck.sideMargin; rightMargin: deck.sideMargin
                              topMargin: Theme.paddingSmall }
                    height: Theme.fontSizeTiny
                    spacing: deck.columnGap
                    Repeater {
                        model: [ { t: qsTr("CAM"), w: deck.leftW },
                                 { t: qsTr("EV"),  w: deck.centerW },
                                 { t: qsTr("SPEED"), w: deck.speedW },
                                 { t: qsTr("ISO"), w: deck.isoW } ]
                        Label {
                            width: modelData.w; height: parent.height
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: app.captionColor; opacity: Theme.opacityHigh
                            font { pixelSize: Theme.fontSizeTiny; bold: true }
                            text: modelData.t
                        }
                    }
                }

                // The four-column grid below the header.
                Row {
                    anchors { left: parent.left; right: parent.right
                              top: header.bottom; bottom: parent.bottom
                              leftMargin: deck.sideMargin; rightMargin: deck.sideMargin }
                    spacing: deck.columnGap

                    // Column 1: focal / type / focus, each a third of the height;
                    // in simple mode (no TYPE) focal and focus take a half each,
                    // so they stay evenly spread and centred.
                    Column {
                        width: deck.leftW
                        height: parent.height
                        // ui 166: the three buttons start lower (8 % of the column
                        // free at the top) and share the rest down to the bottom.
                        topPadding: Math.round(parent.height * 0.08)
                        CycleButton {
                            width: parent.width; height: (parent.height - parent.topPadding) / (settingsMenu.simpleHides("type") ? 2 : 3)
                            compact: page.compactDeck
                            caption: qsTr("FOCAL")
                            // The 35 mm-equivalent focal length of the current camera
                            // (caps), its id until the engine has reported it.
                            // 35 mm-equivalent focal length, marked "(=35)"
                            // so it is clear it is the equivalent, not the real
                            // focal (which is ~5 mm on a phone).
                            value: viewfinder.caps.focalEq > 0
                                   ? qsTr("%1mm").arg(Math.round(viewfinder.caps.focalEq))
                                   : qsTr("cam %1").arg(viewfinder.cameraId)
                            subCaption: viewfinder.caps.focalEq > 0 ? qsTr("(=35)") : ""
                            onCycle: {
                                // Cycle through the ids the HAL lists (caps), else 0/1/2.
                                var ids = page.cameraIds.length > 0 ? page.cameraIds : ["0","1","2"]
                                viewfinder.cameraId = ids[(ids.indexOf(viewfinder.cameraId)+1) % ids.length]
                            }
                        }
                        CycleButton {
                            width: parent.width; height: (parent.height - parent.topPadding) / 3
                            compact: page.compactDeck
                            caption: qsTr("TYPE"); value: page.imageTypeLabel(page.imageType)
                            visible: !settingsMenu.simpleHides("type")   // simple mode: JPEG only
                            // Greyed on a Σ speed (stacking writes a DNG only) and on a
                            // camera without RAW (nothing to cycle to).
                            enabled: !page.stackSelected && page.rawAvailable
                            opacity: page.stackSelected || !page.rawAvailable ? Theme.opacityLow : 1.0
                            onCycle: {
                                var t = page.rawAvailable ? ["raw", "jpg"] : ["jpg"]   // "jpg yuv" when it exists
                                page.imageType = t[(t.indexOf(page.imageType)+1) % t.length]
                            }
                        }
                        CycleButton {
                            width: parent.width; height: (parent.height - parent.topPadding) / (settingsMenu.simpleHides("type") ? 2 : 3)
                            compact: page.compactDeck
                            caption: qsTr("FOCUS")
                            enabled: !page.sceneOn; opacity: page.sceneOn ? Theme.opacityLow : 1.0
                            // Manual focus shows the distance; the others a short label.
                            value: page.focusModeLabel(page.focusMode)
                            onCycle: {
                                var f = ["auto", "cont", "manual"]    // macro: engine only
                                page.focusMode = f[(f.indexOf(page.focusMode)+1) % f.length]
                            }
                        }
                    }

                    // Column 2: histogram (top third) + shutter (centred below).
                    Item {
                        width: deck.centerW
                        height: parent.height

                        // RGB histogram: 64 bins per colour from the chosen source
                        // (viewfinder.histR/G/B), with a clipping bar at each
                        // side whose height is the share of clipped pixels
                        // (left: at black, right: at white).
                        Canvas {
                            id: histogram
                            y: Math.round(2 * page.deckMm)          // ui 166: a little lower
                            width: parent.width
                            // 44 % of the column in the normal deck (was 34 %), 26 % in the
                            // compact one (was 20 %: the JP2 in 4:3 is compact on the phone)
                            // -- never so much that the + / shutter / - block lacks room.
                            readonly property real blockMin: 2 * Theme.itemSizeSmall + Theme.itemSizeLarge
                                                             + (page.compactDeck ? 3 : 6) * page.deckMm
                            height: Math.max(0, Math.min(parent.height * (page.compactDeck ? 0.26 : 0.44),
                                                         parent.height - y - blockMin - 3 * page.deckMm))
                            opacity: settingsMenu.simpleHides("histogram") ? 0.0 : 0.85
                            readonly property real barW: Math.max(2, Math.round(Theme.paddingSmall / 2))
                            function drawChannel(ctx, values, color, x0, w, h) {
                                if (!values || values.length === 0) return
                                ctx.beginPath()
                                ctx.moveTo(x0, h)
                                for (var i = 0; i < values.length; ++i)
                                    ctx.lineTo(x0 + i * w / (values.length - 1), h - values[i] / 1000 * h)
                                ctx.lineTo(x0 + w, h)
                                ctx.closePath()
                                ctx.fillStyle = color
                                ctx.fill()
                            }
                            function strokeChannel(ctx, values, color, x0, w, h) {
                                if (!values || values.length === 0) return
                                ctx.beginPath()
                                for (var i = 0; i < values.length; ++i) {
                                    var px = x0 + i * w / (values.length - 1), py = h - values[i] / 1000 * h
                                    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                                }
                                ctx.lineWidth = 2
                                ctx.strokeStyle = color
                                ctx.stroke()
                            }
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                var x0 = barW + 1, w = width - 2 * (barW + 1)
                                ctx.strokeStyle = Qt.rgba(1,1,1,0.15)
                                ctx.beginPath(); ctx.moveTo(x0, height-1); ctx.lineTo(x0 + w, height-1); ctx.stroke()
                                drawChannel(ctx, viewfinder.histR, Qt.rgba(1.0, 0.2, 0.15, 0.32), x0, w, height)
                                drawChannel(ctx, viewfinder.histG, Qt.rgba(0.2, 1.0, 0.35, 0.28), x0, w, height)
                                drawChannel(ctx, viewfinder.histB, Qt.rgba(0.25, 0.55, 1.0, 0.32), x0, w, height)
                                // The spot's own tones (a light reference set), brighter and
                                // outlined over the whole picture's: where the circle sits.
                                if (viewfinder.spotHistG.length > 0) {
                                    strokeChannel(ctx, viewfinder.spotHistR, Qt.rgba(1.0, 0.45, 0.4, 0.95), x0, w, height)
                                    strokeChannel(ctx, viewfinder.spotHistG, Qt.rgba(0.5, 1.0, 0.6, 0.95), x0, w, height)
                                    strokeChannel(ctx, viewfinder.spotHistB, Qt.rgba(0.55, 0.75, 1.0, 0.95), x0, w, height)
                                }
                                // Clipping bars: full height = 10% of the pixels clipped.
                                var lo = Math.min(1, viewfinder.histClipLow / 10) * height
                                var hi = Math.min(1, viewfinder.histClipHigh / 10) * height
                                ctx.fillStyle = Qt.rgba(0.4, 0.6, 1.0, 0.9)
                                ctx.fillRect(0, height - lo, barW, lo)
                                ctx.fillStyle = Qt.rgba(1.0, 0.3, 0.3, 0.9)
                                ctx.fillRect(width - barW, height - hi, barW, hi)
                            }
                            Connections {
                                target: viewfinder
                                onHistogramChanged: histogram.requestPaint()
                            }
                        }

                        // EV wheel overlaid on the histogram band (auto and
                        // priority modes): the HAL's compensation in full
                        // auto, the loop target's shift in priority modes.
                        ValueWheel {
                            id: evWheel
                            // Hidden with the wheels: the brightness slider is the same control.
                            visible: !exposure.fullManual && !settingsMenu.simpleHides("wheels")
                            enabled: !page.sceneOn; opacity: page.sceneOn ? Theme.opacityLow : 1.0
                            anchors.left: parent.left
                            anchors.right: parent.right
                            y: histogram.y                          // over the histogram
                            height: histogram.height
                            model: page.evModel
                            currentValue: exposure.evSteps
                            valueLabel: page.evLabel
                            orientationHorizontal: true
                            onValueSelected: exposure.setEv(value)
                        }
                        // Full manual: the EV wheel is replaced by a number, the
                        // measured offset in stops from what auto would choose.
                        Label {
                            anchors.centerIn: histogram
                            visible: exposure.fullManual
                            color: app.deckTextColor
                            font { pixelSize: Theme.fontSizeLarge; bold: true }
                            text: exposure.manualOffsetValid
                                  ? (exposure.manualOffsetStops > 0 ? "+" : "")
                                    + exposure.manualOffsetStops.toFixed(1)
                                  : "\u2014"
                        }

                        // Shutter (Jolla system icon) with zoom + above and zoom -
                        // below. Advanced: below the histogram band, pushed down
                        // 4 mm (real size) so the shutter sits under the thumb.
                        // Simple (no histogram, no EV wheel): the three are
                        // centred in the whole column, with more air between.
                        Column {
                            id: shutterBlock
                            anchors.horizontalCenter: parent.horizontalCenter
                            readonly property real mm: Screen.pixelDensity > 0 ? Screen.pixelDensity : 16
                            readonly property bool simple: settingsMenu.simpleHides("histogram")
                            readonly property real contentH: 2 * Theme.itemSizeSmall + Theme.itemSizeLarge + 2 * spacing
                            y: simple ? (parent.height - contentH) / 2 : histogram.y + histogram.height + (page.compactDeck ? 1.5 : 3) * mm
                            width: parent.width                  // the three + / - spread across the column
                            height: contentH
                            // Normal deck: the block spreads over the height left under
                            // the histogram (ui 164), down to 2 mm from the bottom.
                            // The block spreads over the height left under the histogram,
                            // down to ~2 mm from the bottom, in both decks (ui 165).
                            spacing: simple ? 6 * mm
                                   : Math.max((page.compactDeck ? 1.5 : 3) * mm,
                                              (parent.height - histogram.y - histogram.height - (page.compactDeck ? 2 : 3.5) * mm
                                               - 2 * Theme.itemSizeSmall - Theme.itemSizeLarge) / 2)

                            // Three "+" spread across the column: warmth, zoom, tint;
                            // three "−" below the shutter likewise. A tap shows for a
                            // moment what it added; the readouts either side of the
                            // shutter give the white balance in force, in real terms
                            // (kelvin, and the tint as Duv x 1000), estimated by the
                            // engine from the camera's colour gains.
                            Row {
                                width: parent.width; height: Theme.itemSizeSmall
                                Repeater {
                                    model: 3
                                    MouseArea {
                                        width: parent.width / 3; height: parent.height
                                        enabled: index === 1 || settingsMenu.advanced
                                        opacity: enabled ? 1.0 : 0.0
                                        // (no onDoubleClicked: with it connected, Qt swallows the
                                        // second tap of a pair; zoomTap tells a double tap itself)
                                        onClicked: index === 0 ? page.fineWb("warmth", +1) : index === 2 ? page.fineWb("tint", +1)
                                                   : page.zoomTap(+1)
                                        // held: a progressive zoom in (see zoomHoldBegin)
                                        onPressAndHold: if (index === 1) page.zoomHoldBegin(+1)
                                        onReleased: if (index === 1) page.zoomHoldEnd()
                                        onCanceled: if (index === 1) page.zoomHoldEnd()
                                        // the colour says what it does: warm, neutral (zoom), magenta
                                        Label { anchors.centerIn: parent; text: "+"
                                                font.pixelSize: Theme.fontSizeLarge
                                                color: index === 0 ? "#ffb060" : index === 2 ? "#ff70d0" : app.deckTextColor }
                                    }
                                }
                            }

                            Item {
                                width: parent.width; height: Theme.itemSizeLarge
                                // left: the colour temperature; right: the tint
                                // A double tap on a readout resets its shift (back to the AWB);
                                // a 2.1 s press measures the white on the spot / the middle and
                                // keeps it (a ring fills meanwhile; release early to cancel).
                                // The readouts sit between their own + and - (the
                                // warmth's in the left third, the tint's in the right
                                // third), no longer against the shutter.
                                Label {
                                    id: cctReadout
                                    x: parent.width / 6 - width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: settingsMenu.advanced
                                    horizontalAlignment: Text.AlignHCenter
                                    text: page.fineShown === "warmth" ? page.fineText
                                        : (viewfinder.wbCct > 0 ? viewfinder.wbCct + "K" : "")
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: settingsMenu.wbFixed !== "" ? "#80d8ff"                  // a measured white
                                         : settingsMenu.wbShift !== 0 ? "#ffb060" : app.deckTextColor   // tinted while shifted
                                    MouseArea { anchors.fill: parent; anchors.margins: -Theme.paddingSmall
                                                onDoubleClicked: page.fineReset("warmth")
                                                onPressed: page.pickHoldStart(); onReleased: page.pickHoldStop(); onCanceled: page.pickHoldStop() }
                                }
                                Label {
                                    id: tintReadout
                                    x: parent.width * 5 / 6 - width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    horizontalAlignment: Text.AlignHCenter
                                    visible: settingsMenu.advanced
                                    text: page.fineShown === "tint" ? page.fineText
                                        : (viewfinder.wbCct > 0 ? page.duvLabel(viewfinder.wbDuv) : "")
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: settingsMenu.wbFixed !== "" ? "#80d8ff"
                                         : settingsMenu.tint !== 0 ? "#ff70d0" : app.deckTextColor
                                    MouseArea { anchors.fill: parent; anchors.margins: -Theme.paddingSmall
                                                onDoubleClicked: page.fineReset("tint")
                                                onPressed: page.pickHoldStart(); onReleased: page.pickHoldStop(); onCanceled: page.pickHoldStop() }
                                }
                                // The ring of a white-balance pick being held (2.1 s).
                                Canvas {
                                    id: pickRing
                                    anchors.centerIn: shutter
                                    width: shutter.width + 2 * Theme.paddingMedium; height: width
                                    visible: page.pickProgress > 0
                                    onVisibleChanged: requestPaint()
                                    Connections { target: page; onPickProgressChanged: pickRing.requestPaint() }
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        ctx.beginPath()
                                        ctx.arc(width / 2, height / 2, width / 2 - 3, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * page.pickProgress)
                                        ctx.lineWidth = 4
                                        ctx.strokeStyle = "#80d8ff"
                                        ctx.stroke()
                                    }
                                }
                                MouseArea {
                                    id: shutter
                                    anchors.centerIn: parent
                                    width: Theme.itemSizeLarge; height: width
                                    enabled: viewfinder.running
                                    onClicked: page.capture()
                                    Image {
                                        anchors.fill: parent
                                        source: "image://theme/icon-camera-shutter"
                                        opacity: shutter.pressed ? 0.5 : 1.0
                                    }
                                }
                            }

                            Row {
                                width: parent.width; height: Theme.itemSizeSmall
                                Repeater {
                                    model: 3
                                    MouseArea {
                                        width: parent.width / 3; height: parent.height
                                        enabled: index === 1 || settingsMenu.advanced
                                        opacity: enabled ? 1.0 : 0.0
                                        onClicked: index === 0 ? page.fineWb("warmth", -1) : index === 2 ? page.fineWb("tint", -1)
                                                   : page.zoomTap(-1)
                                        // held: a progressive zoom out
                                        onPressAndHold: if (index === 1) page.zoomHoldBegin(-1)
                                        onReleased: if (index === 1) page.zoomHoldEnd()
                                        onCanceled: if (index === 1) page.zoomHoldEnd()
                                        // cool, neutral (zoom), green
                                        Label { anchors.centerIn: parent; text: "−"
                                                font.pixelSize: Theme.fontSizeLarge
                                                color: index === 0 ? "#70b8ff" : index === 2 ? "#70e080" : app.deckTextColor }
                                    }
                                }
                            }
                        }
                    }

                    // Simple mode: a brightness slider where the wheels were
                    // (the EV wheel is hidden too: both drive the same AE
                    // compensation, one control is enough).
                    BrightnessSlider {
                        id: brightness
                        visible: settingsMenu.simpleHides("wheels")
                        width: visible ? deck.speedW + deck.isoW : 0
                        height: parent.height
                        exposure: exposure
                        caps: viewfinder.caps
                    }

                    // Column 3: shutter-speed wheel. Live (turning with the
                    // exposure) until touched, then frozen = manual.
                    Item {
                        width: brightness.visible ? 0 : deck.speedW
                        visible: !brightness.visible
                        height: parent.height
                        ValueWheel {
                            anchors.fill: parent
                            visible: !settingsMenu.simpleHides("wheels")
                            enabled: !page.sceneOn; opacity: page.sceneOn ? Theme.opacityLow : 1.0
                            model: page.shutterModel
                            currentValue: exposure.displayShutterNs
                            live: !exposure.shutterManual
                            liveText: page.shutterLabel(exposure.displayShutterNs)
                            valueLabel: page.shutterLabel
                            prefixFor: page.shutterPrefix           // Σ for stacked speeds
                            subFor: page.shutterSub                 // real shot time, second line
                            onTouched: exposure.touchShutter()
                            onValueSelected: exposure.setShutterNs(value)
                        }
                        Label {
                            anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
                            visible: !exposure.shutterManual && !settingsMenu.simpleHides("wheels")
                            text: qsTr("auto")
                            font.pixelSize: Theme.fontSizeTiny; color: app.captionColor
                            opacity: Theme.opacityHigh
                        }
                    }

                    // Column 4: ISO wheel, same behaviour.
                    Item {
                        width: brightness.visible ? 0 : deck.isoW
                        visible: !brightness.visible
                        height: parent.height
                        ValueWheel {
                            anchors.fill: parent
                            anchors.leftMargin: page.compactDeck ? Theme.paddingLarge : 0   // compact: a little to the right
                            visible: !settingsMenu.simpleHides("wheels")
                            enabled: !page.sceneOn; opacity: page.sceneOn ? Theme.opacityLow : 1.0
                            model: page.isoModel
                            currentValue: exposure.displayIso
                            live: !exposure.isoManual
                            liveText: page.isoLabel(exposure.displayIso)
                            valueLabel: page.isoLabel
                            onTouched: exposure.touchIso()
                            onValueSelected: exposure.setIso(value)
                        }
                        Label {
                            anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter }
                            visible: !exposure.isoManual && !settingsMenu.simpleHides("wheels")
                            text: qsTr("auto")
                            font.pixelSize: Theme.fontSizeTiny; color: app.captionColor
                            opacity: Theme.opacityHigh
                        }
                    }
                }
            }
                    }                                   // cameraContent
                }                                       // VisualItemModel
            }                                           // switcher
        }
    }
}
