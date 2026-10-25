/*
 * MenuHeader.qml -- the label at the top of the settings overlay that names
 * the value under the finger (Jolla Camera's HeaderLabel): shown while an
 * item is pressed, kept 3 s after release, then fades.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0

Item {
    id: header
    height: Theme.itemSizeSmall
    function show(text) { label.text = text; label.opacity = 1.0; hold.stop() }
    function hide() { hold.restart() }
    Label {
        id: label
        anchors.centerIn: parent
        font { pixelSize: Theme.fontSizeExtraSmall; bold: true }
        color: Theme.highlightColor
        opacity: 0.0
        Behavior on opacity { FadeAnimation {} }
    }
    Timer { id: hold; interval: 3000; onTriggered: label.opacity = 0.0 }
}
