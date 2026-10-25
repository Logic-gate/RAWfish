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
    property real previewFps: 10                     // the Σ preview's target rate (indraw.conf)
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

    // A (re)started engine: tell it what it cannot know (source, pause), and
    // ask the state once so the deck shows it before the first change.
    Connections {
        target: viewfinder
        onRunningChanged: if (viewfinder.running) {
            logic.send("source " + (logic.rawSource ? "raw" : "vf"))
            logic.send("pause " + (logic.paused ? "on" : "off"))
            logic.send("pvfps " + logic.previewFps)
            logic.send("state")
        }
    }
}
