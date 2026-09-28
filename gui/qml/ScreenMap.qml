// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The position page's map: a scale model of the display with the panel on it,
// dragged into place, with six anchors (the corners and the middle of either
// side). A click on an anchor puts the panel there; a drag released close to
// one snaps to it, and the anchor is lit while in reach. An anchor writes the
// exact fractions 0 / 0.5 / 1 through the placement the panel itself uses.
//
// The anchors mark destinations, not the panel's body: the occupied one is
// hidden but declared (so Tab order never changes, as with the corner buttons
// on the Notifications page), and they stack under the panel, so a press on
// both is the panel's drag. The arrangement itself is OverlayStage's.

import QtQuick
import QtQuick.Controls
import Vocem

Item {
    id: root

    required property var config

    // The display the map depicts, forwarded to the stage; set by the page's
    // dropdown.
    property var shownDisplay: null

    // Live while dragging, so the panel follows the pointer without the settings
    // file being rewritten on every mouse event.
    property real liveX: config.positionX
    property real liveY: config.positionY

    // The six anchors as the fractions they write, in one fixed order so the
    // Tab order never depends on where the panel is.
    readonly property var snapPoints: [
        { fx: 0.0, fy: 0.0, name: qsTr("top left") },
        { fx: 0.0, fy: 0.5, name: qsTr("middle left") },
        { fx: 0.0, fy: 1.0, name: qsTr("bottom left") },
        { fx: 1.0, fy: 0.0, name: qsTr("top right") },
        { fx: 1.0, fy: 0.5, name: qsTr("middle right") },
        { fx: 1.0, fy: 1.0, name: qsTr("bottom right") }
    ]

    // How close, in map pixels, a released drag must be for the magnet to take
    // it; scaled with the map so the reach feels the same at any size.
    readonly property real snapThreshold: Math.max(14, stage.height * 0.045)

    // Where an anchor puts the panel's top-left corner, through
    // `stage.placeWithin`, so the anchor and the panel agree (entry 56).
    function snapTargetX(point) {
        return stage.placeWithin(point.fx, stage.panel.width, stage.width, stage.inset);
    }
    function snapTargetY(point) {
        return stage.placeWithin(point.fy, stage.panel.height, stage.height, stage.inset);
    }
    // Pixels back to a fraction: the exact inverse of `stage.placeWithin`
    // (vocem/placement.h has both; panel_geometry asserts the round trip).
    function fractionFrom(position, box, extent, gap) {
        const travel = extent - box - gap * 2
        if (travel <= 0)
            return 0
        return Math.max(0, Math.min(1, (position - gap) / travel))
    }

    // Where the panel is now, as the stored pair of fractions. One function,
    // shared by the drag and the release, so the two cannot use different
    // arithmetic.
    function panelFractions() {
        return {
            x: root.fractionFrom(stage.panel.x, stage.panel.width, stage.width, stage.inset),
            y: root.fractionFrom(stage.panel.y, stage.panel.height, stage.height, stage.inset)
        }
    }

    // The anchor a release at (px, py) snaps to, or null when none is in reach.
    function snapNear(px, py) {
        for (const point of snapPoints) {
            if (Math.hypot(px - snapTargetX(point), py - snapTargetY(point)) <
                snapThreshold) {
                return point;
            }
        }
        return null;
    }

    Connections {
        target: root.config
        function onConfigChanged() {
            if (!grab.drag.active) {
                root.liveX = root.config.positionX;
                root.liveY = root.config.positionY;
            }
        }
    }

    Timer {
        id: commit
        interval: 120
        onTriggered: root.config.setPosition(root.liveX, root.liveY)
    }

    OverlayStage {
        id: stage
        objectName: "positionStage"

        config: root.config
        shownDisplay: root.shownDisplay
        // The panel alone: this page moves only the voice panel.
        showMessage: false

        // The six anchors, under the panel so a press on an overlap is the
        // panel's drag -- at z 0 with the panel raised (the Binding below),
        // never at a negative z, which paints below the map's own background.
        // Each is centred on its destination, nudged only to stay inside the
        // clipped edge.
        Repeater {
            model: root.snapPoints

            AnchorMark {
                id: anchor

                required property var modelData

                readonly property real targetX: root.snapTargetX(modelData)
                readonly property real targetY: root.snapTargetY(modelData)
                // The anchor where the panel already is has nothing to offer:
                // hidden, not removed (see the note at the top).
                readonly property bool occupied:
                    Math.abs(stage.panel.x - targetX) < 0.5 &&
                    Math.abs(stage.panel.y - targetY) < 0.5
                // In reach of the magnet during a drag: lit now, taken at release.
                readonly property bool magnet:
                    grab.drag.active &&
                    Math.hypot(stage.panel.x - targetX,
                               stage.panel.y - targetY) < root.snapThreshold

                // The mark sits on the point of the display the panel will hug
                // (the right column against the right edge), not on the panel's
                // future top-left corner. The magnet and the occupied test stay
                // in panel coordinates.
                readonly property real markX:
                    stage.inset + modelData.fx * (stage.width - 2 * stage.inset)
                readonly property real markY:
                    stage.inset + modelData.fy * (stage.height - 2 * stage.inset)

                z: 0
                x: Math.max(1, Math.min(markX - width / 2, stage.width - width - 1))
                y: Math.max(1, Math.min(markY - height / 2, stage.height - height - 1))
                visible: !occupied
                lit: magnet || hovered || activeFocus
                emphasised: magnet

                Accessible.name: qsTr("Move the panel to the %1").arg(modelData.name)
                text: Accessible.name
                ToolTip.visible: hovered && !grab.drag.active

                onClicked: root.config.setPosition(modelData.fx, modelData.fy)
            }
        }

        // The panel above the anchors, so a press on an overlap starts the drag
        // and an anchor never draws over the thing it would move.
        Binding {
            target: stage.panel
            property: "z"
            value: 1
        }

        // The handle, shown only once the background is faint enough to leave
        // nothing to take hold of: an outline reads as a boundary, not a box.
        Rectangle {
            parent: stage.panel
            anchors.fill: parent
            visible: root.config.backgroundFaint
            color: "transparent"
            radius: 8 * stage.panelFactor
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.35)
            antialiasing: true
        }

        // Dragging happens on the panel itself, so nothing else can steal the press.
        MouseArea {
            id: grab
            parent: stage.panel
            anchors.fill: parent
            cursorShape: grab.drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor

            drag.target: stage.panel
            drag.axis: Drag.XAndYAxis
            drag.minimumX: stage.panel.minX
            drag.maximumX: stage.panel.maxX
            drag.minimumY: stage.panel.minY
            drag.maximumY: stage.panel.maxY
            drag.threshold: 0

            // While dragging, the x/y bindings are suspended, so the fraction is
            // read from where the item actually is.
            onPositionChanged: {
                if (drag.active) {
                    const at = root.panelFractions();
                    root.liveX = at.x;
                    root.liveY = at.y;
                    commit.restart();
                }
            }

            // The magnet: released in reach of an anchor, its exact fractions
            // are written; out of reach, the drag means what it says.
            onReleased: {
                commit.stop();
                const point = root.snapNear(stage.panel.x, stage.panel.y);
                if (point) {
                    root.config.setPosition(point.fx, point.fy);
                } else {
                    const at = root.panelFractions();
                    root.config.setPosition(at.x, at.y);
                }
            }
        }
    }
}
