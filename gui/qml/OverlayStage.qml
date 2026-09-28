// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stand-in for the display with the overlay on it, and the one place that
// knows how the overlay is arranged on a screen: the position map, the corner
// picker, the preview column and the spacing diagram all use it, and a view
// chooses only its scale and which boxes to show.
//
// Each box is a wrapper in the view's units holding a preview drawn at the
// overlay's reference size and scaled whole (see PanelPreview.qml for why).
// `overlayScale` is the one thing that differs between views: the maps derive
// it from their height, so they are to scale; the narrow preview column sets
// its own.

import QtQuick
import QtQuick.Controls
import Vocem

Rectangle {
    id: root

    // Named for the geometry dump; each view renames its own stage.
    objectName: "stage"

    required property var config

    // The display this map depicts, one of the bridge's `displays` entries
    // ({name, width, height}), or null for the sizing display. Set by the
    // per-map dropdown.
    property var shownDisplay: null

    // The display's shape, which is all a faithful model needs (see
    // PanelPreview.qml); here so two maps of one screen cannot disagree.
    // In order: a shape pinned by the comparison harness, the chosen display,
    // the kernel's mode for the sizing display (`sizingDisplayAspect`), and
    // last `Screen` -- which is the screen this window is on, possibly another
    // display, and no display shape at all offscreen.
    readonly property real aspect:
        config.pinnedScreenAspect > 0 ? config.pinnedScreenAspect
        : (shownDisplay && shownDisplay.height > 0 ? shownDisplay.width / shownDisplay.height
        : (config.sizingDisplayAspect > 0 ? config.sizingDisplayAspect
        : (Screen.height > 0 ? Screen.width / Screen.height : 16 / 9)))

    // How much larger (or smaller) the overlay looks on the depicted display
    // than on the one it is sized for. The overlay sizes itself from the
    // largest connected display's mode height (vocem/display.h), so on a
    // smaller display it covers largestHeight / thisHeight more of the screen.
    // One for the sizing display, with nothing chosen, and with a pinned shape.
    readonly property real sizeRatio:
        config.pinnedScreenAspect > 0 ? 1
        : (shownDisplay && shownDisplay.height > 0 && config.overlayDisplayHeight > 0
               ? config.overlayDisplayHeight / shownDisplay.height : 1)

    // Fill what is given, keeping the display's shape; a view may override.
    width: Math.min(parent.width, parent.height * aspect)
    height: width / aspect
    anchors.centerIn: parent

    // View units per overlay unit (a pixel at 1080 lines of display height),
    // times sizeRatio so a map of another display draws the boxes at their
    // real share of it.
    property real overlayScale: (height / 1080) * sizeRatio

    // What each box is scaled by: the overlay's ui_scale, and for the message
    // also its size setting on top (panel.cpp builds the toast at
    // ui_scale() * notification_scale).
    property real panelFactor: overlayScale * config.scale
    property real messageFactor: panelFactor * config.notificationScale

    property bool showPanel: true
    property bool showMessage: true

    // Exposed so a view can drag it. The wrapper is what a view places; the
    // preview inside works in overlay units.
    readonly property alias panel: panelBox

    // Each box's gap from the display edge, from its own setting (screen_margin,
    // notification_margin), as in panel.cpp. Both scale with ui_scale, not the
    // message's size: a distance from the display belongs to the display.
    readonly property real inset: config.screenMargin * panelFactor
    readonly property real messageInset: config.notificationMargin * panelFactor

    // Where a box goes, from a fraction and a margin, as vocem/placement.h does:
    // a fraction is a place between the margins, so 0.5 centres the box.
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

    // The caption: the resolution, or the chosen display's name and size, and
    // when that display is not the sizing one, how much larger or smaller the
    // overlay will look there.
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
        // Dimmed when the panel is switched off, as the message box is (entry 173).
        opacity: root.config.panelEnabled ? 1.0 : 0.35
        width: contentWidth * factor
        height: panelContent.implicitHeight * factor

        // The ends of the drag's travel, from the same function as the
        // position. A drag limit left undefined is not an error in QML but a
        // limit of zero.
        readonly property real minX: root.placeWithin(0, width, root.width, root.inset)
        readonly property real maxX: root.placeWithin(1, width, root.width, root.inset)
        readonly property real minY: root.placeWithin(0, height, root.height, root.inset)
        readonly property real maxY: root.placeWithin(1, height, root.height, root.inset)

        // Placed as the overlay places it: between the margins, by the fraction.
        x: root.placeWithin(root.config.positionX, width, root.width, root.inset)
        y: root.placeWithin(root.config.positionY, height, root.height, root.inset)

        PanelPreview {
            id: panelContent

            config: root.config
            // The display less its two margins, in overlay units. A column is
            // held to 520 anyway; a row is held by the display alone (panel.cpp).
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
            // The display less its two margins, in overlay units.
            widestBox: (root.width - root.messageInset * 2) / messageBox.factor

            transformOrigin: Item.TopLeft
            scale: messageBox.factor
            width: messageBox.contentWidth
            height: implicitHeight
        }
    }
}
