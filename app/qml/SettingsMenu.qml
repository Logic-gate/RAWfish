/*
 * SettingsMenu.qml -- the settings overlay, pulled down over the viewfinder,
 * in the Jolla Camera / RAWfish style: the image dims, a header label names
 * the value under the finger, and the settings stand as columns of round
 * items (icon or short text), four columns per page, pages side by side
 * with dots. Everything atomic: one column = one control.
 *
 * Columns: flash, mode, self-timer, format, resolution, JPEG quality, white
 * balance presets, noise reduction, sharpening (and the viewfinder's, if the
 * Settings page asks), bracketing, grid, Σ denoise (the zebra and peaking
 * tweaks are in rawfish-ir.conf only); the scenes only when the Settings page's switch is on (grouped
 * modes override the atomic controls). The fine white balance lives on the
 * deck (the ± around the shutter), no longer here; the capture's 3A wait is
 * the engine's default (1.5 s). Values persist in the app's settings file
 * (viewfinder.setting / setSetting); the live ones are re-sent to the engine
 * at every (re)start.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Item {
    id: menu
    property var viewfinder
    property bool gridOn: false
    // The level's zero (ui 175): the roll and pitch the sensor reports when
    // the phone is truly upright, recorded by a long press on "level" with
    // the phone held straight; subtracted from every reading.
    property real levelRoll0: 0
    property real levelPitch0: 0
    readonly property bool levelZeroed: levelRoll0 !== 0 || levelPitch0 !== 0
    function setLevelZero(r, p) { levelRoll0 = r; levelPitch0 = p; save("levelRoll0", r.toFixed(2)); save("levelPitch0", p.toFixed(2)) }
    function clearLevelZero() { levelRoll0 = 0; levelPitch0 = 0; save("levelRoll0", ""); save("levelPitch0", "") }
    property bool focusCalibrationMenu: false      // the Settings page's switch
    signal captureSizeChosen(string wxh)     // "" = largest (engine picks)
    signal closeRequested()

    // --- values (persisted) ---
    property int timerSec: 0
    property string captureSize: ""
    // "" (the default, nothing saved): the RAW's shape -- 4:3 on the JP2,
    // 16:9 on the XA2 -- so an untouched install takes all the sensor gives.
    property string formatSaved: ""
    readonly property string format: formatSaved !== "" ? formatSaved : sensorFormat
    property int jpegQuality: 95
    property int awbMode: 1
    // Photo processing (the Noise / Sharpen columns): sent with each JPEG.
    property int noiseIndex: 1
    property int edgeIndex: 1
    // Preview processing, separate: for judging framing, focus and exposure.
    // Default: fast noise reduction, no sharpening (a sharpened noisy preview
    // turns into a maze at high ISO; focus is judged with the peaking).
    property int previewNoiseIndex: 2
    property int previewEdgeIndex: 1
    property bool previewColumns: false              // the Settings page's switch
    // The mode at start (Settings page): "last" = as left, else "simple" or "advanced".
    property string startMode: "last"
    // Bracketing: "off", "0" (three shots in a row at the same exposure, for a
    // short moment), or "1".."3" (three shots at -N, 0, +N stops).
    property string bracket: "off"
    property string flash: "off"                   // off | auto | on | torch (if the camera has a flash)
    property int stackDenoise: 1                   // stack: frames multiplied then divided out (1, 2, 4)
    property int frameLatencyMs: 200               // measured HAL time between sub-frames (updated after each stack)
    property bool advanced: true                   // simple = jpg only, no wheels
    // The Σ preview's target frame rate (rawfish-ir.conf, hand-edited): the ISO is
    // raised so that one frame this short stands for the stack.
    property int sigmaPreviewFps: 10
    property int wbShift: 0                        // steps of 5 % red/blue from the AWB; 0 = AWB
    property int tint: 0                           // percent of green, - green .. + magenta
    // Simple mode layout, from ~/.config/com.rawfish/rawfish-ir/simple.json
    // (defaults below): what it hides, what it fixes, which menu columns stay.
    property var simple: ({ "hide": ["wheels", "type", "histogram", "headers"],
                            "quality": 95, "noise": "minimal_noise", "edge": "hq_edge",
                            "menu": ["timer", "format", "wb"] })
    function simpleHides(what) { return !advanced && simple.hide && simple.hide.indexOf(what) >= 0 }
    function simpleMenuHas(col) { return advanced || (simple.menu && simple.menu.indexOf(col) >= 0) }
    property int sceneMode: 0                      // 0 = off (atomic controls)
    property bool showScenes: false                // the Settings page's switch (scenes in the menu)
    property int peakTweak: 0                      // -30 | 0 | +30 (% on the device value)
    property int zebraTweak: 0
    property string imageType: "jpg"               // from the deck, for the size rules
    readonly property var sceneAvailable: viewfinder && viewfinder.caps.sceneModes ? viewfinder.caps.sceneModes : []
    readonly property var sceneNames: { 0: qsTr("Off"), 1: qsTr("Face priority"), 2: qsTr("Action"), 3: qsTr("Portrait"),
                                        4: qsTr("Landscape"), 5: qsTr("Night"), 6: qsTr("Night portrait"), 7: qsTr("Theatre"),
                                        8: qsTr("Beach"), 9: qsTr("Snow"), 10: qsTr("Sunset"), 11: qsTr("Steady photo"),
                                        12: qsTr("Fireworks"), 13: qsTr("Sports"), 14: qsTr("Party"), 15: qsTr("Candlelight"),
                                        16: qsTr("Barcode"), 18: qsTr("HDR") }
    // Short forms under the menu's round buttons (about six characters).
    readonly property var sceneShort: { 0: qsTr("off"), 1: qsTr("face"), 2: qsTr("action"), 3: qsTr("portr."),
                                        4: qsTr("landsc"), 5: qsTr("night"), 6: qsTr("n.port"), 7: qsTr("theatr"),
                                        8: qsTr("beach"), 9: qsTr("snow"), 10: qsTr("sunset"), 11: qsTr("steady"),
                                        12: qsTr("firew."), 13: qsTr("sports"), 14: qsTr("party"), 15: qsTr("candle"),
                                        16: qsTr("barcod"), 18: qsTr("HDR") }
    readonly property var sceneIcons: { 3: "icon-camera-mode-portrait", 5: "icon-camera-mode-night",
                                        13: "icon-camera-mode-sports", 18: "icon-camera-mode-hdr" }
    readonly property var sceneList: [0].concat(sceneAvailable.filter(function(m) { return m > 0 && m !== 17 }))
    function sceneChunk(i) { return sceneList.slice(i * 5, i * 5 + 5) }

    // The photo's noise reduction and sharpening as offered to the user. The
    // ZSL modes are left out (ui 179): they belong to a zero-shutter-lag
    // capture with reprocessing that indRAW does not do; alone they mean what
    // the HAL decides (fast or high quality). An old saved index past the end
    // is clamped (zsl_noise -> minimal, zsl_edge -> hq). docs/ROADMAP: ZSL.
    readonly property var noiseModes: ["default_noise","off_noise","fast_noise","hq_noise","minimal_noise"]
    readonly property var edgeModes:  ["default_edge","off_edge","fast_edge","hq_edge"]
    // The modes as shown: the full name in the header, a short form (four
    // characters at most) under the button. The keys stay the engine's.
    readonly property var modeNames: { "default": qsTr("default"), "off": qsTr("off"), "fast": qsTr("fast"),
                                       "hq": qsTr("high quality"), "minimal": qsTr("minimal") }
    readonly property var modeShort: { "default": qsTr("def"), "off": qsTr("off"), "fast": qsTr("fast"),
                                       "hq": qsTr("hq"), "minimal": qsTr("min") }
    function modeName(m) { var k = shortName(m); return modeNames[k] || k }
    function modeShortName(m) { var k = shortName(m); return modeShort[k] || k.substr(0, 4) }
    readonly property var awbNames: { 1: qsTr("Automatic"), 2: qsTr("Incandescent"), 3: qsTr("Fluorescent"),
                                      4: qsTr("Warm fluorescent"), 5: qsTr("Daylight"), 6: qsTr("Cloudy"),
                                      7: qsTr("Twilight"), 8: qsTr("Shade") }
    // Short forms under the buttons (about three characters).
    readonly property var awbShort: { 1: qsTr("A"), 2: qsTr("inc"), 3: qsTr("flu"), 4: qsTr("wfl"), 5: qsTr("sun"),
                                      6: qsTr("cld"), 7: qsTr("twi"), 8: qsTr("shd") }
    readonly property var awbIcons: { 1: "icon-camera-wb-automatic", 2: "icon-camera-wb-tungsten",
                                      3: "icon-camera-wb-fluorecent", 5: "icon-camera-wb-sunny", 6: "icon-camera-wb-cloudy",
                                      7: "icon-camera-wb-sunset", 8: "icon-camera-wb-shade" }
    readonly property var awbCommands: { 1: "auto", 2: "incandescent", 3: "fluorescent", 4: "warm_fluorescent",
                                         5: "daylight", 6: "cloudy", 7: "twilight", 8: "shade" }
    readonly property var awbAvailable: viewfinder && viewfinder.caps.awbModes ? viewfinder.caps.awbModes : [1]
    readonly property var jpegSizes: viewfinder && viewfinder.caps.jpegSizes ? viewfinder.caps.jpegSizes : []
    // The shape of the RAW stream the HAL gives: "4:3" (the JP2, the whole
    // sensor) or "16:9" (the Xperia XA2: a 16:9 readout of a 4:3 IMX300 --
    // indRAW never sees more than the HAL delivers). The format that matches
    // it is "as the RAW"; the other one crops (or, on the XA2, is another
    // readout: 4208x3120, taller than the 16:9 JPEG but narrower).
    readonly property string sensorFormat: viewfinder && viewfinder.caps.rawW > 0 && viewfinder.caps.rawH > 0
                                           && viewfinder.caps.rawW / viewfinder.caps.rawH > 1.6 ? "16:9" : "4:3"
    // The JPEG size the engine is running at, "WxH" ("" until known).
    readonly property string runningSize: viewfinder && viewfinder.caps.jpegW > 0 ? viewfinder.caps.jpegW + "x" + viewfinder.caps.jpegH : ""

    // Settings come back from an INI file as strings: every one is parsed
    // and clamped to what the menu offers, so a hand-edited or corrupt file
    // cannot send the engine a value it refuses (a bad size stopped it).
    function num(key, def, lo, hi) {
        var v = Number(viewfinder.setting(key, def))
        if (!isFinite(v)) v = def
        return Math.max(lo, Math.min(hi, Math.round(v)))
    }
    function pick(key, def, allowed) {
        var v = String(viewfinder.setting(key, def))
        return allowed.indexOf(v) >= 0 ? v : def
    }
    function load() {
        timerSec = pick("timerSec", "0", ["0", "3", "10"]) * 1
        var sz = String(viewfinder.setting("captureSize", ""))
        captureSize = /^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$/.test(sz) ? sz : ""
        formatSaved = pick("format", "", ["4:3", "16:9"])   /* "" = the RAW's shape (format) */
        jpegQuality = pick("jpegQuality", "95", ["80", "90", "95", "100"]) * 1
        awbMode = num("awbMode", 1, 1, 8)
        noiseIndex = num("noiseIndex", 1, 0, noiseModes.length - 1)
        edgeIndex = num("edgeIndex", 1, 0, edgeModes.length - 1)
        previewNoiseIndex = num("previewNoiseIndex", 2, 0, noiseModes.length - 1)
        previewEdgeIndex = num("previewEdgeIndex", 1, 0, edgeModes.length - 1)
        bracket = pick("bracket", "off", ["off", "0", "1", "2", "3"])
        gridOn = num("gridOn", 0, 0, 1) === 1
        levelRoll0 = num("levelRoll0", 0, -45, 45)
        levelPitch0 = num("levelPitch0", 0, -45, 45)
        advanced = startMode === "simple" ? false : startMode === "advanced" ? true : num("advanced", 1, 0, 1) === 1
        sceneMode = num("sceneMode", 0, 0, 18)
        peakTweak = pick("peakTweak", "0", ["-30", "0", "30"]) * 1
        zebraTweak = pick("zebraTweak", "0", ["-30", "0", "30"]) * 1
        flash = pick("flash", "off", ["off", "auto", "on", "torch"])
        stackDenoise = pick("stackDenoise", "1", ["1", "2", "4"]) * 1
        frameLatencyMs = num("frameLatencyMs", 200, 0, 5000)
        wbShift = num("wbShift", 0, -20, 20)
        wbFixed = String(viewfinder.setting("wbFixed", "") || "")
        sigmaPreviewFps = num("sigmaPreviewFps", 10, 1, 30)
        shadingPivot = num("shadingPivot", 50, 0, 100)
        shadingStrength = num("shadingStrength", 100, 0, 100)
        shadingMaxGain = num("shadingMaxGain", 5, 1, 8)
        fineWarmthStep = num("fineWarmthStep", 1, 1, 4)
        fineTintStep = num("fineTintStep", 5, 1, 25)
        tint = num("tint", 0, -100, 100)
        // simple.json: only an object of the expected shape is taken; any
        // other content keeps the defaults (a typo must not break the mode).
        var j = viewfinder.readJsonFile(viewfinder.configDir + "/simple.json")
        if (j && j.hide instanceof Array && j.menu instanceof Array) {
            simple = { "hide": j.hide.filter(function(x) { return typeof x === "string" }),
                       "quality": Math.max(1, Math.min(100, Number(j.quality) || 95)),
                       "noise": noiseModes.indexOf(j.noise) >= 0 ? j.noise : "minimal_noise",
                       "edge": edgeModes.indexOf(j.edge) >= 0 ? j.edge : "hq_edge",
                       "menu": j.menu.filter(function(x) { return typeof x === "string" }) }
        }
    }
    // Simple mode: fixed JPEG quality from the layout (its noise and edge are
    // in photoProcessing()).
    readonly property int effQuality: advanced ? jpegQuality : Number(simple.quality || 95)
    function save(key, value) { viewfinder.setSetting(key, value) }
    // Fine white balance from the deck (the small + / - around the shutter):
    // warmth by one 5 % step of red/blue, tint by 5 % of green.
    // Step sizes, from the .conf (hand-edited there, not in the menu):
    // fineWarmthStep in 5 % units of red/blue (1 = 5 %), fineTintStep in % of green.
    // A white balance measured on a neutral object (a long press on the
    // kelvin or tint readout): "R,Ge,Go,B" gains, "" when none. Kept across
    // restarts (the app asks at start whether to keep it).
    property string wbFixed: ""
    function setWbFixed(gains) {
        wbFixed = gains; save("wbFixed", gains)
        wbShift = 0; tint = 0; save("wbShift", 0); save("tint", 0)
        applyLive()
    }
    function clearWbFixed() {
        wbFixed = ""; save("wbFixed", "")
        wbShift = 0; tint = 0; save("wbShift", 0); save("tint", 0)
        if (viewfinder && viewfinder.running) viewfinder.sendCommand("wb fixed off")
        applyLive()
    }
    // The measured lens-shading map's dosing (rawfish-ir.conf, hand-edited; see
    // shading.h): pivot 0..100 (0 only brightens the edges, 100 only darkens
    // towards the darkest corner -- darktable clips what is pushed above
    // white), strength 0..100, maximum gain.
    property int shadingPivot: 50
    property int shadingStrength: 100
    property real shadingMaxGain: 5
    property int fineWarmthStep: 1
    property int fineTintStep: 5
    function resetWarmth() { wbShift = 0; save("wbShift", 0); applyLive() }
    function resetTint() { tint = 0; save("tint", 0); applyLive() }
    function stepWarmth(d) { wbShift = Math.max(-20, Math.min(20, wbShift + d * fineWarmthStep)); save("wbShift", wbShift); applyLive() }
    function stepTint(d) { tint = Math.max(-100, Math.min(100, tint + d * fineTintStep)); save("tint", tint); applyLive() }
    // The noise and edge options a JPEG / burst command carries: the photo
    // columns in advanced mode, simple.json's in simple mode.
    function photoProcessing() {
        var ni = advanced ? noiseIndex : Math.max(0, noiseModes.indexOf(simple.noise || "minimal_noise"))
        var ei = advanced ? edgeIndex : Math.max(0, edgeModes.indexOf(simple.edge || "hq_edge"))
        return " " + noiseModes[ni] + " " + edgeModes[ei]
    }
    // The same two modes as "noise=... edge=..." fields (the controller's "sh").
    function photoProcessingFields() {
        var ni = advanced ? noiseIndex : Math.max(0, noiseModes.indexOf(simple.noise || "minimal_noise"))
        var ei = advanced ? edgeIndex : Math.max(0, edgeModes.indexOf(simple.edge || "hq_edge"))
        return " noise=" + noiseModes[ni] + " edge=" + edgeModes[ei]
    }
    function applyLive() {
        if (!viewfinder || !viewfinder.running) return
        // The preview's own processing (the photos' goes with each shot:
        // photoProcessing()).
        viewfinder.sendCommand("preview " + noiseModes[previewNoiseIndex] + " " + edgeModes[previewEdgeIndex])
        viewfinder.sendCommand("shading balance=" + shadingPivot + " strength=" + shadingStrength + " maxgain=" + shadingMaxGain)
        // A measured white first: it is the base the mode and the shifts keep.
        if (advanced && wbFixed !== "") viewfinder.sendCommand("wb fixed " + wbFixed)
        viewfinder.sendCommand("wb " + (awbCommands[awbMode] || "auto"))
        if (advanced) viewfinder.sendCommand("wb shift=" + wbShift + " tint=" + tint)
        // A scene the camera does not list (another device's setting, a hand
        // edit) is sent as off, never as a wrong number.
        var sc = showScenes && sceneMode > 0 && sceneAvailable.indexOf(sceneMode) >= 0 ? sceneMode : 0
        viewfinder.sendCommand("scene " + (sc > 0 ? sc : "off"))
        // The flash only where the camera has one (a front camera usually not).
        if (viewfinder.caps.flash) viewfinder.sendCommand("flash " + flash)
    }
    onAdvancedChanged: applyLive()
    Connections { target: viewfinder; onRunningChanged: if (viewfinder.running) menu.applyLive() }

    function shortName(m) { return m.replace("_noise","").replace("_edge","") }
    function mp(sz) { if (sz === "") return qsTr("max"); var p = sz.split("x"); return qsTr("%1 MP").arg(Math.round(p[0] * p[1] / 1e5) / 10) }
    // Listed JPEG sizes of a ratio, largest first.
    // Listed JPEG sizes of a ratio that are clean binnings (1/1, 1/2, 1/4)
    // of the LARGEST SIZE OF THAT RATIO: the odd in-between sizes the HAL
    // lists (a 12.2 MP one froze the viewfinder) are left out. Judging them
    // against the largest size of all was wrong on a 16:9 RAW: the 4:3
    // sizes are smaller than the 16:9 maximum in every dimension (the XA2's
    // 4208x3120 under 5984x3392), so the 4:3 list came out empty and the
    // menu could not leave 16:9 (ui 158). In raw the JPEG cannot be wider
    // than the RAW16 stream (the 50 MP mode goes).
    function sizesFor(fmt) {
        var want = fmt === "16:9" ? 16 / 9 : 4 / 3, same = [], list = [], refW = 0
        for (var i = 0; i < jpegSizes.length; ++i) {
            var q = jpegSizes[i].split("x"), qw = Number(q[0]), qh = Number(q[1])
            if (Math.abs(qw / qh - want) > 0.02) continue
            same.push(jpegSizes[i]); refW = Math.max(refW, qw)
        }
        var rawW = viewfinder && viewfinder.caps.rawW > 0 ? viewfinder.caps.rawW : refW
        for (var j = 0; j < same.length; ++j) {
            var p = same[j].split("x"), w = Number(p[0])
            var ratio = refW / w
            if (Math.abs(ratio - Math.round(ratio)) > 0.02 || [1, 2, 4].indexOf(Math.round(ratio)) < 0) continue
            if (imageType === "raw" && w > rawW) continue
            list.push(same[j])
        }
        list.sort(function(a, b) { var pa = a.split("x"), pb = b.split("x"); return pb[0]*pb[1] - pa[0]*pa[1] })
        return list
    }
    // The size in force: the chosen one if still allowed, else the largest allowed.
    function effectiveSize() {
        var l = sizesFor(format)
        return l.indexOf(captureSize) >= 0 ? captureSize : (l[0] || "")
    }
    // A type change can narrow the sizes (in raw the JPEG is no wider than the
    // RAW16 stream): settle like a camera change -- restart only if the engine
    // is not already running at the size that results. Until ui 184 it always
    // set the size, and on a fresh install (the size still "", the engine on
    // "max") the first switch to RAW restarted the engine for nothing (XA2,
    // 29 Oct 2026: "Starting camera..." at the first RAW).
    // Deferred, like onJpegSizesChanged: a camera without RAW sets the type
    // aside to jpg while its caps are being taken in, and settling inside that
    // change used the PREVIOUS camera's sizes: the JP2's front camera was asked
    // for 8192x6144 (the main camera's 50 MP), which it does not list, and the
    // engine stopped ('# settle: format 4:3 wants 8192x6144', ui 185).
    onImageTypeChanged: Qt.callLater(settleSize)
    function setSize(sz) {
        captureSize = sz; save("captureSize", sz)
        if (sz !== "") save(format === "16:9" ? "captureSize169" : "captureSize43", sz)   /* the format's own */
        captureSizeChosen(sz)
    }
    // The size the format implies for THIS camera, once its sizes are known:
    // restart only if the engine is not already running at it (it resolves
    // "max" itself; a new camera's largest size of the format usually is).
    function settleSize() {
        if (jpegSizes.length === 0) return
        var e = effectiveSize()
        if (e === captureSize) return
        viewfinder.sendCommand("# settle: format " + format + " wants " + e + ", engine runs " + runningSize
                               + (e === runningSize ? " (kept)" : " (restart)"))
        if (e === runningSize) { captureSize = e; save("captureSize", e) }   /* already so: no restart */
        else setSize(e)
    }
    // Deferred: inside onJpegSizesChanged the other bindings on the new caps
    // (runningSize, sensorFormat) are not re-evaluated yet -- the JP2 log
    // showed 'engine runs 4096x3072' (the previous camera's size) for a
    // camera just opened at 6560x4928, hence a second engine start on every
    // switch (ui 170). Qt.callLater runs it once the bindings have settled.
    onJpegSizesChanged: Qt.callLater(settleSize)

    // --- the overlay ---
    Rectangle { anchors.fill: parent; color: "black"; opacity: 0.6 }
    // Outside the columns: a tap closes the menu, and so does an upward swipe
    // (the mirror of the opening swipe). One MouseArea that keeps the press:
    // the earlier version gave the press away and never saw the swipe.
    MouseArea {
        anchors.fill: parent
        property real startY: 0
        property bool swiped: false
        onPressed: { startY = mouse.y; swiped = false }
        onPositionChanged: if (!swiped && startY - mouse.y > Theme.itemSizeSmall / 3) { swiped = true; menu.closeRequested() }
        onClicked: if (!swiped) menu.closeRequested()
    }

    MenuHeader { id: header; anchors { top: parent.top; topMargin: Theme.paddingMedium; left: parent.left; right: parent.right } }

    // The settings band, wider than the screen (RAWfish): drag it left/right;
    // the bar below shows where you are, like a scroll indicator.
    Flickable {
        id: band
        anchors { top: header.bottom; left: parent.left; right: parent.right; bottom: bar.top; bottomMargin: Theme.paddingMedium }
        clip: true
        // Horizontal scrolling of the band; an upward swipe on it closes the
        // menu too: the band is made one pixel taller than its view so it can
        // be dragged vertically, and pulling it up past a third of an item
        // closes (it never really scrolls vertically).
        flickableDirection: Flickable.AutoFlickDirection
        boundsBehavior: Flickable.DragOverBounds
        contentWidth: row.width + 2 * Theme.horizontalPageMargin
        contentHeight: height + 1
        onContentYChanged: if (draggingVertically && contentY > Theme.itemSizeSmall / 3) { cancelFlick(); contentY = 0; menu.closeRequested() }
        readonly property real colW: Theme.iconSizeMedium + 2 * Theme.paddingMedium
        Row {
            id: row
            x: Theme.horizontalPageMargin
            spacing: Theme.paddingMedium
            // Flash: off / auto / on (fires with the photo; automatic exposure
            // meters with a pre-flash first, ~0.5-1 s more) / torch (continuous).
            // First column, in simple mode too.
            MenuColumn { visible: !!(menu.viewfinder && menu.viewfinder.caps.flash)
                width: band.colW; header: header
                caption: qsTr("Flash"); values: ["off", "auto", "on", "torch"]; current: menu.flash
                labelFor: function(v) { return v === "off" ? qsTr("Flash off") : v === "auto" ? qsTr("Flash when dark")
                                              : v === "on" ? qsTr("Flash always") : qsTr("Torch (continuous light)") }
                shortFor: function(v) { return v === "torch" ? qsTr("torch") : v }
                iconFor: function(v) { return v === "off" ? "icon-camera-flash-off" : v === "auto" ? "icon-camera-flash-automatic"
                                              : v === "on" ? "icon-camera-flash-on" : "" }
                onPicked: { menu.flash = value; menu.save("flash", value); menu.applyLive() } }
            MenuColumn { width: band.colW; header: header
                caption: qsTr("Mode"); values: [false, true]; current: menu.advanced
                labelFor: function(v) { return v ? qsTr("Advanced: wheels, raw, every control") : qsTr("Simple: JPEG, automatic") }
                shortFor: function(v) { return v ? qsTr("adv") : qsTr("simp") }
                onPicked: { menu.advanced = value; menu.save("advanced", value ? 1 : 0) } }
            MenuColumn { visible: menu.simpleMenuHas("timer"); width: band.colW; header: header
                caption: qsTr("Timer"); values: [0, 3, 10]; current: menu.timerSec
                labelFor: function(v) { return v === 0 ? qsTr("No timer") : qsTr("%1 second timer").arg(v) }
                shortFor: function(v) { return v === 0 ? qsTr("off") : v + "s" }
                iconFor: function(v) { return v === 0 ? "icon-camera-timer" : "icon-camera-timer-" + v + "s" }
                onPicked: { menu.timerSec = value; menu.save("timerSec", value) } }
            MenuColumn { visible: menu.simpleMenuHas("format"); width: band.colW; header: header
                caption: qsTr("Format"); values: ["4:3", "16:9"]; current: menu.format
                // "as the RAW" is whichever format has the RAW stream's shape
                // (4:3 on the JP2, 16:9 on the XA2); the other one crops. Up to
                // ui 156 "4:3" asked for the largest size of all, which on a
                // 16:9 RAW is 16:9: the format did nothing on the XA2.
                labelFor: function(v) { return v === menu.sensorFormat ? qsTr("%1, as the RAW").arg(v) : qsTr("%1 (crop)").arg(v) }
                onPicked: {
                    // The size last chosen in that format, if still listed; else
                    // its largest (ui 170: coming back to 4:3 used to land on the
                    // largest, 50 MP, whatever the user had picked there).
                    var list = menu.sizesFor(value), sz = list[0] || ""
                    var remembered = String(viewfinder.setting(value === "16:9" ? "captureSize169" : "captureSize43", ""))
                    if (list.indexOf(remembered) >= 0) sz = remembered
                    if (sz === "") { header.show(qsTr("No %1 size listed").arg(value)); return }
                    menu.formatSaved = value; menu.save("format", value); menu.setSize(sz) } }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Resolution")
                values: menu.sizesFor(menu.format)
                current: menu.effectiveSize()
                labelFor: function(v) { return v }
                shortFor: function(v) { return menu.mp(v) }
                onPicked: menu.setSize(value) }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Quality"); values: [80, 90, 95, 100]; current: menu.jpegQuality
                labelFor: function(v) { return qsTr("JPEG quality %1").arg(v) }
                onPicked: { menu.jpegQuality = value; menu.save("jpegQuality", value) } }
            MenuColumn { visible: menu.simpleMenuHas("wb"); width: band.colW; header: header
                caption: qsTr("White bal."); values: menu.awbAvailable.slice(0, 8); current: menu.awbMode
                labelFor: function(v) { return menu.awbNames[v] || String(v) }
                shortFor: function(v) { return menu.awbShort[v] || String(v) }
                iconFor: function(v) { return menu.awbIcons[v] || "" }
                // A preset also clears the manual shift and tint.
                onPicked: { menu.awbMode = value; menu.wbShift = 0; menu.tint = 0
                            menu.save("awbMode", value); menu.save("wbShift", 0); menu.save("tint", 0); menu.applyLive() } }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Noise"); values: [0, 1, 2, 3, 4]; current: menu.noiseIndex
                labelFor: function(v) { return qsTr("Photo noise reduction: %1").arg(menu.modeName(menu.noiseModes[v])) }
                shortFor: function(v) { return menu.modeShortName(menu.noiseModes[v]) }
                onPicked: { menu.noiseIndex = value; menu.save("noiseIndex", value) } }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Sharpen"); values: [0, 1, 2, 3]; current: menu.edgeIndex
                labelFor: function(v) { return qsTr("Photo sharpening: %1").arg(menu.modeName(menu.edgeModes[v])) }
                shortFor: function(v) { return menu.modeShortName(menu.edgeModes[v]) }
                onPicked: { menu.edgeIndex = value; menu.save("edgeIndex", value) } }
            // The viewfinder's processing (Vf), separate from the photos' (shown
            // when "Viewfinder processing in the camera menu" is on in Settings).
            MenuColumn { visible: menu.advanced && menu.previewColumns; width: band.colW; header: header
                caption: qsTr("Vf noise"); values: [0, 1, 2, 3, 4]; current: menu.previewNoiseIndex
                labelFor: function(v) { return qsTr("Viewfinder noise reduction: %1").arg(menu.modeName(menu.noiseModes[v])) }
                shortFor: function(v) { return menu.modeShortName(menu.noiseModes[v]) }
                onPicked: { menu.previewNoiseIndex = value; menu.save("previewNoiseIndex", value); menu.applyLive() } }
            MenuColumn { visible: menu.advanced && menu.previewColumns; width: band.colW; header: header
                caption: qsTr("Vf sharpen"); values: [0, 1, 2, 3]; current: menu.previewEdgeIndex
                labelFor: function(v) { return qsTr("Viewfinder sharpening: %1").arg(menu.modeName(menu.edgeModes[v])) }
                shortFor: function(v) { return menu.modeShortName(menu.edgeModes[v]) }
                onPicked: { menu.previewEdgeIndex = value; menu.save("previewEdgeIndex", value); menu.applyLive() } }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Bracket"); values: ["off", "0", "1", "2", "3"]; current: menu.bracket
                labelFor: function(v) { return v === "off" ? qsTr("No bracketing")
                                              : v === "0" ? qsTr("3 frames in a row, same exposure")
                                              : qsTr("3 frames, ±%1 EV").arg(v) }
                shortFor: function(v) { return v === "off" ? qsTr("off") : v === "0" ? "x3" : "±" + v }
                onPicked: { menu.bracket = value; menu.save("bracket", value) } }
            // The level's zero: "zeroed" once recorded (a long press on "level"
            // with the phone upright), "factory" clears it.
            MenuColumn { width: band.colW; header: header
                caption: qsTr("Level"); values: ["factory", "zeroed"]; current: menu.levelZeroed ? "zeroed" : "factory"
                labelFor: function(v) { return v === "zeroed" ? qsTr("Level zero: as recorded (hold \"level\" with the phone upright)") : qsTr("Level zero: the sensor's own") }
                shortFor: function(v) { return v === "zeroed" ? qsTr("zeroed") : qsTr("factory") }
                onPicked: { if (value === "factory") menu.clearLevelZero(); else if (!menu.levelZeroed) header.show(qsTr("Hold \"level\" with the phone upright to record the zero")) } }
            // Rule-of-thirds grid (its notch button became the level).
            MenuColumn { width: band.colW; header: header
                caption: qsTr("Grid"); values: [false, true]; current: menu.gridOn
                labelFor: function(v) { return v ? qsTr("Grid on") : qsTr("Grid off") }
                shortFor: function(v) { return v ? qsTr("on") : qsTr("off") }
                iconFor: function(v) { return v ? "icon-camera-grid-thirds" : "icon-camera-grid-none" }
                // The menu holds the state (the page reads it): loading it from the
                // settings file used to break a binding, so a second tap on "off"
                // was taken for no change and the grid stayed on.
                onPicked: { menu.gridOn = value; menu.save("gridOn", value ? 1 : 0) } }
            // Stacked exposures: more frames than the brightness needs, divided
            // out -- same exposure, noise divided by the square root of the
            // factor, at the price of the shot taking that many times longer.
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                caption: qsTr("Σ denoise"); values: [1, 2, 4]; current: menu.stackDenoise
                labelFor: function(v) { return v === 1 ? qsTr("Stacks: no extra frames") : qsTr("Stacks: %1× frames, %1× longer, noise ÷%2").arg(v).arg(Math.sqrt(v).toFixed(1).replace(".0", "")) }
                shortFor: function(v) { return v === 1 ? qsTr("off") : "÷" + Math.sqrt(v).toFixed(1).replace(".0", "") }
                onPicked: { menu.stackDenoise = value; menu.save("stackDenoise", value) } }
            // Focus calibration assistant. Focus (circle) on a target at one of
            // these distances, wait for the solid circle, then tap the distance:
            // the engine records "this distance = the lens value it has now" in
            // focus-calibration.json and applies the table at once. Repeat for
            // a few distances; the manual focus wheel is calibrated from then
            // on. The last entry forgets the assistant's points.
            MenuColumn { visible: menu.advanced && menu.focusCalibrationMenu; width: band.colW; header: header
                confirm: true                    // two taps: a calibration is not erased by mistake
                caption: qsTr("Focus cal."); values: [0.1, 0.25, 0.5, 1, 2, 5, "inf", "clear"]; current: null
                labelFor: function(v) { return v === "clear" ? qsTr("Forget the recorded points")
                                              : v === "inf" ? qsTr("Record: lens now = infinity")
                                              : qsTr("Record: lens now = %1 m (focus on it first)").arg(v) }
                shortFor: function(v) { return v === "inf" ? "\u221e" : v === "clear" ? qsTr("clear") : v + "m" }
                onPicked: { viewfinder.sendCommand("calib " + value); header.show(value === "clear" ? qsTr("Calibration points forgotten")
                                                                                 : qsTr("Recorded %1").arg(value === "inf" ? "\u221e" : value + " m")) } }
            // Focus peaking and zebra thresholds: no longer in the menu; the
            // % tweaks (peakTweak, zebraTweak) are read from rawfish-ir.conf, the
            // device values from override-hal.json (see the .example files).
            // Scenes, far right: grouped modes; with one on, the HAL drives
            // 3A and the atomic controls grey out (see the deck).
            // Scenes: as many columns of five as the HAL's list needs.
            // Shown only when Settings > RAWfish-ir > "Scenes in the camera menu"
            // is on (off by default: not yet implemented properly), in the
            // simple mode as in the advanced one.
            Repeater {
                model: menu.showScenes ? Math.ceil(menu.sceneList.length / 5) : 0
                MenuColumn { width: band.colW * 1.25; header: header
                    caption: index === 0 ? qsTr("Scene") : " "
                    values: menu.sceneChunk(index); current: menu.sceneMode
                    labelFor: function(v) { return menu.sceneNames[v] || qsTr("Scene %1").arg(v) }
                    shortFor: function(v) { return menu.sceneShort[v] || String(v) }
                    iconFor: function(v) { return menu.sceneIcons[v] || "" }
                    onPicked: { menu.sceneMode = value; menu.save("sceneMode", value); menu.applyLive() } }
            }
        }
    }
    // Position bar: as fine as the dots were; its length is the visible share
    // of the band, its place the scroll position.
    Item {
        id: bar
        anchors { bottom: parent.bottom; bottomMargin: Theme.paddingLarge; left: parent.left; right: parent.right
                  leftMargin: Theme.horizontalPageMargin; rightMargin: Theme.horizontalPageMargin }
        height: Math.max(2, Math.round(Theme.paddingSmall / 3))        // as fine as a hairline
        Rectangle { anchors.fill: parent; radius: height / 2; color: Theme.lightPrimaryColor; opacity: Theme.opacityLow }
        Rectangle {
            readonly property real share: band.contentWidth > 0 ? Math.min(1, band.width / band.contentWidth) : 1
            width: parent.width * share
            x: band.contentWidth > band.width ? (parent.width - width) * band.contentX / (band.contentWidth - band.width) : 0
            height: parent.height; radius: height / 2
            color: Theme.highlightColor; opacity: Theme.opacityHigh
        }
    }
}
