// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The message map: one screen with the message box in the corner it is set to, at
// the size and the transparency the game will draw it, and a button in each of the
// other three saying "put it here".
//
// Corners rather than free placement, unlike the voice panel: a message arrives
// unannounced and leaves on its own, so it should always turn up in the same
// predictable spot. The voice panel is permanent, so it goes wherever you want.
//
// The buttons are the desktop style's own, drawn with a frame rather than flat.
// The map is a dark gradient standing in for a game, and a flat button on it is a
// monochrome glyph with no shape until the pointer is over it. What was here
// before was neither: two strokes drawn at each corner, which had to be a fixed
// size -- so the one thing this page must not do, show something at a size the
// game never draws, was drawn three times over.
//
// The occupied corner's button is hidden rather than removed, and all four are
// declared in a fixed order, so that Tab walks them in the same order whatever the
// setting currently is.
//
// The voice panel is deliberately absent, as the message is absent from the panel
// map: each page stands for one box, and a second one in the picture is only
// something else to mistake for the thing being set.

import QtQuick
import QtQuick.Controls
import Vocem

Item {
    id: root

    required property var config

    // The display the map depicts, forwarded to the stage; null keeps the map
    // as it has always been. The page's dropdown sets it.
    property var shownDisplay: null

    readonly property var corners: [
        { corner: 0, right: false, bottom: false, name: qsTr("top left") },
        { corner: 1, right: true, bottom: false, name: qsTr("top right") },
        { corner: 2, right: false, bottom: true, name: qsTr("bottom left") },
        { corner: 3, right: true, bottom: true, name: qsTr("bottom right") }
    ]

    OverlayStage {
        id: stage
        objectName: "cornerStage"

        config: root.config
        shownDisplay: root.shownDisplay
        showPanel: false
        showMessage: true

        Accessible.role: Accessible.Grouping
        Accessible.name: qsTr("Messages appear in the %1 corner")
                            .arg(root.corners[root.config.notificationCorner].name)

        Repeater {
            model: root.corners

            // The same anchor mark the Panel map uses (AnchorMark.qml): the two
            // maps say "the box can go here" with one sign now, the owner's
            // request after the Panel's anchors arrived. Same hidden-when-
            // occupied rule as before, same corners.
            AnchorMark {
                required property var modelData

                text: qsTr("Show messages in the %1 corner").arg(modelData.name)
                Accessible.name: text

                visible: root.config.notificationCorner !== modelData.corner
                enabled: root.config.notificationsEnabled
                onClicked: root.config.notificationCorner = modelData.corner

                // The marks move with the message's own distance from the edge,
                // exactly as the Panel map's do with the panel's: they are the
                // centre of where the box would land, through the same
                // `placeWithin` the drawing uses. They used to sit at a fixed
                // spacing, so the slider moved the box and left its targets
                // behind -- a map disagreeing with the thing it is a map of.
                readonly property real markX:
                    stage.placeWithin(modelData.right ? 1 : 0, 0, stage.width,
                                      stage.messageInset)
                readonly property real markY:
                    stage.placeWithin(modelData.bottom ? 1 : 0, 0, stage.height,
                                      stage.messageInset)

                x: Math.max(1, Math.min(markX - width / 2, stage.width - width - 1))
                y: Math.max(1, Math.min(markY - height / 2, stage.height - height - 1))
            }
        }
    }
}
