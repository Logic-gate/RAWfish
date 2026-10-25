/*
 * ValueWheel.qml -- a scrolling value wheel, modelled on RAWfish's
 * ValueCarousel (simplified: snap-to-item ListView with the centred item
 * selected and emphasised, without the curve/blur effects).
 *
 * Two uses: a manual wheel (flicking or tapping an item emits
 * valueSelected(value)) and a live wheel (`live` true): it follows
 * currentValue on its own and the centre item shows `liveText`, the real
 * intermediate value, instead of the nearest notch. The first touch on a live
 * wheel emits touched() BEFORE any selection, so the owner can freeze it.
 */
import QtQuick 2.0
import Sailfish.Silica 1.0

Item {
    id: root
    property var model: []                 // array of values
    property var currentValue
    property var valueLabel                 // optional: function(value) -> string (plain text)
    // Optional decorations around the value, plain text each (no rich text:
    // rich text cannot shrink to fit, and a wide label was cut at both ends):
    //   prefixFor(value) -> a small mark before the value (the Σ of stacks)
    //   subFor(value)    -> a second, smaller line right under it, set tight
    //                      (the real shot time of a stack, "(2:48)")
    property var prefixFor
    property var subFor
    property color textColor: "white"       // non-selected items (deck is white)
    property bool orientationHorizontal: false   // false = vertical wheel
    property bool live: false               // follows currentValue, shows liveText
    property string liveText: ""            // centre text while live
    signal valueSelected(var value)
    signal touched()

    function textFor(v) { return valueLabel ? valueLabel(v) : String(v) }
    // Nearest notch to v (numeric models); exact match for others.
    function indexOfValue(v) {
        var best = 0, bestD = -1
        for (var i = 0; i < model.length; ++i) {
            if (model[i] === v) return i
            if (typeof v === "number" && typeof model[i] === "number") {
                var d = Math.abs(Math.log(model[i] / v))
                if (bestD < 0 || d < bestD) { bestD = d; best = i }
            }
        }
        return best
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: root.model
        orientation: root.orientationHorizontal ? ListView.Horizontal : ListView.Vertical
        readonly property bool horiz: root.orientationHorizontal
        property bool userMoving: false     // a drag/flick by the user is going on
        // The selected item sits at the centre (along the scroll axis).
        readonly property real itemH: horiz
                 ? Math.max(Theme.itemSizeMedium, Math.round(width / 5))   // item width when horizontal
                 : Math.max(Theme.fontSizeLarge * 1.6, Math.round(height / 5))
        readonly property real sideMargin: horiz
                 ? Math.max(0, width / 2 - itemH / 2)
                 : Math.max(0, height / 2 - itemH / 2)
        preferredHighlightBegin: sideMargin
        preferredHighlightEnd: sideMargin
        highlightRangeMode: ListView.StrictlyEnforceRange
        highlightMoveDuration: 150
        snapMode: ListView.SnapToItem
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 2 * Theme.flickDeceleration
        maximumFlickVelocity: Theme.maximumFlickVelocity / 2
        currentIndex: root.indexOfValue(root.currentValue)

        // Empty space so the first/last item can reach the centre.
        header: Item { width: list.horiz ? list.sideMargin : 1
                       height: list.horiz ? 1 : list.sideMargin }
        footer: Item { width: list.horiz ? list.sideMargin : 1
                       height: list.horiz ? 1 : list.sideMargin }

        onDragStarted: { userMoving = true; root.touched() }
        onFlickStarted: userMoving = true
        onMovementEnded: {
            if (!userMoving) return             // programmatic (live) scroll
            userMoving = false
            if (currentIndex >= 0 && currentIndex < root.model.length)
                root.valueSelected(root.model[currentIndex])
        }

        delegate: MouseArea {
            width: list.horiz ? list.itemH : list.width
            height: list.horiz ? list.height : list.itemH
            readonly property bool selected: index === list.currentIndex
            onPressed: root.touched()
            onClicked: {
                list.positionViewAtIndex(index, ListView.Center)
                root.valueSelected(root.model[index])
            }
            // The value the item shows: the live one on the centre item while
            // the wheel follows the exposure, else this notch.
            readonly property bool showLive: selected && root.live && root.liveText.length > 0
            readonly property var shownValue: showLive ? root.currentValue : modelData
            readonly property string prefix: root.prefixFor ? root.prefixFor(shownValue) : ""
            readonly property string sub: root.subFor ? root.subFor(shownValue) : ""
            readonly property color ink: selected ? "white" : root.textColor
            Column {
                anchors.centerIn: parent
                width: parent.width
                spacing: -Theme.paddingSmall                 // the sub line sits close
                opacity: parent.selected ? 1.0 : Theme.opacityLow
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    Label {                                   // Σ: 70 % of the value's size
                        visible: text.length > 0
                        anchors.baseline: valueText.baseline
                        text: parent.parent.parent.prefix
                        color: parent.parent.parent.ink
                        font.pixelSize: valueText.font.pixelSize * 0.7
                    }
                    Label {
                        id: valueText
                        // Shrinks to fit the column rather than being cut.
                        width: Math.min(implicitWidth, parent.parent.width - (parent.children[0].visible ? parent.children[0].width : 0))
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: Theme.fontSizeTiny
                        horizontalAlignment: Text.AlignHCenter
                        color: parent.parent.parent.ink
                        font {
                            pixelSize: parent.parent.parent.selected ? Theme.fontSizeLarge : Theme.fontSizeMedium
                            bold: parent.parent.parent.selected
                            italic: parent.parent.parent.showLive   // live value, not a notch
                        }
                        text: parent.parent.parent.showLive ? root.liveText : root.textFor(parent.parent.parent.shownValue)
                    }
                }
                Label {                                       // second line, tight and small
                    visible: text.length > 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Theme.fontSizeTiny * 0.8
                    color: parent.parent.ink
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: parent.parent.sub
                }
            }
        }
    }
}
