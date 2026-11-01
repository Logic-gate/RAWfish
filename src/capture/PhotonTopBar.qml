// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style top bar: a row of icon buttons spread evenly across the width. Here:
// self-timer (shows the delay in seconds when set), histogram toggle (yellow while on)
// and settings. Presentation only; the owner acts on the signals.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: bar

    property int timerSeconds: 0
    property bool histogramOn: true
    property bool flashOn: false

    signal flashClicked()
    signal timerClicked()
    signal histogramClicked()
    signal settingsClicked()

    implicitHeight: 48 * PhotonTheme.dp
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: PhotonTheme.scrim }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }

    Row {
        anchors.fill: parent

        Item {
            width: bar.width / 4
            height: bar.height

            PhotonIcon {
                anchors.centerIn: parent
                kind: bar.flashOn ? "flash" : "flashoff"
                color: bar.flashOn ? PhotonTheme.hud : PhotonTheme.textPrimary
            }

            MouseArea { anchors.fill: parent; onClicked: bar.flashClicked() }
        }

        Item {
            width: bar.width / 4
            height: bar.height

            PhotonIcon {
                anchors.centerIn: parent
                kind: "timer"
                color: bar.timerSeconds > 0 ? PhotonTheme.hud : PhotonTheme.textPrimary
            }

            Text {
                visible: bar.timerSeconds > 0
                anchors { left: parent.horizontalCenter; leftMargin: 14 * PhotonTheme.dp
                          verticalCenter: parent.verticalCenter }
                text: bar.timerSeconds + "s"
                color: PhotonTheme.hud
                font.pixelSize: PhotonTheme.fontSizeCaption
                font.bold: true
            }

            MouseArea { anchors.fill: parent; onClicked: bar.timerClicked() }
        }

        Item {
            width: bar.width / 4
            height: bar.height

            PhotonIcon {
                anchors.centerIn: parent
                kind: "histogram"
                color: bar.histogramOn ? PhotonTheme.hud : PhotonTheme.textPrimary
            }

            MouseArea { anchors.fill: parent; onClicked: bar.histogramClicked() }
        }

        Item {
            width: bar.width / 4
            height: bar.height

            PhotonIcon {
                anchors.centerIn: parent
                kind: "gear"
                color: PhotonTheme.textPrimary
            }

            MouseArea { anchors.fill: parent; onClicked: bar.settingsClicked() }
        }
    }
}
