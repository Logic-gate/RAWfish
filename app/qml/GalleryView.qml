/*
 * GalleryView.qml -- the captures, the first item of the camera page's
 * switcher strip ([ gallery | camera ]), the way Jolla Camera does it
 * (com.jolla.camera's GalleryView in a PagedView): one capture per screen,
 * the system's own ImageViewer (pinch zoom, EXIF orientation), the system's
 * GalleryOverlay (share, delete, details) on top, shown/hidden by a tap.
 *
 * Silica's PagedView, RightToLeft and without wrap: the newest capture is
 * index 0 and sits at the right end, next to the camera; the older ones lie
 * to its left; the strip ends at the oldest. Nested in the switcher: a
 * right-to-left swipe from the newest carries on into the camera in the same
 * movement, as in Jolla Camera. "3 / 27" says where one is. The dismiss
 * button goes back to the camera too.
 *
 * The list follows the folders row by row (rebuild), so the strip stays on
 * the capture shown. Deleting it shows the next older capture in its place,
 * at once (see rebuild: Silica animates programmatic moves of a RightToLeft
 * PagedView the wrong way, so none is animated).
 *
 * DNGs are shown through their NAME_vf.jpg preview (nothing on the phone
 * decodes DNG); deleting a DNG deletes its preview too.
 */
import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Silica.private 1.0 as Private
import Sailfish.Gallery 1.0
import Qt.labs.folderlistmodel 2.1

