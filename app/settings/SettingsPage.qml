/*
 * SettingsPage.qml -- RAWfish-ir's page in the system Settings app (Settings >
 * Apps > RAWfish-ir), the way RAWfish has one. It holds what does not belong in
 * the camera's own menus: where captures go and how they are named. Values
 * live in dconf under /apps/rawfish-ir/output and the app reads them at every
 * engine start (Nemo.Configuration on both sides).
 *
 * Folders must be under ~/Pictures: that is the only place the sandboxed app
 * may write; the page refuses anything else rather than let photos be lost.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.Configuration 1.0

Page {
    id: page

    ConfigurationGroup {
        id: output
        path: "/apps/rawfish-ir/output"
        property string prefix: "photo"
        property string jpgDir: "~/Pictures/RAWfish-ir"
        property string dngDir: "~/Pictures/RAWfish-ir"
    }

    // Tuning helpers: the calibration column in the camera's menu, and a
    // request for the app to write the device profile files at its next start
    // (the Settings app itself cannot run the engine).
    ConfigurationGroup {
        id: menuGroup
        path: "/apps/rawfish-ir/menu"
        property bool previewColumns: false
        property string startMode: "last"
        property bool showScenes: false
        property bool startManual: false
    }
    ConfigurationGroup {
        id: device
        path: "/apps/rawfish-ir/device"
        property bool focusCalibrationMenu: false
        property bool probeRequested: false
        property string probeDone: ""        // the app writes the date when done
        property bool shadingCalRequested: false   // the lens-shading assistant, at the next start
    }

    readonly property string picturesHome: StandardPaths.pictures     // "/home/<user>/Pictures"
    function folderOk(f) {
        var t = f.trim()
        if (t.indexOf("~/Pictures") === 0) return true
        return t.indexOf(picturesHome) === 0
    }
    function prefixOk(p) { return /^[A-Za-z0-9_-]{1,32}$/.test(p) }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: "RAWfish-ir" }

            SectionHeader { text: qsTr("Captures") }

            TextField {
                id: prefixField
                width: parent.width
                label: qsTr("File name prefix")
                placeholderText: "photo"
                text: output.prefix
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                errorHighlight: !page.prefixOk(text)
                description: qsTr("Letters, digits, - and _ (up to 32). Files are named PREFIX_YYYYMMDD_HHMMSS_mmm.EXT.")
                onTextChanged: if (page.prefixOk(text)) output.prefix = text
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }

            TextField {
                id: jpgField
                width: parent.width
                label: qsTr("JPEG folder")
                placeholderText: "~/Pictures/RAWfish-ir"
                text: output.jpgDir
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                errorHighlight: !page.folderOk(text)
                description: qsTr("Under ~/Pictures (the only place the app may write).")
                onTextChanged: if (page.folderOk(text)) output.jpgDir = text.trim()
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }

            TextField {
                id: dngField
                width: parent.width
                label: qsTr("DNG folder")
                placeholderText: "~/Pictures/RAWfish-ir"
                text: output.dngDir
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                errorHighlight: !page.folderOk(text)
                description: qsTr("May be another folder than the JPEGs. DNG previews go to its hidden .previews/ subfolder.")
                onTextChanged: if (page.folderOk(text)) output.dngDir = text.trim()
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Reset to defaults")
                onClicked: {
                    output.prefix = "photo"; output.jpgDir = "~/Pictures/RAWfish-ir"; output.dngDir = "~/Pictures/RAWfish-ir"
                    prefixField.text = output.prefix; jpgField.text = output.jpgDir; dngField.text = output.dngDir
                }
            }

            SectionHeader { text: qsTr("Camera menu") }

            TextSwitch {
                text: qsTr("Scenes in the camera menu (not yet implemented)")
                description: qsTr("The camera's scene modes (night, sport…) as a menu column, in simple and advanced mode. With a scene on, the camera drives exposure, focus and white balance itself.")
                checked: menuGroup.showScenes
                onCheckedChanged: menuGroup.showScenes = checked
            }

            TextSwitch {
                text: qsTr("Start in manual focus and exposure")
                description: qsTr("At start, the focus is manual at the last distance and both wheels are pinned at the last ISO and speed (advanced mode).")
                checked: menuGroup.startManual
                onCheckedChanged: menuGroup.startManual = checked
            }

            ComboBox {
                label: qsTr("Mode at start")
                description: qsTr("Simple: JPEG only, a brightness slider, a short menu. Advanced: everything. \"As left\" keeps the last mode used.")
                currentIndex: menuGroup.startMode === "simple" ? 1 : menuGroup.startMode === "advanced" ? 2 : 0
                menu: ContextMenu {
                    MenuItem { text: qsTr("As left") }
                    MenuItem { text: qsTr("Simple") }
                    MenuItem { text: qsTr("Advanced") }
                }
                onCurrentIndexChanged: menuGroup.startMode = currentIndex === 1 ? "simple" : currentIndex === 2 ? "advanced" : "last"
            }

            TextSwitch {
                text: qsTr("Viewfinder processing in the camera menu")
                description: qsTr("Shows two columns, \"Vf noise\" and \"Vf sharpen\": the viewfinder's own noise reduction and sharpening, separate from the photos'. By default the viewfinder has light noise reduction and no sharpening.")
                checked: menuGroup.previewColumns
                onCheckedChanged: menuGroup.previewColumns = checked
            }

            SectionHeader { text: qsTr("Device tuning") }

            TextSwitch {
                text: qsTr("Focus calibration in the camera menu")
                description: qsTr("Shows the \"Focus cal.\" column: focus on a target at a known distance, tap that distance, the lens position is recorded. Off once the lens is calibrated.")
                checked: device.focusCalibrationMenu
                onCheckedChanged: device.focusCalibrationMenu = checked
            }

            SectionHeader { text: qsTr("Lens shading (vignetting)") }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryHighlightColor
                text: qsTr("Measures how the lens darkens the corners, for every DNG to be corrected. Needs a white translucent diffuser on the lens and an even light; about a minute. The assistant opens at the next start of RAWfish-ir.")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: device.shadingCalRequested ? qsTr("Requested — open RAWfish-ir") : qsTr("Calibrate the lens shading")
                enabled: !device.shadingCalRequested
                onClicked: device.shadingCalRequested = true
            }
            SectionHeader { text: qsTr("Device profile files") }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: device.probeRequested ? qsTr("Requested — open RAWfish-ir") : qsTr("Create the device profile files")
                enabled: !device.probeRequested
                onClicked: device.probeRequested = true
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: (device.probeDone.length > 0 ? qsTr("Last written: %1. ").arg(device.probeDone) : "")
                      + qsTr("RAWfish-ir writes them when it next opens: generated-hal.json (what the camera driver claims) and, if absent, a starter override-hal.json to edit (shutter ceiling, peaking and zebra thresholds), in ~/.local/share/com.rawfish/rawfish-ir/device-profiles/. Changes above apply the next time the camera starts.")
            }
        }
        VerticalScrollDecorator {}
    }
}
