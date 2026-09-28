// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The message map: one screen with the message box in the corner it is set to, at
// the size and the transparency the game will draw it, and a button in each of the
// other three saying "put it here".
//
// Corners rather than free placement, unlike the voice panel: a message arrives
// unannounced and leaves on its own, so it should turn up in a predictable spot.
//
// The occupied corner's button is hidden rather than removed, and all four are
// declared in a fixed order, so Tab walks them in the same order whatever the
// setting. The voice panel is deliberately absent: each map stands for one box.

import QtQuick
import QtQuick.Controls
import Vocem

Item {
    id: root

    required property var config

    // The display the map depicts, forwarded to the stage; null means the
    // largest. The page's dropdown sets it.
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

            // The same anchor mark the Panel map uses (AnchorMark.qml).
            AnchorMark {
                required property var modelData

                text: qsTr("Show messages in the %1 corner").arg(modelData.name)
                Accessible.name: text

                visible: root.config.notificationCorner !== modelData.corner
                enabled: root.config.notificationsEnabled
                onClicked: root.config.notificationCorner = modelData.corner

                // Each mark is the centre of where the box would land, through
                // the same `placeWithin` the drawing uses, so it moves with the
                // message's distance from the edge.
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
