// SPDX-License-Identifier: BSD-3-Clause
//
// Photon's capture-mode switcher: a dark pill with a sliding knob behind the selected
// label. The knob is the accent colour for "pro" and white for the others, and the
// selected label is dark on either.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: switcher

    // [{ key: "pro", label: "Pro", enabled: true }, ...]
    property var modes: []
    property string selectedKey

    signal modeSelected(string key)

    readonly property real _inset: 3 * PhotonTheme.dp
    readonly property int _selectedIndex: {
        for (var i = 0; i < modes.length; ++i) {
            if (modes[i].key === selectedKey)
                return i
        }
        return 0
    }
    readonly property real _knobWidth: (width - 2 * _inset) / Math.max(1, modes.length)

    implicitWidth: 76 * PhotonTheme.dp * Math.max(1, modes.length)
    implicitHeight: PhotonTheme.modeSwitcherHeight
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: "#47000000"                    // black at 28%
        border.width: Math.max(1, 0.5 * PhotonTheme.dp)
        border.color: "#2EFFFFFF"             // white at 18%
    }

    Rectangle {
        id: knob

        x: switcher._inset + switcher._selectedIndex * switcher._knobWidth
        y: switcher._inset
        width: switcher._knobWidth
        height: switcher.height - 2 * switcher._inset
        radius: height / 2
        color: switcher.selectedKey === "pro" ? PhotonTheme.accent : "#FFFFFF"

        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }
        Behavior on color { ColorAnimation { duration: 220 } }
    }

    Row {
        x: switcher._inset
        y: switcher._inset
        height: switcher.height - 2 * switcher._inset

        Repeater {
            model: switcher.modes

            delegate: Item {
                readonly property bool selected: modelData.key === switcher.selectedKey
                readonly property bool usable: modelData.enabled !== false

                width: switcher._knobWidth
                height: parent.height

                Text {
                    anchors.centerIn: parent
                    text: modelData.label
                    font.pixelSize: Math.round(12 * PhotonTheme.dp)
                    font.bold: selected
                    color: selected ? "#E0000000"
                                    : (usable ? "#D1FFFFFF" : "#4DFFFFFF")
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: usable
                    onClicked: switcher.modeSelected(modelData.key)
                }
            }
        }
    }
}