Item {
    id: gallery
    property var viewfinder                 // the camera's IndrawPreview: folders, deleteFile()
    property var cameraPage                 // returnToCaptureMode()
    readonly property string folder: viewfinder ? "file://" + viewfinder.jpgDir : ""
    readonly property string dngFolder: viewfinder ? "file://" + viewfinder.dngDir : ""
    // A zoomed picture pans: the switcher must not take the drag then.
    readonly property bool positionLocked: slides.currentItem ? slides.currentItem.zoomed : false
    readonly property bool atNewest: slides.currentIndex === 0
    function toNewest() { slides.moveTo(0, PagedView.Immediate) }
    function toCamera() { if (cameraPage) cameraPage.returnToCaptureMode() }

    // JPEGs and DNGs may live in two folders ([output] jpgDir / dngDir);
    // their lists are merged, newest first, into `captures`.
    FolderListModel {
        id: jpgs
        folder: gallery.folder
        nameFilters: [ "*_???.jpg", "*_???.jpeg" ]   // NAME_YYYYMMDD_HHMMSS_mmm.EXT
        showDirs: false
        sortField: FolderListModel.Time     // QDir::Time: newest first
        onCountChanged: gallery.rebuild()
    }
    FolderListModel {
        id: dngs
        folder: gallery.dngFolder
        nameFilters: [ "*_???.dng" ]
        showDirs: false
        sortField: FolderListModel.Time
        onCountChanged: gallery.rebuild()
    }
    // merged: fileName, filePath, fileURL, isDng -- NEWEST FIRST (index 0,
    // at the right end of the RightToLeft strip, next to the camera)
    ListModel { id: captures }
    // A file whose details are not read yet (the folder still loading) is
    // skipped this time, not allowed to abort the whole list; the list is
    // then brought up to date once more a moment later.
    function entry(model, i, dng) {
        var m = model.get(i, "fileModified"), n = model.get(i, "fileName")
        if (!n || !m || typeof m.getTime !== "function") return null
        return { fileName: n, filePath: model.get(i, "filePath"), fileURL: String(model.get(i, "fileURL")),
                 isDng: dng, t: m.getTime() }
    }
    Timer { id: retryRebuild; interval: 500; onTriggered: gallery.rebuild() }
    // Brings `captures` up to date with the folders, ROW BY ROW, as Jolla
    // Camera's own capture model does (it removes and inserts rows, never
    // resets): a file gone is removed from the list, a new one inserted at its
    // place. The strip is told exactly which row changed, so it stays on the
    // capture shown; when that is the one deleted, step 0 shows the next
    // older one first: sorting goes on from there.
    // (Up to 0.20.25 the list was cleared and refilled: the strip started
    // again at the newest capture and recreated every picture.) As in Jolla
    // Camera, a new capture arriving while the newest is shown is shown.
    function rebuild() {
        var list = [], e, skipped = false, k, i
        for (i = 0; i < jpgs.count; ++i) { if ((e = entry(jpgs, i, false))) list.push(e); else skipped = true }
        for (i = 0; i < dngs.count; ++i) { if ((e = entry(dngs, i, true))) list.push(e); else skipped = true }
        if (skipped) retryRebuild.restart()
        list.sort(function(a, b) { return b.t - a.t })          // newest first
        var wanted = {}
        for (k = 0; k < list.length; ++k) wanted[list[k].filePath] = true
        var wasEmpty = captures.count === 0, atNewest = wasEmpty || slides.currentIndex === 0
        var newest = wasEmpty ? "" : captures.get(0).filePath
        // 0. The capture shown is gone (deleted): show its neighbour FIRST,
        //    the next older one still there (or the newer one if it was the
        //    oldest), so that the page shown is never the row removed below.
        //    Silica's PagedView, RightToLeft, animates any move it makes by
        //    itself the wrong way (the removal of its current page, or a
        //    change of currentIndex: the picture leaves to the left while the
        //    one from the left arrives), and offers no setting for it -- so
        //    the move is immediate (moveTo with PagedView.Immediate, as
        //    documented): the neighbour simply replaces the deleted capture.
        var cur = slides.currentIndex
        if (!wasEmpty && cur >= 0 && cur < captures.count && !wanted[captures.get(cur).filePath]) {
            var to = -1
            for (i = cur + 1; i < captures.count && to < 0; ++i) if (wanted[captures.get(i).filePath]) to = i
            for (i = cur - 1; i >= 0 && to < 0; --i) if (wanted[captures.get(i).filePath]) to = i
            if (to >= 0) slides.moveTo(to, PagedView.Immediate)
        }
        // 1. the rows whose file is gone (from the end: indexes stay valid)
        for (i = captures.count - 1; i >= 0; --i)
            if (!wanted[captures.get(i).filePath]) captures.remove(i)
        // 2. walk both lists in order: the rows still missing are inserted
        //    where they belong (both lists are newest first)
        for (k = 0; k < list.length; ++k)
            if (k >= captures.count || captures.get(k).filePath !== list[k].filePath) {
                // a row further down for this file (its date changed): move it up
                for (i = k + 1; i < captures.count; ++i)
                    if (captures.get(i).filePath === list[k].filePath) { captures.remove(i); break }
                captures.insert(k, list[k])
            }
        // 3. leftovers past the end (should not happen)
        while (captures.count > list.length) captures.remove(captures.count - 1)
        if (captures.count > 0 && (wasEmpty || (atNewest && captures.get(0).filePath !== newest)))
            toNewest()                                           // first fill, or a new capture while on the newest
    }
    // The DNG previews live in the hidden ".previews" subfolder of the DNG
    // folder (the system indexer skips it: they never appear as photos).
    FolderListModel {
        id: previews
        folder: gallery.dngFolder + "/.previews"
        nameFilters: [ "*_vf.jpg" ]
        showDirs: false
    }
    function previewFor(name) {
        var want = name.substr(0, name.length - 4) + "_vf.jpg"
        for (var i = 0; i < previews.count; ++i)
            if (previews.get(i, "fileName") === want) return previews.get(i, "fileURL")
        return ""
    }

    // The strip: Silica's PagedView, right to left, no wrap (Jolla Camera's).
    PagedView {
        id: slides
        anchors.fill: parent
        model: captures
        clip: true
        direction: PagedView.RightToLeft
        wrapMode: PagedView.NoWrap
        interactive: !gallery.positionLocked
        delegate: Item {
            width: slides.width; height: slides.height
            readonly property bool zoomed: viewer.zoomed
            readonly property string name: model.fileName
            readonly property bool isDng: model.isDng
            readonly property string shown: isDng ? gallery.previewFor(name) : model.fileURL
            readonly property string path: model.filePath
            // Pinch zooms (the viewer's own); the overlay hides while zoomed,
            // as in Jolla Camera; a tap toggles it, or zooms out first.
            ImageViewer {
                id: viewer
                anchors.fill: parent
                source: shown
                active: slides.currentIndex === index && gallery.visible
                viewMoving: slides.moving
                onZoomedChanged: overlay.active = !zoomed
                onClicked: { if (zoomed) zoomOut(); else overlay.active = !overlay.active }
            }
            // A DNG without a preview yet (older files): a tap here still
            // toggles the overlay, so the X is always reachable -- with no
            // viewer to tap, the overlay could not be brought back and the
            // page could not be left.
            MouseArea {
                anchors.fill: parent
                visible: isDng && shown === ""
                onClicked: overlay.active = !overlay.active
            }
            Column {
                anchors.centerIn: parent
                visible: isDng && shown === ""
                Label { text: "DNG"; font.pixelSize: Theme.fontSizeHuge; color: Theme.highlightColor
                        anchors.horizontalCenter: parent.horizontalCenter }
                Label { text: name; font.pixelSize: Theme.fontSizeSmall; color: Theme.primaryColor }
            }
            // A DNG is shown through its quick preview: a banner sits on the
            // top edge of the picture itself (not the screen's), so it reads
            // as part of it, and says that deleting it deletes the DNG. The
            // picture's top is found from its aspect ratio (a tiny hidden load
            // of the same file) and the fit-to-page layout; hidden while zoomed.
            Image {
                id: aspectProbe
                visible: false
                source: isDng ? shown : ""
                sourceSize.width: 64
                asynchronous: true
            }
            Rectangle {
                id: dngBanner
                visible: isDng && shown !== "" && !viewer.zoomed && aspectProbe.status === Image.Ready
                readonly property real aspect: aspectProbe.implicitWidth > 0 ? aspectProbe.implicitHeight / aspectProbe.implicitWidth : 0.75
                readonly property real paintedH: Math.min(parent.height, parent.width * aspect)
                readonly property real paintedW: Math.min(parent.width, parent.height / aspect)
                x: (parent.width - paintedW) / 2
                y: (parent.height - paintedH) / 2
                width: paintedW
                height: bannerText.height + 2 * Theme.paddingSmall
                color: Qt.rgba(0, 0, 0, 0.55)
                Column {
                    id: bannerText
                    anchors.centerIn: parent
                    width: parent.width - 2 * Theme.paddingMedium
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("DNG quick preview")
                        color: Theme.highlightColor
                        font { pixelSize: Theme.fontSizeMedium; bold: true }
                    }
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: qsTr("Deleting this preview deletes the original DNG file.")
                        color: Theme.secondaryHighlightColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }
                }
            }
        }
    }
    Label {
        anchors.centerIn: parent
        visible: captures.count === 0
        text: qsTr("No captures yet"); color: Theme.highlightColor
    }

    // The system's overlay: share, delete (with remorse), details, dismiss.
    // Where we are, counted from the newest (1 = the newest, next to the
    // camera); shown with the overlay.
    Label {
        z: 1
        visible: overlay.active && captures.count > 0
        // below the screen's cutout (the notch), not in it
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.itemSizeLarge }
        text: (slides.currentIndex + 1) + " / " + captures.count
        color: Theme.highlightColor; font.pixelSize: Theme.fontSizeSmall
        style: Text.Outline; styleColor: "black"
    }
    GalleryOverlay {
        id: overlay
        anchors.fill: parent
        active: captures.count > 0
        // Share / details / delete act on the capture itself (the DNG), never
        // on its preview; the viewer shows the preview.
        source: slides.currentItem ? "file://" + slides.currentItem.path : ""
        isImage: true
        editingAllowed: false
        onRemove: {
            var item = slides.currentItem
            if (!item) return
            var path = item.path, isDng = item.isDng
            remorseAction(qsTr("Deleting"), function() {
                viewfinder.deleteFile(path)
                if (isDng) {                        /* and its preview, in .previews/ */
                    var i = path.lastIndexOf("/")
                    viewfinder.deleteFile(path.substr(0, i) + "/.previews/" + path.substr(i + 1, path.length - i - 5) + "_vf.jpg")
                }
            })
        }
        // The dismiss button goes forward to the camera page (attached to
        // the right), not back: this page is the root of the stack.
        Private.DismissButton { popPageOnClick: false; onClicked: gallery.toCamera() }
    }
}
