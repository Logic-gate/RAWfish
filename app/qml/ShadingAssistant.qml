/*
 * ShadingAssistant.qml -- measure a lens-shading (vignetting) calibration on
 * the phone, launched from Settings > RAWfish-ir ("Calibrate the lens shading"):
 * the request flag is set there, and the camera page pushes this page at
 * its next start. The camera and its engine keep running underneath; this
 * page only drives the engine's 'shading calibrate' command and shows what
 * it reports (cal_progress, cal_exposure, cal_done), see docs/LENS-SHADING.md.
 *
 * Steps (one column, the current step's controls shown):
 *   1 Prepare   -- what is needed; full calibration or add a light; the
 *                  light's kind (its colour temperature, the file's key).
 *   2 Measure   -- the engine settles the exposure itself, walks the lens
 *                  positions (full) or one (add a light), 3 RAW each; a bar.
 *   3 Result    -- written where (the user's device-profiles/lens-shading/),
 *                  or why it failed; a before/after look at a flat field.
 * Nothing here changes the camera page; leaving pops back to it, which uses
 * the new file at its next engine start (restart()).
 *
 * Which camera (0.3.8): the map corrects DNG files only, so only a camera
 * that gives RAW can be calibrated. The assistant opens over the camera
 * running at the start, which may have none (the JP2's front and wide
 * cameras). The caps list which cameras give RAW (raw_cams, read from every
 * camera's characteristics without opening them):
 *   - none: the assistant says so and offers no measurement;
 *   - one: it switches to that camera by itself and names it ("Camera 0");
 *   - several: step 1 offers a choice; picking one switches to it.
 * The camera is named by its number, as the interface's camera button does.
 *
 * The lens positions are the HAL's own values (no focus calibration), spread
 * over the lens's range: 0 = infinity, then up to the closest position the
 * HAL reports in caps (min_focus_diopters) -- six positions, as measured on
 * the JP2 the falloff only moves at the near end.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Page {
    id: assistant
    property var viewfinder                     // the camera page's IndrawPreview
    property var cameraPage
    allowedOrientations: Orientation.Portrait

    // --- state -------------------------------------------------------------
    property int step: 1                        // 1 prepare, 2 measure, 3 result
    property bool full: true                    // full calibration vs add a light
    property int lightIndex: 0                  // index into lights
    readonly property var lights: [
        { name: qsTr("Daylight"),            cct: 5500 },
        { name: qsTr("Filament bulb"),       cct: 3000 },
        { name: qsTr("LED / fluorescent"),   cct: 4200 },
        { name: qsTr("Cloudy / shade"),      cct: 6500 }
    ]
    property int done: 0
    property int total: 0
    property bool running: false
    property bool finished: false
    property bool ok: false
    property string resultPath: ""
    property string failWhy: ""
    property string exposureText: ""
    // the flat field used for the before/after look: a DNG the user takes
    // from the camera page later; here we only show the thumbnail the engine
    // writes for the LAST capture of the calibration when it is kept
    property string previewPath: ""

    // --- which camera: those that give RAW ---------------------------------
    // caps.cameras: every camera id; caps.rawCams: 1 / 0 for each, same order.
    // Unknown (an engine before 0.3.8): every camera assumed to have RAW, as
    // before (the measurement then fails by itself on one without).
    readonly property var rawCameras: {
        var c = viewfinder && viewfinder.caps ? viewfinder.caps : {}, ids = c.cameras || [], r = c.rawCams, out = []
        for (var i = 0; i < ids.length; ++i) if (r === undefined || r.length !== ids.length || r[i] === 1) out.push(String(ids[i]))
        return out
    }
    readonly property string cameraHere: viewfinder ? String(viewfinder.cameraId) : ""
    readonly property bool hereHasRaw: rawCameras.indexOf(cameraHere) >= 0
    // The engine restarts when the camera changes: measuring waits for it.
    readonly property bool ready: hereHasRaw && viewfinder && viewfinder.running
    // One camera with RAW and the assistant opened on another: switch to it.
    function settleCamera() {
        if (!viewfinder || hereHasRaw || rawCameras.length !== 1) return
        viewfinder.cameraId = rawCameras[0]
    }
    Component.onCompleted: settleCamera()
    onRawCamerasChanged: settleCamera()          // the caps may come after the page

    // --- lens positions: 6 over the HAL's range ---------------------------
    function lensList() {
        var near = viewfinder && viewfinder.caps && viewfinder.caps.min_focus_diopters > 0
                   ? viewfinder.caps.min_focus_diopters : 15
        var l = [0]
        // geometric-ish spacing: dense near infinity (where it changes least)
        // would waste points; spread evenly in the HAL's value
        for (var i = 1; i < 6; ++i) l.push(Math.round(near * i / 5 * 10) / 10)
        return l.join(",")
    }

    // --- engine events ---------------------------------------------------
    Connections {
        target: viewfinder
        onCalProgress: { assistant.done = done; assistant.total = total }
        onCalExposure: assistant.exposureText = qsTr("Exposure set: 1/%1 s, ISO %2").arg(Math.round(1e9 / exposureNs)).arg(iso)
        onCalDone: {
            assistant.running = false
            assistant.finished = true
            assistant.ok = ok
            assistant.resultPath = path
            assistant.failWhy = why
            assistant.step = 3
        }
    }

    function start() {
        done = 0; total = 0; finished = false; ok = false; resultPath = ""; failWhy = ""; exposureText = ""
        running = true
        step = 2
        var cct = lights[lightIndex].cct
        if (full) viewfinder.sendCommand("shading calibrate full cct=" + cct + " lens=" + lensList())
        else viewfinder.sendCommand("shading calibrate light cct=" + cct)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader { title: qsTr("Lens shading calibration") }

            // ---------------- step 1: prepare ----------------
            Column {
                width: parent.width
                visible: assistant.step === 1
                spacing: Theme.paddingMedium

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeSmall
                    text: qsTr("Measures how the lens darkens the corners, so that every DNG can be corrected.\n\n" +
                               "You need: a white translucent diffuser held flat against the lens (a sheet of white " +
                               "plastic or paper), an even and steady light, and the phone held still.\n\n" +
                               "The light, the diffuser and the framing must not change during the whole measurement, " +
                               "nor between the captures: only the lens moves, by itself.")
                }

                // The camera being calibrated: named, or chosen among several.
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: assistant.rawCameras.length === 0 ? "#ff8080" : Theme.highlightColor
                    visible: assistant.rawCameras.length !== 1 || assistant.hereHasRaw
                    text: assistant.rawCameras.length === 0
                          ? qsTr("No camera of this phone gives RAW: the map corrects DNG files only, there is nothing to calibrate.")
                          : assistant.rawCameras.length === 1
                            ? qsTr("Camera %1: the only camera of this phone that gives RAW (the map corrects DNG files only).").arg(assistant.cameraHere)
                            : qsTr("Several cameras give RAW: choose the one to calibrate (the map corrects DNG files only).")
                }
                ComboBox {
                    label: qsTr("Camera")
                    visible: assistant.rawCameras.length > 1
                    currentIndex: Math.max(0, assistant.rawCameras.indexOf(assistant.cameraHere))
                    menu: ContextMenu {
                        Repeater {
                            model: assistant.rawCameras
                            MenuItem { text: qsTr("Camera %1").arg(modelData) }
                        }
                    }
                    onCurrentIndexChanged: if (assistant.viewfinder && assistant.rawCameras[currentIndex] !== undefined &&
                                               assistant.rawCameras[currentIndex] !== assistant.cameraHere)
                                               assistant.viewfinder.cameraId = assistant.rawCameras[currentIndex]
                }

                ComboBox {
                    label: qsTr("What to measure")
                    currentIndex: assistant.full ? 0 : 1
                    menu: ContextMenu {
                        MenuItem { text: qsTr("A full calibration (brightness at 6 lens positions + this light's colour)") }
                        MenuItem { text: qsTr("Add this light to the existing calibration (colour only)") }
                    }
                    onCurrentIndexChanged: assistant.full = currentIndex === 0
                }

                ComboBox {
                    label: qsTr("The light in front of the lens")
                    currentIndex: assistant.lightIndex
                    menu: ContextMenu {
                        Repeater {
                            model: assistant.lights
                            MenuItem { text: modelData.name + "  (" + modelData.cct + " K)" }
                        }
                    }
                    onCurrentIndexChanged: assistant.lightIndex = currentIndex
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryHighlightColor
                    text: assistant.full
                          ? qsTr("About one minute: %1 captures. The exposure is set automatically; the camera refuses a clipped, too dark, or uneven field.").arg(18)
                          : qsTr("A few seconds: 3 captures at one lens position, added to the file already measured.")
                }

                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: assistant.hereHasRaw && !assistant.ready ? qsTr("Starting camera %1…").arg(assistant.rawCameras.length === 1 ? assistant.rawCameras[0] : assistant.cameraHere)
                                                                   : qsTr("Start the measurement")
                    visible: assistant.rawCameras.length > 0
                    enabled: assistant.ready
                    onClicked: assistant.start()
                }
            }

            // ---------------- step 2: measuring ----------------
            Column {
                width: parent.width
                visible: assistant.step === 2
                spacing: Theme.paddingMedium

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    color: Theme.highlightColor
                    text: qsTr("Measuring… keep everything still.")
                }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeSmall
                    text: assistant.exposureText
                    visible: assistant.exposureText !== ""
                }
                ProgressBar {
                    width: parent.width
                    minimumValue: 0
                    maximumValue: assistant.total > 0 ? assistant.total : 1
                    value: assistant.done
                    indeterminate: assistant.total === 0
                    label: assistant.total > 0 ? qsTr("%1 / %2 captures").arg(assistant.done).arg(assistant.total) : qsTr("Setting the exposure…")
                }
            }

            // ---------------- step 3: result ----------------
            Column {
                width: parent.width
                visible: assistant.step === 3
                spacing: Theme.paddingMedium

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    color: assistant.ok ? Theme.highlightColor : Theme.errorColor
                    text: assistant.ok
                          ? (assistant.full ? qsTr("Calibration measured and saved.") : qsTr("Light added to the calibration."))
                          : qsTr("The measurement failed: %1").arg(assistant.failWhy)
                }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                    visible: assistant.ok
                    text: assistant.resultPath
                }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeSmall
                    visible: assistant.ok
                    text: qsTr("RAWfish-ir will use it from its next start. To see the effect: take a DNG of the flat field " +
                               "from the camera and look at it in the gallery — the corners come out as bright as the centre.\n\n" +
                               "A previous calibration, if any, was kept in the previous/ folder.")
                }
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeSmall
                    visible: !assistant.ok
                    text: qsTr("Check the diffuser covers the whole lens, add light, and try again.")
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.paddingLarge
                    Button {
                        text: qsTr("Try again")
                        visible: !assistant.ok
                        onClicked: assistant.step = 1
                    }
                    Button {
                        text: qsTr("Back to the camera")
                        onClicked: {
                            if (assistant.ok && viewfinder) viewfinder.restart()   // load the new file
                            pageStack.pop()
                        }
                    }
                }
            }
        }
        VerticalScrollDecorator {}
    }
}
