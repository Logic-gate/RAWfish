// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style parameter row: evenly spaced columns, each a small label above a value
// (AE / Tv / ISO / Av / AWB). The selected item is drawn in the HUD yellow, unselected
// items keep a dim yellow label with a white value, and unavailable items are dimmed.
// Plain QtQuick; the owner decides what each item means and what a tap does.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: bar

    // [{ key: "ev", label: "AE", valueText: "0.0", available: true }, ...]
    property var items: []
    property string selectedKey

    signal itemClicked(string key)

    implicitHeight: PhotonTheme.parameterBarHeight
    height: implicitHeight

    Row {
        anchors.fill: parent

        Repeater {
            model: bar.items

            delegate: Item {
                id: cell

                readonly property bool selected: modelData.key === bar.selectedKey
                readonly property bool available: modelData.available !== false

                width: bar.width / Math.max(1, bar.items.length)
                height: bar.height

                readonly property bool titleOnly: modelData.valueText === undefined
                                                  || modelData.valueText === ""

                Column {
                    anchors.centerIn: parent
                    spacing: PhotonTheme.spacingSmall

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.label
                        font.pixelSize: cell.titleOnly ? PhotonTheme.fontSizeBody
                                                       : PhotonTheme.fontSizeCaption
                        font.bold: true
                        color: !cell.available ? PhotonTheme.textTertiary
                               : cell.selected ? PhotonTheme.hud
                               : (cell.titleOnly ? PhotonTheme.textPrimary : PhotonTheme.hudDim)
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: !cell.titleOnly
                        text: modelData.valueText
                        font.pixelSize: PhotonTheme.fontSizeBody
                        font.bold: cell.selected
                        color: !cell.available ? PhotonTheme.textTertiary
                               : cell.selected ? PhotonTheme.hud
                               : PhotonTheme.textPrimary
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: cell.available
                    onClicked: bar.itemClicked(modelData.key)
                }
            }
        }
    }
}
