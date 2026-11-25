/*
 * SettingsPage.qml -- RAWfish-ir's page in the system Settings app (Settings >
 * Apps > RAWfish-ir), the way RAWfish has one. It holds what does not belong in
 * the camera's own menus: where captures go and how they are named. Values
 * live in dconf under /apps/rawfish-ir/output and the app reads them at every
 * engine start (Nemo.Configuration on both sides).
 *
 * Folders must be under ~/Pictures or on a memory card (/run/media/<user>/
 * <card>/...): those are the only places the sandboxed app may write
 * (permissions Pictures and RemovableMedia); the page refuses anything else
 * rather than let photos be lost. The "Storage" choice moves both folders at
 * once to the phone's ~/Pictures/RAWfish-ir or to <card>/Pictures/RAWfish-ir;
 * the two fields below it still take any folder in those places. When the
 * card asked for is not mounted, the app saves to ~/Pictures/RAWfish-ir.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.Configuration 1.0
import org.nemomobile.systemsettings 1.0   // PartitionModel: the memory cards, as the system camera's page lists them

Page {
    id: page

    ConfigurationGroup {
        id: output
        path: "/apps/rawfish-ir/output"
        property string prefix: "photo"
        property string jpgDir: "~/Pictures/RAWfish-ir"
        property string dngDir: "~/Pictures/RAWfish-ir"
        property bool jpgWithDng: false        // a DNG of the same instant with each JPG
    }

    // Tuning helpers: the calibration column in the camera's menu, and a
    // request for the app to write the device profile files at its next start
    // (the Settings app itself cannot run the engine).
    ConfigurationGroup {
        id: menuGroup
        path: "/apps/rawfish-ir/menu"
        property string startMode: "last"
        property bool showScenes: false
        property bool processingColumns: false
        property bool startManual: false
    }
    ConfigurationGroup {
        id: device
        path: "/apps/rawfish-ir/device"
        property bool focusCalibrationMenu: false
        property bool probeRequested: false
        property string probeDone: ""        // the app writes the date when done
        property bool shadingCalRequested: false   // the lens-shading assistant, at the next start
        property bool resetSettingsRequested: false // rawfish-ir.conf back to the defaults, at the next start
        property bool extraLog: false               // a detailed log (checks in the reports), for bug reports
    }

    readonly property string picturesHome: StandardPaths.pictures     // "/home/<user>/Pictures"
    // The user's memory cards are mounted under /run/media/<user>/ (the user
    // name is the last part of the home folder, /home/<user>).
    readonly property string mediaHome: "/run/media/" + StandardPaths.home.split("/").pop() + "/"
    readonly property string defaultFolder: "~/Pictures/RAWfish-ir"
    // The card a folder is on ("/run/media/<user>/<card>"), "" for any other
    // folder (the app's own rule, rawfish-ir-preview.cpp cardRoot()).
    function cardOf(f) {
        var t = f.trim()
        if (t.indexOf(mediaHome) !== 0) return ""
        var name = t.substring(mediaHome.length).split("/")[0]
        return name === "" ? "" : mediaHome + name
    }
    function folderOk(f) {
        var t = f.trim()
        if (t.indexOf("~/Pictures") === 0) return true
        if (cardOf(t) !== "") return true
        return t.indexOf(picturesHome) === 0
    }
    // Both folders to one storage: "" the phone's memory, or a card's mount
    // folder. The fields show the new values at once.
    function useStorage(mountPath) {
        var f = mountPath === "" ? defaultFolder : mountPath + "/Pictures/RAWfish-ir"
        output.jpgDir = f; output.dngDir = f
        jpgField.text = f; dngField.text = f
    }

    // The memory cards (external partitions), mounted or not.
    PartitionModel {
        id: partitions
        storageTypes: PartitionModel.External | PartitionModel.ExcludeParents
        onExternalStoragesPopulatedChanged: storageCombo.updateCurrentIndex()
    }
    // Empty: no prefix (files named by the date alone). Not starting with
    // "-": a lone "-" is the engine's "measure only", which writes no file.
    function prefixOk(p) { return p === "" || /^[A-Za-z0-9_][A-Za-z0-9_-]{0,31}$/.test(p) }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: "RAWfish-ir" }

            SectionHeader { text: qsTr("Captures") }

            TextSwitch {
                text: qsTr("Also save a DNG with each JPG")
                description: qsTr("Same exposure, same file name. Saved in the DNG folder. Not working with the camera's own JPEG (HAL JPG), on long exposures (Σ = DNG only) or on phones which can't do both at the same time.")
                checked: output.jpgWithDng
                onCheckedChanged: output.jpgWithDng = checked
            }

            TextField {
                id: prefixField
                width: parent.width
                label: qsTr("File name prefix")
                placeholderText: qsTr("No prefix")
                text: output.prefix
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                errorHighlight: !page.prefixOk(text)
                description: qsTr("Letters, digits, - and _ (up to 32; not starting with -). Files are named PREFIX_YYYYMMDD_HHMMSS.EXT, or YYYYMMDD_HHMMSS.EXT with the field empty; the milliseconds (_mmm) are added when two photos fall in the same second, and always for a bracket or a burst.")
                // Saved when the edit ends (Enter, or the field left), not at
                // every keystroke. (Silica's TextField has no editingFinished:
                // using it made the whole page fail to load.)
                onActiveFocusChanged: if (!activeFocus) { if (page.prefixOk(text)) output.prefix = text }
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: { if (page.prefixOk(text)) output.prefix = text; focus = false }
            }

            // Where both folders go: the phone's memory or a memory card. The
            // item shown is found from the JPEG folder: on a card, that card
            // (or "not inserted" when it is not in the list); anywhere else,
            // the phone's memory.
            ComboBox {
                id: storageCombo
                width: parent.width
                label: qsTr("Storage")
                description: qsTr("Moves both folders below to Pictures/RAWfish-ir on the phone or on the memory card. If the card is not there when the camera starts, photos go to the phone's ~/Pictures/RAWfish-ir.")
                readonly property string wanted: page.cardOf(output.jpgDir)   // "" = the phone's memory
                onWantedChanged: updateCurrentIndex()
                Component.onCompleted: updateCurrentIndex()
                function updateCurrentIndex() {
                    if (!partitions.externalStoragesPopulated) return
                    for (var i = 0; i < menu.children.length; ++i) {
                        var item = menu.children[i]
                        if (item.hasOwnProperty("__silica_menuitem") && item.visible && item.mountPath === wanted) {
                            currentIndex = i
                            currentItem = item
                            return
                        }
                    }
                    currentIndex = -1
                }
                menu: ContextMenu {
                    MenuItem {
                        property string mountPath: ""
                        text: qsTr("Phone memory")
                        onClicked: page.useStorage("")
                    }
                    // The card the folders are on, when it is not in the list
                    // (taken out): shown so the choice stays visible.
                    MenuItem {
                        property string mountPath: storageCombo.wanted
                        text: qsTr("Memory card not inserted")
                        visible: storageCombo.wanted !== "" && partitions.externalStoragesPopulated && !storageCombo.cardListed
                        onVisibleChanged: storageCombo.updateCurrentIndex()
                        opacity: Theme.opacityLow
                    }
                    Repeater {
                        id: cardItems
                        model: partitions
                        delegate: MenuItem {
                            property string mountPath: model.mountPath
                            onMountPathChanged: storageCombo.updateCurrentIndex()
                            enabled: model.status === PartitionModel.Mounted && model.mountPath !== ""
                            text: model.status === PartitionModel.Mounted
                                  ? qsTr("Memory card (%1 free)").arg(Format.formatFileSize(model.bytesAvailable))
                                  : qsTr("Memory card not mounted")
                            onClicked: page.useStorage(model.mountPath)
                        }
                    }
                }
                // Whether the card the folders are on is in the list.
                readonly property bool cardListed: {
                    for (var i = 0; i < cardItems.count; ++i) {
                        var it = cardItems.itemAt(i)
                        if (it && it.mountPath === wanted) return true
                    }
                    return false
                }
            }

            TextField {
                id: jpgField
                width: parent.width
                label: qsTr("JPEG folder")
                placeholderText: "~/Pictures/RAWfish-ir"
                text: output.jpgDir
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
                errorHighlight: !page.folderOk(text)
                description: qsTr("Under ~/Pictures or on the memory card (/run/media/…), the only places the app may write.")
                // Saved when the edit ends (Enter, or the field left), not at
                // every keystroke. (Silica's TextField has no editingFinished:
                // using it made the whole page fail to load.)
                onActiveFocusChanged: if (!activeFocus) { if (page.folderOk(text)) output.jpgDir = text.trim() }
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: { if (page.folderOk(text)) output.jpgDir = text.trim(); focus = false }
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
                // Saved when the edit ends (Enter, or the field left), not at
                // every keystroke. (Silica's TextField has no editingFinished:
                // using it made the whole page fail to load.)
                onActiveFocusChanged: if (!activeFocus) { if (page.folderOk(text)) output.dngDir = text.trim() }
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: { if (page.folderOk(text)) output.dngDir = text.trim(); focus = false }
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Reset to defaults")
                onClicked: {
                    output.prefix = "photo"; output.jpgDir = page.defaultFolder; output.dngDir = page.defaultFolder
                    prefixField.text = output.prefix; jpgField.text = output.jpgDir; dngField.text = output.dngDir
                }
            }

            SectionHeader { text: qsTr("Camera menu") }

            TextSwitch {
                text: qsTr("JPG processing in the camera menu")
                description: qsTr("Shows RAWfish-ir's processing of the JPG in the advanced menu: Denoise (classic or tiles), Frames (1 to 8 merged), Per frame, Colour NR, Moiré, Luma NR and Sharpen. Otherwise they are set in rawfish-ir.conf.")
                checked: menuGroup.processingColumns
                onCheckedChanged: menuGroup.processingColumns = checked
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
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: (device.probeDone.length > 0 ? qsTr("Last written: %1. ").arg(device.probeDone) : "")
                      + qsTr("RAWfish-ir writes them when it next opens: generated-hal.json (what the camera driver claims) and, if absent, a starter override-hal.json to edit (shutter ceiling, peaking and zebra thresholds), in ~/.local/share/com.rawfish/rawfish-ir/device-profiles/. Changes above apply the next time the camera starts.")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: device.probeRequested ? qsTr("Requested — open RAWfish-ir") : qsTr("Create the device profile files")
                enabled: !device.probeRequested
                onClicked: device.probeRequested = true
            }

            SectionHeader { text: qsTr("Log") }
            TextSwitch {
                text: qsTr("Detailed log")
                description: qsTr("Turn it on before reproducing a problem of focus, exposure or gestures: the log then keeps every command and reply (the normal log keeps only the starts, the photos and the errors), and each photo's report the histogram the app showed and the photo's own; our JPG photos carry their settings in their file names (seen in the Gallery's details). Takes effect at the next start of RAWfish-ir.")
                checked: device.extraLog
                onCheckedChanged: device.extraLog = checked
            }

            SectionHeader { text: qsTr("Camera settings") }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Puts every setting of the camera and of rawfish-ir.conf back to its default when RAWfish-ir next opens (close it first if it is open). The current file is kept as rawfish-ir.conf.bak in ~/.config/com.rawfish/rawfish-ir/. The settings of this page are not changed.")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: device.resetSettingsRequested ? qsTr("Requested — open RAWfish-ir") : qsTr("Reset the camera settings")
                enabled: !device.resetSettingsRequested
                onClicked: device.resetSettingsRequested = true
            }
        }
        VerticalScrollDecorator {}
    }
}
