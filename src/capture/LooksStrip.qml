// SPDX-License-Identifier: BSD-3-Clause
//
// Colour-look picker laid over the bottom of the viewfinder, modelled on
// PhotonCamera's main screen: the current look's name sits bottom-left, and tapping
// the "Looks" pill opens a horizontal strip of rounded thumbnails (selected one
// outlined in white) plus an intensity slider. Plain QtQuick so it matches Photon's
// styling instead of Silica's.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: strip

    property string selectedPath
    property int strength: 100
    property bool expanded: false
    property string looksFolder

    signal lookSelected(string path)
    signal strengthEdited(int percent)

    readonly property real _thumb: 64 * PhotonTheme.dp

    implicitHeight: content.height
    height: implicitHeight

    LooksModel {
        id: looksModel
    }

    Binding {
        target: looksModel
        property: "folder"
        value: strip.looksFolder
        when: strip.looksFolder.length > 0
    }

    // Soft scrim so white text stays legible over any scene.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: PhotonTheme.scrim }
        }
    }

    Column {
        id: content
        width: parent.width
        spacing: PhotonTheme.spacing

        Item {
            x: PhotonTheme.spacingLarge
            width: parent.width - 2 * PhotonTheme.spacingLarge
            height: toggle.height + PhotonTheme.spacing

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: looksModel.nameForPath(strip.selectedPath)
                color: PhotonTheme.textPrimary
                font.pixelSize: PhotonTheme.fontSizeBody
                font.bold: true
                style: Text.Outline
                styleColor: "#80000000"
            }

            Rectangle {
                id: toggle
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                width: toggleLabel.width + 2 * PhotonTheme.spacingLarge
                height: toggleLabel.height + PhotonTheme.spacing * 1.5
                radius: height / 2
                color: PhotonTheme.overlayPill

                Text {
                    id: toggleLabel
                    anchors.centerIn: parent
                    text: strip.expanded ? qsTr("Hide looks") : qsTr("Looks")
                    color: PhotonTheme.textPrimary
                    font.pixelSize: PhotonTheme.fontSizeCaption + 1
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: strip.expanded = !strip.expanded
            }
        }

        ListView {
            id: looksList
            visible: strip.expanded
            width: parent.width
            height: visible ? strip._thumb + PhotonTheme.fontSizeCaption * 1.5 + PhotonTheme.spacing : 0
            orientation: ListView.Horizontal
            spacing: PhotonTheme.spacing
            leftMargin: PhotonTheme.spacingLarge
            rightMargin: PhotonTheme.spacingLarge
            clip: true
            model: looksModel

            delegate: Item {
                id: look
                readonly property bool selected: model.path === strip.selectedPath
                width: strip._thumb
                height: looksList.height

                Rectangle {
                    id: frame
                    width: strip._thumb
                    height: strip._thumb
                    radius: PhotonTheme.radiusSmall
                    color: PhotonTheme.surfaceVariant
                    border.width: look.selected ? Math.max(2, 2 * PhotonTheme.dp) : 0
                    border.color: PhotonTheme.textPrimary

                    Image {
                        anchors.fill: parent
                        anchors.margins: frame.border.width
                        source: model.thumbnail
                        sourceSize: Qt.size(strip._thumb, strip._thumb)
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                    }
                }

                Text {
                    anchors { top: frame.bottom; topMargin: PhotonTheme.spacingSmall }
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: model.name
                    color: look.selected ? PhotonTheme.textPrimary : PhotonTheme.textSecondary
                    font.pixelSize: PhotonTheme.fontSizeCaption
                    font.bold: look.selected
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: strip.lookSelected(model.path)
                }
            }

        }

        // Intensity, as in Photon's edit screen: label left, track, percentage right.
        Item {
            id: intensity
            x: PhotonTheme.spacingLarge
            width: parent.width - 2 * PhotonTheme.spacingLarge
            visible: strip.expanded && strip.selectedPath.length > 0
            height: visible ? 36 * PhotonTheme.dp : 0

            Text {
                id: intensityLabel
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Intensity")
                color: PhotonTheme.textSecondary
                font.pixelSize: PhotonTheme.fontSizeCaption + 1
            }

            Text {
                id: intensityValue
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: strip.strength + "%"
                color: PhotonTheme.textSecondary
                font.pixelSize: PhotonTheme.fontSizeCaption + 1
            }

            Item {
                id: track
                anchors {
                    left: intensityLabel.right
                    leftMargin: PhotonTheme.spacingLarge
                    right: intensityValue.left
                    rightMargin: PhotonTheme.spacingLarge
                    verticalCenter: parent.verticalCenter
                }
                height: parent.height

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 2 * PhotonTheme.dp
                    radius: height / 2
                    color: PhotonTheme.textTertiary
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: knob.x + knob.width / 2
                    height: 2 * PhotonTheme.dp
                    radius: height / 2
                    color: PhotonTheme.textPrimary
                }

                Rectangle {
                    id: knob
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16 * PhotonTheme.dp
                    height: width
                    radius: width / 2
                    color: PhotonTheme.textPrimary
                    x: (track.width - width) * strip.strength / 100
                }

                MouseArea {
                    anchors.fill: parent
                    preventStealing: true

                    function updateFrom(mouseX) {
                        var fraction = (mouseX - knob.width / 2)
                                / Math.max(1, track.width - knob.width)
                        var percent = Math.round(100 * Math.max(0, Math.min(1, fraction)))
                        if (percent !== strip.strength)
                            strip.strengthEdited(percent)
                    }

                    onPressed: updateFrom(mouse.x)
                    onPositionChanged: updateFrom(mouse.x)
                }
            }
        }

        Item { width: 1; height: PhotonTheme.spacing }
    }
}
