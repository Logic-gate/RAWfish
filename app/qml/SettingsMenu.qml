/*
 * SettingsMenu.qml -- the settings overlay, pulled down over the viewfinder,
 * in the Jolla Camera / RAWfish style: the image dims, a header label names
 * the value under the finger, and the settings stand as columns of round
 * items (icon or short text), four columns per page, pages side by side
 * with dots. Everything atomic: one column = one control.
 *
 * Columns, in this order (November 2026): mode, flash (if the camera has
 * one), self-timer, grid, format, resolution, JPEG quality, white balance
 * presets, bracketing, the JPG processing group (frames, colour noise,
 * moiré, sharpening: framed together, if the Settings page asks), level;
 * then, hidden unless the Settings page's switches are on, the focus
 * calibration and the scenes (grouped modes override the atomic controls).
 * The Σ denoise and the zebra and peaking tweaks are in rawfish-ir.conf only.
 * The band opens on its left end. The fine white balance lives on the
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
    property string captureSize: ""                 // the size in force, for the camera running
    // ---- The size chosen, per camera (0.3.8) ----
    //
    // Until 0.3.8 there was ONE saved size for the whole app, and a camera
    // change cleared it: a 50 MP chosen on camera 0 was lost on a visit to
    // the selfie, and at the next start the app opened camera 0 with the
    // size last saved -- often another camera's (the selfie's 3264x2448) --
    // then restarted it at its own a second later (log of 14 Nov: 1.4 s).
    // Now each camera keeps the size last in force on it (captureSizeMap:
    // camera id -> "WxH", "" = its default), the app starts camera 0 with
    // camera 0's, and a camera change starts the new camera with its own.
    // A stored size is still checked against the camera's list by the settle
    // below: one not offered any more (another format chosen meanwhile)
    // falls back to the default with one restart, as before.
    property string captureSizeMap: "{}"
    function sizeMapParse() { try { var m = JSON.parse(captureSizeMap); return (m && typeof m === "object") ? m : {} } catch (e) { return {} } }
    // The size kept for camera id ("" if none).
    function sizeForCamera(id) { var m = sizeMapParse(), v = m[String(id)]; return (v !== undefined && /^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$/.test(String(v))) ? String(v) : "" }
    // Keeps sz ("" = the default) for the camera running now.
    function storeSizeHere(sz) {
        if (currentCamera === "") return
        var m = sizeMapParse()
        if (sz === "") delete m[currentCamera]; else m[currentCamera] = sz
        captureSizeMap = JSON.stringify(m); save("captureSizeMap", captureSizeMap)
    }
    // Once per install: the 12.5 MP default of 0.3.1 applied over a size that
    // was only ever the old default. Until 0.3.0 the default was the largest
    // size, and settleSize wrote it into the settings as if chosen; so on an
    // update the new default never showed. At the first settle with this
    // version, a stored size equal to the camera's largest is taken for that
    // old default and cleared (whoever wants 50 MP chooses it again, once).
    property bool sizeDefaultApplied: false
    property string settleTried: ""                // the size a settle last restarted the engine for
    property string settleReported: ""
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
    // Default (0.3.8): the camera's high-quality noise reduction and
    // sharpening. Until then fast noise reduction and no sharpening, chosen
    // when a sharpened noisy preview turned into a maze at high ISO.
    property int previewNoiseIndex: 3      /* hq_noise (rawfish-ir.conf viewfinderNoise) */
    property int previewEdgeIndex: 3       /* hq_edge (rawfish-ir.conf viewfinderSharpen) */
    // The mode at start (Settings page): "last" = as left, else "simple" or "advanced".
    property string startMode: "last"
    // Bracketing: "off", "0" (three shots in a row at the same exposure, for a
    // short moment), or "1".."3" (three shots at -N, 0, +N stops).
    property string bracket: "off"
    // A stacked shutter (Σ) is one long exposure made of many frames: no
    // bracket with it. While Σ is chosen the bracket in force is "off" and
    // its column greyed; the chosen value is kept (saved as it is) and comes
    // back when the shutter leaves Σ, as the type does (typeBeforeStack).
    // Everything that sends or decides by the bracket reads effBracket.
    property bool stackSelected: false
    // The flash makes the photo fully automatic (0.3.8): in automatic
    // exposure the camera meters with a pre-flash, then fires with the
    // photo; with a manual pair there is nothing to meter and "on" fires a
    // lone flash. So while the flash is "on" or "auto" (and this camera has
    // one) the interface puts the exposure and the focus in automatic and
    // greys their manual controls (rawfish-ir.qml, flashAuto). A bracket is
    // three photos at chosen exposures: with an automatic exposure they
    // would be three identical photos, so it shows "off" and is greyed too
    // (its chosen value is kept and comes back, as with the Σ).
    readonly property bool flashAuto: (flash === "on" || flash === "auto") && !!(viewfinder && viewfinder.caps && viewfinder.caps.flash)
    readonly property string effBracket: (stackSelected || flashAuto) ? "off" : bracket
    property string flash: "off"                   // off | auto | on | torch (if the camera has a flash)
    property int stackDenoise: 1                   // stack: frames multiplied then divided out (1, 2, 4)
    // JPG: RAWfish-ir's own processing of the ISP's image (rawfish-ir.conf only).
    property string snapSize: ""                   // snap's JPEG size (WxH listed by the camera); "" = the largest 4:3 <= 5 Mpx
    property string snapActualSize: ""             // the size snap uses on this camera (from the page), for the menu
    property bool logChecks: false                 // the reports carry the viewfinder-vs-photo histograms (a check)
    property string zebraJpegMode: "luma"          // JPEG zebra: luma (a pixel that comes out white) or channel (any one)
    property int yuvFrames: 1                      // 1, 2 or 4 frames merged
    property int yuvColour: 2                      // colour filter: 0 off, 1..5 (1, 2, 3, 5, 8 x the noise)
    property int yuvMedian: 0                      // false colour: 0 off, 1..4 (a 3x3 .. 9x9 median)
    property int yuvSharpen: 3                     // our sharpening: 0 off, 1..5 (0.5 .. 4 x the detail)
    property int frameLatencyMs: 200               // measured HAL time between sub-frames (updated after each stack)
    property bool advanced: true                   // simple = jpg only, no wheels
    // The Σ preview's target frame rate (rawfish-ir.conf, hand-edited): the ISO is
    // raised so that one frame this short stands for the stack.
    property int sigmaPreviewFps: 10
    property int wbShift: 0                        // steps of 5 % red/blue from the AWB; 0 = AWB
    property int tint: 0                           // percent of green, - green .. + magenta
    // Simple mode layout, from ~/.config/com.rawfish/rawfish-ir/simple.json
    // (defaults below): what it hides, what it fixes, which menu columns stay.
    property var simple: ({ "hide": ["wheels", "histogram", "headers", "focus"],
                            "quality": 95, "noise": "minimal_noise", "edge": "hq_edge",
                            "menu": ["timer", "format", "wb"] })
    function simpleHides(what) { return !advanced && simple.hide && simple.hide.indexOf(what) >= 0 }
    function simpleMenuHas(col) { return advanced || (simple.menu && simple.menu.indexOf(col) >= 0) }
    property int sceneMode: 0                      // 0 = off (atomic controls)
    property bool showScenes: false                // the Settings page's switch (scenes in the menu)
    property bool processingColumns: false         // the Settings page's switch (JPG processing in the menu)
    // RAWfish-ir's own JPG processing applies to jpg alone, and not to a
    // bracket: its three photos are the camera's JPEG.
    readonly property bool yuvColumnsOff: imageType !== "jpg" || effBracket !== "off"
    readonly property string yuvColumnsHint: imageType === "snap" ? qsTr("snap: RAWfish-ir's processing at its defaults")
                                           : imageType !== "jpg" ? qsTr("JPG only: RAWfish-ir's processing")
                                                                  : qsTr("bracket: the camera's own JPEG, not RAWfish-ir's processing")
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
    // The camera's AWB modes without OFF (0): a manual white is set by the
    // shift, the tint or the pick, never by switching the AWB off here.
    readonly property var awbAvailable: viewfinder && viewfinder.caps.awbModes
                                        ? viewfinder.caps.awbModes.filter(function(m) { return Number(m) !== 0 }) : [1]
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
        sizeDefaultApplied = String(viewfinder.setting("sizeDefaultApplied", "0")) === "1"
        captureSizeMap = String(viewfinder.setting("captureSizeMap", "") || "{}")
        // The camera the app starts on (camera 0, rawfish-ir.qml) takes its own
        // size. With no map yet (0.3.7 or earlier), the one saved size goes
        // to camera 0: usually its own; if it was another camera's, the
        // settle restarts once, as it always did.
        var start = currentCamera !== "" ? currentCamera : "0"
        var sz = sizeForCamera(start)
        if (sz === "" && String(viewfinder.setting("captureSizeMap", "")) === "") {
            var legacy = String(viewfinder.setting("captureSize", ""))
            if (/^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$/.test(legacy)) {
                sz = legacy
                var m0 = {}; m0[start] = legacy; captureSizeMap = JSON.stringify(m0); save("captureSizeMap", captureSizeMap)
            }
        }
        captureSize = sz
        formatSaved = pick("format", "", ["4:3", "16:9"])   /* "" = the RAW's shape (format) */
        jpegQuality = pick("jpegQuality", "95", ["20", "40", "60", "80", "90", "95", "100"]) * 1
        // The menu's White bal. column starts on auto at each start of the app
        // (the mode chosen there lasted across starts and could be forgotten
        // on "daylight" or "tungsten"). Only this column: measured whites,
        // the temperature and tint shifts keep their behaviour.
        awbMode = 1; save("awbMode", 1)
        // Set in rawfish-ir.conf only (not in the menu): the processing.
        var ss = String(viewfinder.setting("snapSize", "")).trim()
        snapSize = /^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$/.test(ss) ? ss : ""
        logChecks = pick("logChecks", "false", ["false", "true"]) === "true"
        zebraJpegMode = pick("zebraJpegMode", "luma", ["luma", "channel"])
        noiseIndex = Math.max(0, noiseModes.indexOf(pick("halJpegNoise", "off", ["default", "off", "fast", "hq", "minimal"]) + "_noise"))
        edgeIndex = Math.max(0, edgeModes.indexOf(pick("halJpegSharpen", "hq", ["default", "off", "fast", "hq"]) + "_edge"))
        previewNoiseIndex = Math.max(0, noiseModes.indexOf(pick("viewfinderNoise", "hq", ["default", "off", "fast", "hq", "minimal"]) + "_noise"))
        previewEdgeIndex = Math.max(0, edgeModes.indexOf(pick("viewfinderSharpen", "hq", ["default", "off", "fast", "hq"]) + "_edge"))
        bracket = pick("bracket", "off", ["off", "0", "1", "2", "3"])
        yuvFrames = pick("jpgFrames", "1", ["1", "2", "4"]) * 1
        yuvColour = num("jpgNoiseColourFilter", 2, 0, 5)
        yuvMedian = num("jpgMoireFilter", 0, 0, 4)
        yuvSharpen = num("jpgSharpen", 3, 0, 5)
        stackDenoise = pick("stackDenoise", "1", ["1", "2", "4"]) * 1
        gridOn = num("gridOn", 0, 0, 1) === 1
        levelRoll0 = num("levelRoll0", 0, -45, 45)
        levelPitch0 = num("levelPitch0", 0, -45, 45)
        advanced = startMode === "simple" ? false : startMode === "advanced" ? true : num("advanced", 0, 0, 1) === 1   /* a new install opens in simple mode */
        sceneMode = num("sceneMode", 0, 0, 18)
        peakTweak = pick("peakTweak", "0", ["-30", "0", "30"]) * 1
        zebraTweak = pick("zebraTweak", "0", ["-30", "0", "30"]) * 1
        flash = pick("flash", "off", ["off", "auto", "on", "torch"])
        frameLatencyMs = num("frameLatencyMs", 200, 0, 5000)
        wbShift = num("wbShift", 0, -20, 20)
        wbLoad()
        sigmaPreviewFps = num("sigmaPreviewFps", 10, 1, 30)
        shadingPivot = num("shadingPivot", 50, 0, 100)
        shadingStrength = num("shadingStrength", 100, 0, 100)
        shadingMaxGain = num("shadingMaxGain", 5, 1, 8)
        fineWarmthStep = num("fineWarmthStep", 1, 1, 4)
        fineTintStep = num("fineTintStep", 5, 1, 25)
        hapticShutterMs = num("hapticShutter", 40, 0, 200)
        focusScanWaitMs = num("focusScanWait", 1000, 0, 3000)
        hapticTypeMs = num("hapticType", 20, 0, 100)
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
    // ---- The measured white (a long press on the kelvin or tint readout) ----
    //
    // A measured white is the gains R, Ge, Go, B that make the measured object
    // grey ON THE CAMERA THAT MEASURED IT: each camera's sensor sees colours
    // its own way, so those gains are not valid on another camera (the JP2's
    // camera 0 gains, sent to camera 1, gave a colour cast). Since 0.3.8:
    //  - each camera keeps its own measured white (wbFixedMap: camera id ->
    //    "R,Ge,Go,B"), applied exact on that camera;
    //  - the LAST white measured is also kept as an offset from its camera's
    //    automatic white at that moment (wbOffset: "red factor,blue factor",
    //    e.g. "1.18,0.91" = 18 % more red, 9 % less blue than automatic). A
    //    camera with no white of its own gets that offset on top of ITS
    //    automatic white ("wb offset"): a deliberate colour (a white measured
    //    on a coloured card) follows to every camera, approximately (each
    //    camera's automatic white leans its own way).
    // Kept across restarts (the app asks at start whether to keep them).
    // Before 0.3.8 one measured white ("wbFixed") was kept for all cameras;
    // it was measured on RAW, which only camera 0 had: it becomes camera
    // 0's (wbLoad).
    property string wbFixedMap: "{}"                  // JSON: { "0": "R,Ge,Go,B", ... }
    property string wbOffset: ""                       // "R,B" factors, "" when none
    readonly property string currentCamera: viewfinder ? String(viewfinder.cameraId || "") : ""
    // This camera's own measured white, "" when it has none.
    readonly property string wbFixed: {
        var m = wbParseMap(); return m[currentCamera] !== undefined ? String(m[currentCamera]) : ""
    }
    // The offset applies here: a white was measured elsewhere, none here.
    readonly property bool wbOffsetHere: wbFixed === "" && wbOffset !== ""
    function wbParseMap() { try { var m = JSON.parse(wbFixedMap); return (m && typeof m === "object") ? m : {} } catch (e) { return {} } }
    function wbLoad() {
        wbFixedMap = String(viewfinder.setting("wbFixedMap", "") || "")
        if (wbFixedMap === "") wbFixedMap = "{}"
        wbOffset = String(viewfinder.setting("wbOffset", "") || "")
        var legacy = String(viewfinder.setting("wbFixed", "") || "")
        if (legacy !== "") {                            // a white kept by 0.3.7 or earlier: camera 0's
            var m = wbParseMap()
            if (m["0"] === undefined) m["0"] = legacy
            wbFixedMap = JSON.stringify(m); save("wbFixedMap", wbFixedMap); save("wbFixed", "")
        }
    }
    // A white just measured on this camera: gains "R,Ge,Go,B", and awb the
    // camera's automatic white at that moment (from the engine's event).
    function setWbFixed(gains, awb) {
        var m = wbParseMap(), g = String(gains).split(","), a = String(awb || "").split(",")
        m[currentCamera] = String(gains)
        wbFixedMap = JSON.stringify(m); save("wbFixedMap", wbFixedMap)
        // The offset: the measured white's red and blue against the
        // automatic one, each relative to its green (green stays the
        // reference). Without the automatic white (an old engine), no offset.
        if (g.length === 4 && a.length === 4 && Number(a[0]) > 0 && Number(a[3]) > 0 && Number(g[1]) > 0 && Number(a[1]) > 0) {
            var fr = (Number(g[0]) / Number(g[1])) / (Number(a[0]) / Number(a[1]))
            var fb = (Number(g[3]) / Number(g[1])) / (Number(a[3]) / Number(a[1]))
            wbOffset = fr.toFixed(4) + "," + fb.toFixed(4)
        } else wbOffset = ""
        save("wbOffset", wbOffset)
        wbShift = 0; tint = 0; save("wbShift", 0); save("tint", 0)
        applyLive()
    }
    // Not on the camera's automatic white: a measured white (here or carried),
    // a shift or tint, or a preset mode. The start-up banner offers to keep it
    // or drop it all.
    readonly property bool wbManualAny: wbFixed !== "" || wbOffsetHere || wbShift !== 0 || tint !== 0 || awbMode !== 1
    // Back to automatic drops EVERY camera's measured white and the offset:
    // one gesture, the automatic white everywhere.
    function wbDropMeasured() {
        wbFixedMap = "{}"; save("wbFixedMap", wbFixedMap)
        wbOffset = ""; save("wbOffset", "")
        if (viewfinder && viewfinder.running) { viewfinder.sendCommand("wb fixed off"); viewfinder.sendCommand("wb offset off") }
    }
    function resetWbAll() {
        wbDropMeasured()
        wbShift = 0; tint = 0; save("wbShift", 0); save("tint", 0)
        if (awbMode !== 1) { awbMode = 1; save("awbMode", 1) }
        applyLive()                                    // the mode, then shift 0: the camera's AWB again
    }
    function clearWbFixed() {
        wbDropMeasured()
        wbShift = 0; tint = 0; save("wbShift", 0); save("tint", 0)
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
    // Vibrations (rawfish-ir.conf): their length in milliseconds, 0 = none.
    // At the release, and at a change of type (a very short tick).
    property int hapticShutterMs: 40
    // How long a photo may wait for a continuous-AF search in progress at the
    // release (rawfish-ir.conf focusScanWait, ms; 0 = never): 1 s by default.
    property int focusScanWaitMs: 1000
    property int hapticTypeMs: 20               // 8 ms did not move the JP2's motor
    // A shift or tint step sends only the shift: the rest of the live settings
    // did not change (six commands and their replies per step filled the log).
    function applyShift() { if (viewfinder && viewfinder.running && advanced) viewfinder.sendCommand("wb shift=" + wbShift + " tint=" + tint) }
    function resetWarmth() { wbShift = 0; save("wbShift", 0); applyShift() }
    function resetTint() { tint = 0; save("tint", 0); applyShift() }
    function stepWarmth(d) { wbShift = Math.max(-20, Math.min(20, wbShift + d * fineWarmthStep)); save("wbShift", wbShift); applyShift() }
    function stepTint(d) { tint = Math.max(-100, Math.min(100, tint + d * fineTintStep)); save("tint", tint); applyShift() }
    // The noise and edge options a HAL JPEG / burst command carries.
    function photoProcessing() {
        return " " + noiseModes[noiseIndex] + " " + edgeModes[edgeIndex]
    }
    // The same two modes as "noise=... edge=..." fields (the controller's "sh").
    // The HAL JPEG's processing, from rawfish-ir.conf (halJpegNoise,
    // halJpegSharpen); stacks, brackets and bursts use it too.
    function photoProcessingFields() {
        return " noise=" + noiseModes[noiseIndex] + " edge=" + edgeModes[edgeIndex]
    }
    function applyLive() {
        if (!viewfinder || !viewfinder.running) return
        viewfinder.sendCommand("focuswait " + focusScanWaitMs)
        // The preview's own processing (the photos' goes with each shot:
        // photoProcessing()).
        viewfinder.sendCommand("preview " + noiseModes[previewNoiseIndex] + " " + edgeModes[previewEdgeIndex])
        viewfinder.sendCommand("shading balance=" + shadingPivot + " strength=" + shadingStrength + " maxgain=" + shadingMaxGain)
        // A measured white first: it is the base the mode and the shifts keep.
        // This camera's own if it has one, else the offset of a white measured
        // on another camera (see wbFixedMap). A new session starts with
        // neither in the engine, so nothing needs clearing here.
        if (advanced && wbFixed !== "") viewfinder.sendCommand("wb fixed " + wbFixed)
        else if (advanced && wbOffset !== "") viewfinder.sendCommand("wb offset " + wbOffset)
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
    // The size in force: the one chosen if this camera and format list it,
    // else the default: the largest size of at most 13 megapixels (12.5 on
    // the JP2: its 50 MP binned by four, the size its lens resolves and the
    // one that keeps the viewfinder and the files light), or the smallest
    // listed if every size is larger. Until 0.3.0 the default was the largest.
    function defaultSize(l) {
        for (var i = 0; i < l.length; ++i) {           // largest first
            var p = String(l[i]).split("x")
            if (p[0] * p[1] <= 13000000) return l[i]
        }
        return l.length > 0 ? l[l.length - 1] : ""
    }
    function effectiveSize() {
        var l = sizesFor(format)
        return l.indexOf(captureSize) >= 0 ? captureSize : defaultSize(l)
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
    onImageTypeChanged: settleTimer.restart()
    function setSize(sz) {
        captureSize = sz; save("captureSize", sz)
        storeSizeHere(sz)                                /* this camera's own from now on */
        if (sz !== "") save(format === "16:9" ? "captureSize169" : "captureSize43", sz)   /* the format's own */
        captureSizeChosen(sz)
    }
    // The size the format implies for THIS camera, once its sizes are known:
    // restart only if the engine is not already running at it (it resolves
    // "max" itself; a new camera's largest size of the format usually is).
    function settleSize() {
        if (jpegSizes.length === 0) return
        if (!sizeDefaultApplied) {
            var largest = sizesFor(format)[0]
            sizeDefaultApplied = true; save("sizeDefaultApplied", 1)
            viewfinder.sendCommand("# size default (once): stored \"" + captureSize + "\", largest " + largest
                                   + (captureSize !== "" && captureSize === largest ? ": taken for the old default, cleared" : ": kept"))
            if (captureSize !== "" && captureSize === largest) {
                captureSize = ""; save("captureSize", ""); storeSizeHere("")
                save(format === "16:9" ? "captureSize169" : "captureSize43", "")
            }
        }
        var e = effectiveSize()
        // The engine must run at the size in force. captureSize "" (nothing
        // chosen, or forgotten on a camera change) starts it on "max": the
        // default (12.5 MP) then differs from what runs, and a restart follows.
        // (0.3.4 returned when e equalled captureSize, and with captureSize ""
        // the menu showed 12.5 MP while the engine stayed at 50.)
        if (e === captureSize && e === runningSize) return
        // One restart per size: if the engine comes back at another size
        // anyway (it fell back to "max" after a failure), say so and stop,
        // rather than restart at every caps.
        if (e === settleTried && e !== runningSize) {
            if (settleTried !== settleReported) { settleReported = settleTried; viewfinder.sendCommand("# settle: " + e + " asked, the engine runs " + runningSize + " (left so)") }
            return
        }
        if (e !== runningSize) settleTried = e
        viewfinder.sendCommand("# settle: format " + format + " wants " + e + ", engine runs " + runningSize
                               + (e === runningSize ? " (kept)" : " (restart)"))
        if (e === runningSize) { captureSize = e; save("captureSize", e); storeSizeHere(e) }   /* already so: no restart */
        else setSize(e)
    }
    // Deferred: inside onJpegSizesChanged the other bindings on the new caps
    // (runningSize, sensorFormat) are not re-evaluated yet -- the JP2 log
    // showed 'engine runs 4096x3072' (the previous camera's size) for a
    // camera just opened at 6560x4928, hence a second engine start on every
    // switch (ui 170). It runs once the bindings have settled: a zero-delay
    // timer (Qt.callLater is Qt 5.8; this works on any Sailfish OS), restarted
    // by each trigger, so several triggers in a row make one settle.
    Timer { id: settleTimer; interval: 0; repeat: false; onTriggered: menu.settleSize() }
    onJpegSizesChanged: settleTimer.restart()
    // And at every caps (each engine start): a start on the same camera brings
    // the same list, and a list that does not change sends no signal.
    Connections { target: viewfinder; onCapsChanged: settleTimer.restart() }

    // The band always opens on its left end (Mode first): it is wider than
    // the screen, and it used to reopen wherever it had been left.
    onVisibleChanged: if (visible) { band.cancelFlick(); band.contentX = 0 }

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
            MenuColumn { width: band.colW; header: header
                caption: qsTr("Mode"); values: [false, true]; current: menu.advanced
                labelFor: function(v) { return v ? qsTr("Advanced: wheels, raw, every control") : qsTr("Simple: JPEG, automatic") }
                shortFor: function(v) { return v ? qsTr("adv") : qsTr("simp") }
                onPicked: { menu.advanced = value; menu.save("advanced", value ? 1 : 0) } }
            // Flash: off / auto / on (fires with the photo; automatic exposure
            // meters with a pre-flash first, ~0.5-1 s more) / torch (continuous).
            // Right after Mode, in simple mode too.
            MenuColumn { visible: !!(menu.viewfinder && menu.viewfinder.caps.flash)
                width: band.colW; header: header
                caption: qsTr("Flash"); values: ["off", "auto", "on", "torch"]; current: menu.flash
                labelFor: function(v) { return v === "off" ? qsTr("Flash off") : v === "auto" ? qsTr("Flash when dark")
                                              : v === "on" ? qsTr("Flash always") : qsTr("Torch (continuous light)") }
                shortFor: function(v) { return v === "torch" ? qsTr("torch") : v }
                iconFor: function(v) { return v === "off" ? "icon-camera-flash-off" : v === "auto" ? "icon-camera-flash-automatic"
                                              : v === "on" ? "icon-camera-flash-on" : "" }
                onPicked: { menu.flash = value; menu.save("flash", value); if (viewfinder.running && viewfinder.caps.flash) viewfinder.sendCommand("flash " + value) } }
            MenuColumn { visible: menu.simpleMenuHas("timer"); width: band.colW; header: header
                caption: qsTr("Timer"); values: [0, 3, 10]; current: menu.timerSec
                labelFor: function(v) { return v === 0 ? qsTr("No timer") : qsTr("%1 second timer").arg(v) }
                shortFor: function(v) { return v === 0 ? qsTr("off") : v + "s" }
                iconFor: function(v) { return v === 0 ? "icon-camera-timer" : "icon-camera-timer-" + v + "s" }
                onPicked: { menu.timerSec = value; menu.save("timerSec", value) } }
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
            // Format, resolution and quality belong to the JPEG: in RAW the DNG
            // is the sensor's, so the three are greyed out (a tap says so).
            MenuColumn { visible: menu.simpleMenuHas("format"); width: band.colW; header: header
                dimmed: menu.imageType === "raw" || menu.imageType === "snap"; dimmedHint: menu.imageType === "snap" ? qsTr("snap: one light size, for utility shots") : qsTr("RAW has one size and one format: the sensor's")
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
                dimmed: menu.imageType === "raw" || menu.imageType === "snap"; dimmedHint: menu.imageType === "snap" ? qsTr("snap: one light size, for utility shots") : qsTr("RAW has one size and one format: the sensor's")
                caption: qsTr("Resolution")
                // snap shows its own size (greyed), not the JPG's last one
                values: menu.imageType === "snap" && menu.snapActualSize !== "" ? [menu.snapActualSize] : menu.sizesFor(menu.format)
                current: menu.imageType === "snap" && menu.snapActualSize !== "" ? menu.snapActualSize : menu.effectiveSize()
                labelFor: function(v) { return v }
                shortFor: function(v) { return menu.mp(v) }
                onPicked: menu.setSize(value) }
            MenuColumn { visible: menu.advanced; width: band.colW; header: header
                dimmed: menu.imageType === "raw"; dimmedHint: qsTr("RAW: no JPEG quality")
                caption: qsTr("Quality"); values: [20, 40, 60, 80, 90, 95, 100]; current: menu.jpegQuality
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
                dimmed: menu.imageType === "snap" || menu.stackSelected || menu.flashAuto
                dimmedHint: menu.stackSelected ? qsTr("Σ: one long exposure, no bracketing")
                          : menu.flashAuto ? qsTr("Flash: no bracketing")
                          : qsTr("snap: one shot, no bracketing")
                caption: qsTr("Bracket"); values: ["off", "0", "1", "2", "3"]; current: menu.effBracket
                labelFor: function(v) { return v === "off" ? qsTr("No bracketing")
                                              : v === "0" ? qsTr("3 frames in a row, same exposure")
                                              : qsTr("3 frames, ±%1 EV").arg(v) }
                shortFor: function(v) { return v === "off" ? qsTr("off") : v === "0" ? "x3" : "±" + v }
                onPicked: { menu.bracket = value; menu.save("bracket", value) } }
            // RAWfish-ir's JPG processing, when Settings > Camera menu asks for
            // it (otherwise in rawfish-ir.conf only). Saved to the same keys.
            // The four columns are one group, set off by a faint frame with its
            // name under them, and greyed together (yuvColumnsOff): they act on
            // our JPG only, not on RAW, snap, Σ or the camera's own JPEG.
            Item {
                id: jpgGroup
                visible: menu.advanced && menu.processingColumns
                width: jpgRow.width; height: jpgRow.height + jpgGroupName.height + Theme.paddingSmall
                Rectangle {                                  // the frame, a little wider than the columns
                    anchors { fill: parent; leftMargin: -Theme.paddingSmall; rightMargin: -Theme.paddingSmall }   // (not above: the band clips there)
                    radius: Theme.paddingMedium
                    color: Theme.highlightBackgroundColor; opacity: menu.yuvColumnsOff ? 0.06 : 0.15
                    border { width: 1; color: Theme.highlightColor }
                }
                Row {
                    id: jpgRow
                    spacing: Theme.paddingMedium
                MenuColumn { width: band.colW; header: header
                    defaultValue: 1
                    dimmed: menu.yuvColumnsOff; dimmedHint: menu.yuvColumnsHint
                    caption: qsTr("Frames"); values: [1, 2, 4]; current: menu.yuvFrames
                    labelFor: function(v) { return v === 1 ? qsTr("One frame") : qsTr("%1 frames merged (less grain)").arg(v) }
                    onPicked: { menu.yuvFrames = value; menu.save("jpgFrames", value) } }
                MenuColumn { width: band.colW; header: header
                    defaultValue: 2
                    dimmed: menu.yuvColumnsOff; dimmedHint: menu.yuvColumnsHint
                    caption: qsTr("Colour NR"); values: [0, 1, 2, 3, 4, 5]; current: menu.yuvColour
                    labelFor: function(v) { return v === 0 ? qsTr("Colour noise filter off") : qsTr("Colour noise filter: %1 x the noise").arg([0, 1, 2, 3, 5, 8][v]) }
                    shortFor: function(v) { return v === 0 ? qsTr("off") : String(v) }
                    onPicked: { menu.yuvColour = value; menu.save("jpgNoiseColourFilter", value) } }
                MenuColumn { width: band.colW; header: header
                    defaultValue: 0
                    dimmed: menu.yuvColumnsOff; dimmedHint: menu.yuvColumnsHint
                    caption: qsTr("Moiré"); values: [0, 1, 2, 3, 4]; current: menu.yuvMedian
                    labelFor: function(v) { return v === 0 ? qsTr("False colour: nothing removed") : qsTr("False colour: a %1 median on the colour").arg(["", "3×3", "5×5", "7×7", "9×9"][v]) }
                    shortFor: function(v) { return v === 0 ? qsTr("off") : ["", "3×3", "5×5", "7×7", "9×9"][v] }
                    onPicked: { menu.yuvMedian = value; menu.save("jpgMoireFilter", value) } }
                MenuColumn { width: band.colW; header: header
                    defaultValue: 3
                    dimmed: menu.yuvColumnsOff; dimmedHint: menu.yuvColumnsHint
                    caption: qsTr("Sharpen"); values: [0, 1, 2, 3, 4, 5]; current: menu.yuvSharpen
                    labelFor: function(v) { return v === 0 ? qsTr("RAWfish sharpening off") : qsTr("RAWfish sharpening: %1 x the fine detail").arg([0, 0.5, 1, 1.5, 2.5, 4][v]) }
                    shortFor: function(v) { return v === 0 ? qsTr("off") : String(v) }
                    onPicked: { menu.yuvSharpen = value; menu.save("jpgSharpen", value) } }
                }
                Label {                                      // the group's name, under its columns
                    id: jpgGroupName
                    anchors { top: jpgRow.bottom; horizontalCenter: parent.horizontalCenter }
                    text: qsTr("JPG processing")
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.highlightColor; opacity: menu.yuvColumnsOff ? Theme.opacityLow : 1.0
                }
            }
            // The level's zero: "zeroed" once recorded (a long press on "level"
            // with the phone upright), "factory" clears it.
            MenuColumn { width: band.colW; header: header
                caption: qsTr("Level"); values: ["factory", "zeroed"]; current: menu.levelZeroed ? "zeroed" : "factory"
                labelFor: function(v) { return v === "zeroed" ? qsTr("Level zero: as recorded (hold \"level\" with the phone upright)") : qsTr("Level zero: the sensor's own") }
                shortFor: function(v) { return v === "zeroed" ? qsTr("zeroed") : qsTr("factory") }
                onPicked: { if (value === "factory") menu.clearLevelZero(); else if (!menu.levelZeroed) header.show(qsTr("Hold \"level\" with the phone upright to record the zero")) } }
            // Focus calibration assistant. Focus (circle) on a target at one of
            // these distances, wait for the solid circle, then tap the distance:
            // the engine records "this distance = the lens value it has now" in
            // focus-calibration.json and applies the table at once. Repeat for
            // a few distances; the manual focus wheel is calibrated from then
            // on. The last entry forgets the assistant's points.
            // 0.08 m: below the autofocus' own limit on the JP2's camera 0
            // (~9 cm), within the manual focus' reach (~7 cm): without a point
            // there the distances under 10 cm were extrapolated (the stop
            // shown, 6.8 cm, too). Its focus is set by hand (volume keys),
            // not by a touch.
            MenuColumn { visible: menu.advanced && menu.focusCalibrationMenu; width: band.colW; header: header
                confirm: true                    // two taps: a calibration is not erased by mistake
                caption: qsTr("Focus cal."); values: [0.08, 0.1, 0.25, 0.5, 1, 2, 5, "inf", "clear"]; current: null
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
