// SPDX-License-Identifier: BSD-3-Clause
//
// Photon-style exposure controls: a ruler for the selected parameter above the
// AE / Tv / ISO / Av / AWB row. Pure presentation; the owner supplies the parameters
// and acts on the committed choice.
import QtQuick 2.0
import com.vivid.camera 1.0

Column {
    id: deck

    // [{ key, label, valueText, available, count, index, startLabel, endLabel,
    //    currentLabel, majorStep }, ...]
    property var parameters: []
    property string selectedKey: "ev"

    signal indexPicked(string key, int index)
    // For items that cycle through their values on tap instead of showing a ruler.
    signal cycleRequested(string key)

    function _isCycling(key) {
        for (var i = 0; i < parameters.length; ++i) {
            if (parameters[i].key === key)
                return parameters[i].cycles === true
        }
        return false
    }

    readonly property var _selected: {
        for (var i = 0; i < parameters.length; ++i) {
            if (parameters[i].key === selectedKey)
                return parameters[i]
        }
        return null
    }

    spacing: PhotonTheme.spacingSmall

    PhotonRuler {
        x: PhotonTheme.spacingLarge
        width: parent.width - 2 * PhotonTheme.spacingLarge
        visible: deck._selected !== null && deck._selected.available !== false
                 && deck._selected.count > 1
        count: deck._selected ? deck._selected.count : 0
        index: deck._selected ? deck._selected.index : 0
        startLabel: deck._selected ? deck._selected.startLabel : ""
        endLabel: deck._selected ? deck._selected.endLabel : ""
        currentLabel: deck._selected ? deck._selected.currentLabel : ""
        majorStep: deck._selected && deck._selected.majorStep ? deck._selected.majorStep : 0
        onIndexCommitted: deck.indexPicked(deck.selectedKey, index)
    }

    PhotonParameterBar {
        width: parent.width
        selectedKey: deck.selectedKey
        items: deck.parameters
        onItemClicked: {
            if (deck._isCycling(key))
                deck.cycleRequested(key)
            else
                deck.selectedKey = key
        }
    }
}
