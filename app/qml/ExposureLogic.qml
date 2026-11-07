/*
 * ExposureLogic.qml -- the deck's view of the exposure (non-visual).
 *
 * Since 0.47.3 the exposure BEHAVIOUR lives in the controller
 * (sailfish/ctl_exposure.c, docs/ARCHITECTURE.md step 3): the modes
 * (automatic, ISO / speed priority, manual), the loop that drives the free
 * wheel in a priority mode, the spot measure of a light reference, the
 * calibrated target, the manual offset and the metered EV100. This item
 * only:
 *   - sends the user's choices as controller commands ("xp ...");
 *   - exposes the controller's state ("xpstate" event, viewfinder.exposureState)
 *     under the names the deck has always used.
 * The names kept: fullAuto, fullManual, isoManual, shutterManual, fixedPair,
 * spotHold, displayIso, displayShutterNs, evSteps, manualOffsetStops,
 * manualOffsetValid, meteredEv100, shutterMaxNs, isStacked(), rawSource,
 * paused; touchIso(), touchShutter(), setIso(), setShutterNs(), setEv(),
 * resetAuto(), startSpot(), stopSpot().
 */
import QtQuick 2.0

Item {
    id: logic
    visible: false

    property var viewfinder                  // the IndrawPreview item
    property string imageType: "jpg"         // "raw" | "jpg" | "jpg yuv"
    readonly property var caps: viewfinder ? viewfinder.caps : ({})
    readonly property var st: viewfinder ? viewfinder.exposureState : ({})

    // --- The state, as the controller reports it ------------------------------
    readonly property bool isoManual: !!st.isoManual
    readonly property bool shutterManual: !!st.shutterManual
    readonly property bool fullAuto: st.fullAuto === undefined ? true : !!st.fullAuto
    readonly property bool fullManual: !!st.fullManual
    readonly property bool fixedPair: !!st.fixed
    readonly property bool spotHold: !!st.spot
    readonly property int  displayIso: st.iso > 0 ? st.iso : (viewfinder && viewfinder.liveIso > 0 ? viewfinder.liveIso : 100)
    readonly property real displayShutterNs: st.expNs > 0 ? st.expNs : (viewfinder && viewfinder.liveExposureNs > 0 ? viewfinder.liveExposureNs : 16666667)
    readonly property int  evSteps: st.ev !== undefined ? st.ev : 0
    readonly property real manualOffsetStops: st.offset !== undefined ? st.offset : 0
    readonly property bool manualOffsetValid: !!st.offsetValid
    readonly property real meteredEv100: st.ev100 !== undefined ? st.ev100 : NaN
    // A Σ preview at its limits (the controller's report): slowed by the ISO
    // limit; darker than the photo by `pvDeficit` stops; too dark to measure.
    readonly property real pvMs: st.pvMs !== undefined ? st.pvMs : 0
    readonly property bool pvSlow: !!st.pvSlow
    readonly property real pvDeficit: st.deficit !== undefined ? st.deficit : 0
    readonly property bool pvTooDark: !!st.tooDark
    property real previewFps: 10                     // the Σ preview's target rate (rawfish-ir.conf)
    onPreviewFpsChanged: send("pvfps " + previewFps)

    // Stacked exposures (longer than the single-exposure ceiling, Σ on the wheel).
    readonly property real shutterMaxNs: caps.shutterMaxNs ? caps.shutterMaxNs : 500000000
    function isStacked(ns) { return ns > shutterMaxNs + 1 }
    readonly property bool rawSource: imageType === "raw"

    // Set by the page while the loupe moves without a reference, or a focus
    // search runs: the controller then leaves the exposure alone.
    property bool paused: false

    // --- The user's choices, to the controller ---------------------------------
    function send(c) { if (viewfinder) viewfinder.sendCommand("xp " + c) }
    function touchIso() { send("touch iso") }
    function touchShutter() { send("touch speed") }
    function setIso(iso) { send("iso " + Math.round(iso)) }
    function setShutterNs(ns) { send("speed " + Math.round(ns)) }
    function setEv(steps) { send("ev " + steps) }
    function resetAuto() { send("auto") }
    function startSpot() { send("spot on") }
    function stopSpot() { send("spot off") }
    onPausedChanged: send("pause " + (paused ? "on" : "off"))
    onRawSourceChanged: send("source " + (rawSource ? "raw" : "vf"))

    // --- Kept across an engine restart ------------------------------------------
    // The controller lives in the engine's process: a restart (back from the
    // background, a size or type change, another camera) starts it in
    // automatic. The mode and the manual values are kept here and sent again
    // once the new engine's capabilities are known. On another camera the
    // mode stays and the light is kept: the ISO as it was, the speed scaled
    // by the square of the apertures' ratio, within the new camera's ranges.
    property bool kIsoManual: false
    property int kIso: 0
    property bool kShutterManual: false
    property real kShutterNs: 0
    property int kEv: 0
    property string kCamera: ""
    property real kAperture: 0
    property bool synced: false              // the state shown is the user's, not a fresh engine's
    property bool pendingRestore: false      // a fresh engine: restore at its first caps
    function keep() {
        if (!viewfinder || !viewfinder.running || st.fullAuto === undefined) return
        kIsoManual = !!st.isoManual; kShutterManual = !!st.shutterManual
        if (kIsoManual && st.iso > 0) kIso = st.iso
        if (kShutterManual && st.expNs > 0) kShutterNs = st.expNs
        kEv = st.ev !== undefined ? st.ev : 0
        kCamera = viewfinder.cameraId
        if (caps.aperture > 0) kAperture = caps.aperture
    }
    onStChanged: {
        if (!viewfinder || !viewfinder.running || st.fullAuto === undefined || pendingRestore) return
        if (!synced) {                       // a fresh engine: wait until the restored modes show
            if (!!st.isoManual !== kIsoManual || !!st.shutterManual !== kShutterManual) return
            synced = true; syncTimer.stop()
        }
        keep()
    }
    // A state that never matches the restored modes (refused by the engine):
    // follow what the engine says from then on.
    Timer { id: syncTimer; interval: 2000; onTriggered: { logic.synced = true; logic.keep() } }

    function restore() {
        pendingRestore = false
        var iso = kIso, ns = kShutterNs
        if (kCamera !== "" && kCamera !== viewfinder.cameraId) {
            // another camera: the same light; the ISO kept, the speed scaled
            if (kAperture > 0 && caps.aperture > 0) ns = ns * Math.pow(caps.aperture / kAperture, 2)
            if (caps.isoMin > 0 && iso < caps.isoMin) iso = caps.isoMin
            if (caps.isoMax > 0 && iso > caps.isoMax) iso = caps.isoMax
            if (caps.expMinNs > 0 && ns < caps.expMinNs) ns = caps.expMinNs
            if (ns > 16000000000) ns = 16000000000
            viewfinder.sendCommand("# exposure carried to camera " + viewfinder.cameraId + ": iso " + iso + " speed " + Math.round(ns) + " ns (aperture " + kAperture + " -> " + caps.aperture + ")")
        }
        if (kIsoManual && iso > 0) send("iso " + iso)
        if (kShutterManual && ns > 0) send("speed " + Math.round(ns))
        if (kEv !== 0) send("ev " + kEv)
        send("state")
        syncTimer.restart()
    }

    // A (re)started engine: tell it what it cannot know (source, pause), and
    // give it back the mode and the values (its capabilities come before its
    // first frame; should they come later, the restore waits for them).
    Connections {
        target: viewfinder
        onRunningChanged: if (viewfinder.running) {
            logic.send("source " + (logic.rawSource ? "raw" : "vf"))
            logic.send("pause " + (logic.paused ? "on" : "off"))
            logic.send("pvfps " + logic.previewFps)
            logic.pendingRestore = true
            if (caps.isoMax !== undefined) logic.restore()   // the caps come before the first frame
        } else {
            logic.synced = false
        }
        onCapsChanged: if (logic.pendingRestore && viewfinder.running && caps.isoMax !== undefined) logic.restore()
    }
}
