// SPDX-License-Identifier: BSD-3-Clause
//
// Photon's shutter-row side controls: the latest photo as a rounded thumbnail on the
// left (opens the gallery) and a camera-flip button on the right. Fill the item that
// the shutter is centred in; the controls sit at fixed offsets from its centre.
import QtQuick 2.0
import com.vivid.camera 1.0

Item {
    id: sides

    property string folder
    property bool flipVisible: true
    property bool flipEnabled: true

    signal galleryRequested()
    signal flipRequested()

    LatestCapture {
        id: latest
        folder: sides.folder
    }

    Rectangle {
        id: thumb
        width: 36 * PhotonTheme.dp
        height: width
        radius: 4 * PhotonTheme.dp
        color: PhotonTheme.surfaceVariant
        border.width: Math.max(1, PhotonTheme.dp)
        border.color: PhotonTheme.textSecondary
        x: sides.width / 2 - 108 * PhotonTheme.dp - width / 2
        y: (sides.height - height) / 2
        visible: latest.url.toString().length > 0

        Image {
            anchors.fill: parent
            anchors.margins: thumb.border.width + 1.5 * PhotonTheme.dp
            source: latest.url
            sourceSize: Qt.size(96, 96)
            asynchronous: true
            cache: false
            fillMode: Image.PreserveAspectCrop
            smooth: true
        }

        MouseArea {
            anchors.fill: parent
            anchors.margins: -6 * PhotonTheme.dp
            onClicked: sides.galleryRequested()
        }
    }

    Item {
        id: flip
        width: 36 * PhotonTheme.dp
        height: width
        x: sides.width / 2 + 106 * PhotonTheme.dp - width / 2
        y: (sides.height - height) / 2
        visible: sides.flipVisible
        opacity: sides.flipEnabled ? 1.0 : 0.4

        PhotonIcon {
            anchors.centerIn: parent
            kind: "flip"
            color: PhotonTheme.textPrimary
        }

        MouseArea {
            anchors.fill: parent
            enabled: sides.flipEnabled
            onClicked: sides.flipRequested()
        }
    }
}
