// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stand-in for the display with the overlay on it, and the one place that knows
// how the overlay is arranged on a screen.
//
// Four views needed the same arrangement -- the position map, the corner picker,
// the preview column and the spacing diagram -- and each had grown its own copy of
// it. The inset from the edge was written as 8 in one, 12 in another and a clamped
// expression in a third, so the "distance from the edge" setting moved nothing
// anywhere: every view was ignoring it in its own way. The geometry lives here
// now, and a view chooses only its scale and which widgets to show.
//
// Each box is drawn at the overlay's reference size and scaled as a whole, which is
// what keeps it a scale model: laid out directly at a fortieth of a screen, Qt's
// whole-pixel font sizes and whole-pixel layout rounding quantise every distance in
// it. So each box has a wrapper here, in the view's units, holding a preview drawn
// in the overlay's own.
//
// `overlayScale` is the one thing that legitimately differs between views. The maps
// stand for the whole display and derive it from their own height, so what they
// show is to scale. The narrow preview column cannot do that and be legible, so it
// sets its own -- and says so.

import QtQuick
import QtQuick.Controls
import Vocem

Rectangle {
    id: root

    // Named so the development geometry dump can find it. Every view gives its
    // own stage a name of its own; this is the fallback.
    objectName: "stage"

    required property var config

    // The display this map depicts, as one of the bridge's `displays` entries
    // ({name, width, height}), or null for what the map has always shown. Set by
    // the per-map dropdown; null everywhere there is only one display to depict.
    property var shownDisplay: null

    // The display's shape, which is all a faithful model needs -- see the note in
    // PanelPreview.qml. Here rather than in each view, so that two maps of the same
    // screen cannot disagree about it. A logical size is a wrong resolution but a
    // right ratio, which is the one thing it is used for; the resolution itself is
    // a caption and comes from the kernel, through the bridge.
    //
    // A pinned shape wins over everything, the chosen display included, and only
    // the comparison harness pins one. With nothing chosen the map stands for the
    // display the overlay is sized for, and takes its shape from the kernel's
    // mode for that display (the bridge's `sizingDisplayAspect`).
    //
    // `Screen` is the last resort and not the first any more. It is the screen
    // this *window* happens to be on: on a laptop beside a monitor it is a
    // different display from the one the map is about, so the caption said one
    // resolution while the rectangle under it was another display's shape. It is
    // also not a display shape at all under a platform plugin that has no screen,
    // which is what every offscreen run of this window has -- the maps come out
    // square there, and no test could see it.
    readonly property real aspect:
        config.pinnedScreenAspect > 0 ? config.pinnedScreenAspect
        : (shownDisplay && shownDisplay.height > 0 ? shownDisplay.width / shownDisplay.height
        : (config.sizingDisplayAspect > 0 ? config.sizingDisplayAspect
        : (Screen.height > 0 ? Screen.width / Screen.height : 16 / 9)))

    // How much larger (or smaller) the overlay looks on the depicted display than
    // on the one it is sized for. The overlay sizes itself from the LARGEST
    // connected display's mode height (the daemon publishes it; vocem/display.h),
    // so on a smaller display the same pixels cover a larger share of the screen
    // -- by exactly largestHeight / thisHeight. The maps used to normalise that
    // away silently; a map of a particular display has to show it. One while the
    // depicted display is the sizing one, while nothing is chosen, and while the
    // harness pins the shape -- so on the largest display, and in every existing
    // comparison, nothing changes numerically.
    readonly property real sizeRatio:
        config.pinnedScreenAspect > 0 ? 1
        : (shownDisplay && shownDisplay.height > 0 && config.overlayDisplayHeight > 0
               ? config.overlayDisplayHeight / shownDisplay.height : 1)

    // A model of the display fills what it is given while keeping the display's
    // shape. Every view wants this; a view that wants something else can still
    // assign over it.
    width: Math.min(parent.width, parent.height * aspect)
    height: width / aspect
    anchors.centerIn: parent

    // View units per overlay unit, where an overlay unit is a pixel at 1080 lines
    // of display height. A view standing for the whole screen uses height / 1080
    // -- times the honesty factor above, so a map of a display the overlay is NOT
    // sized for draws the boxes at the share of that screen they will really
    // cover. With sizeRatio at 1 this is exactly the old height / 1080.
    property real overlayScale: (height / 1080) * sizeRatio

    // What each box is scaled by. The first of the two is the overlay's own ui_scale, and the message's size
    // setting multiplies it rather than replacing it -- panel.cpp builds the toast
    // at ui_scale() * notification_scale, so a message at 1.0 grows with the panel.
    property real panelFactor: overlayScale * config.scale
    property real messageFactor: panelFactor * config.notificationScale

    property bool showPanel: true
    property bool showMessage: true

    // Exposed so a view can drag what is inside it. The wrapper is what a view
    // places; the preview inside carries the overlay's measurements, in the
    // overlay's own units.
    readonly property alias panel: panelBox

    // The gap each box keeps from the edge of the display, from its own setting --
    // the panel's `screen_margin` and the message's `notification_margin`, as in
    // panel.cpp. Both are multiples of ui_scale rather than of the message's own
    // size: a distance from the display belongs to the display.
    readonly property real inset: config.screenMargin * panelFactor
    readonly property real messageInset: config.notificationMargin * panelFactor

    // Where a box goes, from a fraction and a margin. The same arithmetic as
    // vocem/placement.h, which is the point: a fraction is a place *between the
    // margins*, so 0.5 centres the box instead of putting its near edge halfway.
    // The previews have to reproduce the drawing to the pixel, and this is the one
    // number both of them start from.
    function placeWithin(fraction, box, extent, gap) {
        const travel = extent - box - gap * 2
        if (travel > 0)
            return gap + fraction * travel
        const room = extent - box
        return room > 0 ? room / 2 : 0
    }

    gradient: Theme.gameBackdrop
    radius: Theme.cornerRadius
    border.color: Qt.rgba(1, 1, 1, 0.12)
    border.width: 1
    clip: true

    // What this rectangle stands for, in the middle of it, where it is a caption
    // rather than something to read against an edge. It says the resolution rather
    // than "your screen": the same picture then also answers how large the boxes
    // are against a display of that size. With a display chosen it says which one
    // -- and, when that display is not the one the overlay is sized for, by how
    // much the overlay will look larger or smaller there, because a map that
    // silently rescaled that away was the map lying about the one thing it is
    // a map of.
    Column {
        anchors.centerIn: parent
        spacing: 2

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.shownDisplay
                      ? qsTr("%1 (%2 × %3)").arg(root.shownDisplay.name)
                            .arg(root.shownDisplay.width).arg(root.shownDisplay.height)
                      : root.config.screenResolution
            visible: text !== ""
            color: Qt.rgba(1, 1, 1, 0.28)
            font.pointSize: Math.max(6, Qt.application.font.pointSize - 1)
        }

        Label {
            objectName: "displaySizeNote"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: Math.abs(root.sizeRatio - 1) > 0.001
            text: root.sizeRatio > 1
                      ? qsTr("The overlay is sized for the largest display; here it appears %1% larger")
                            .arg(Math.round((root.sizeRatio - 1) * 100))
                      : qsTr("The overlay is sized for the largest display; here it appears %1% smaller")
                            .arg(Math.round((1 - root.sizeRatio) * 100))
            color: Qt.rgba(1, 1, 1, 0.28)
            font.pointSize: Math.max(6, Qt.application.font.pointSize - 2)
        }
    }

    Item {
        id: panelBox

        objectName: "panel"

        readonly property real factor: root.panelFactor
        readonly property real contentWidth: panelContent.implicitWidth

        visible: root.showPanel
        width: contentWidth * factor
        height: panelContent.implicitHeight * factor

        // The two ends of the travel, which is what a drag has to be limited to.
        // These were deleted when the placement changed and the drag kept asking
        // for them: `drag.maximumX: undefined` is not an error QML reports, it is a
        // limit of zero -- so the panel was yanked back towards the top left on
        // every mouse move, which is the flashing the owner saw. Expressed through
        // the same function as the position, so the ends and the middle cannot
        // disagree.
        readonly property real minX: root.placeWithin(0, width, root.width, root.inset)
        readonly property real maxX: root.placeWithin(1, width, root.width, root.inset)
        readonly property real minY: root.placeWithin(0, height, root.height, root.inset)
        readonly property real maxY: root.placeWithin(1, height, root.height, root.inset)

        // Placed exactly as the overlay places it: between the margins, by the
        // fraction. This used to multiply the fraction by the whole screen and
        // clamp, which put a middle anchor's box below the middle in both the
        // window and the game -- the same defect twice, agreeing with itself.
        x: root.placeWithin(root.config.positionX, width, root.width, root.inset)
        y: root.placeWithin(root.config.positionY, height, root.height, root.inset)

        PanelPreview {
            id: panelContent

            config: root.config
            // What the overlay will not draw past, in the overlay's units: the
            // display less its two margins. A column is held to 520 units and
            // never notices; a row is held by the display alone (panel.cpp),
            // and only a view standing for a display can say how wide that is.
            widestBox: (root.width - root.inset * 2) / panelBox.factor

            transformOrigin: Item.TopLeft
            scale: panelBox.factor
            width: panelBox.contentWidth
            height: implicitHeight
        }
    }

    Item {
        id: messageBox

        objectName: "message"

        readonly property real factor: root.messageFactor
        readonly property real contentWidth: messageContent.implicitWidth

        visible: root.showMessage
        opacity: root.config.notificationsEnabled ? 1.0 : 0.35
        width: contentWidth * factor
        height: messageContent.implicitHeight * factor

        readonly property bool onRight: root.config.notificationCorner === 1 ||
                                        root.config.notificationCorner === 3
        readonly property bool onBottom: root.config.notificationCorner === 2 ||
                                         root.config.notificationCorner === 3

        x: root.placeWithin(onRight ? 1 : 0, width, root.width, root.messageInset)
        y: root.placeWithin(onBottom ? 1 : 0, height, root.height, root.messageInset)

        NotificationPreview {
            id: messageContent

            config: root.config
            // The overlay never draws the box wider than the display less its two
            // margins, expressed in the overlay's units.
            widestBox: (root.width - root.messageInset * 2) / messageBox.factor

            transformOrigin: Item.TopLeft
            scale: messageBox.factor
            width: messageBox.contentWidth
            height: implicitHeight
        }
    }
}
