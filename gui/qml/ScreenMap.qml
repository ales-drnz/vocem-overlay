// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The position page: a scale model of the display with the panel on it, dragged
// to wherever it should sit -- with six anchor points to land on exactly.
//
// The anchors are the six positions somebody usually means when they put a panel
// somewhere: the four corners and the middle of either side. Each is a small
// target inside the map. A click puts the panel there; a drag released close to
// one snaps to it, with the target lit while the pointer is in range so the snap
// never comes as a surprise. What an anchor writes is the exact pair 0 / 0.5 / 1,
// with the inset from the edge applied by the same clamp the panel itself uses --
// so "top left" is the same numbers every time rather than wherever the hand
// happened to stop, and the comparison harness stays exact.
//
// History, because this page had targets once before and they were removed: they
// sat exactly where the panel sits and stole the press meant for it. Three things
// answer that objection now. The anchors mark the panel's destinations rather
// than its body; the occupied one is declared but not shown -- the same rule the
// corner buttons on the Notifications page follow, so Tab walks the rest in an
// order that never changes; and they are stacked under the panel rather than over
// it, so a press that lands on both is the panel's drag, never the target's
// click.
//
// The arrangement itself is OverlayStage's; this file adds the drag and the
// anchors.

import QtQuick
import QtQuick.Controls
import Vocem

Item {
    id: root

    required property var config

    // The display the map depicts, forwarded to the stage; null keeps the map
    // as it has always been. The page's dropdown sets it.
    property var shownDisplay: null

    // Live while dragging, so the panel follows the pointer without the settings
    // file being rewritten on every mouse event.
    property real liveX: config.positionX
    property real liveY: config.positionY

    // The six anchors, as the fractions they write. Declared in one fixed order,
    // left column then right, so the Tab order never depends on where the panel
    // currently is.
    readonly property var snapPoints: [
        { fx: 0.0, fy: 0.0, name: qsTr("top left") },
        { fx: 0.0, fy: 0.5, name: qsTr("middle left") },
        { fx: 0.0, fy: 1.0, name: qsTr("bottom left") },
        { fx: 1.0, fy: 0.0, name: qsTr("top right") },
        { fx: 1.0, fy: 0.5, name: qsTr("middle right") },
        { fx: 1.0, fy: 1.0, name: qsTr("bottom right") }
    ]

    // How close, in map pixels, a released drag has to be for the magnet to take
    // it. Scaled with the map so the reach feels the same at any window size.
    readonly property real snapThreshold: Math.max(14, stage.height * 0.045)

    // Where an anchor puts the panel's top-left corner, in map coordinates --
    // through `stage.placeWithin`, the one arithmetic the overlay itself uses, so
    // the anchor and the panel cannot disagree about where "there" is. It used to
    // be the fraction times the whole map, clamped: at the middle anchors that put
    // the panel's *top edge* halfway down and the magnet agreed with it, both
    // wrong together.
    function snapTargetX(point) {
        return stage.placeWithin(point.fx, stage.panel.width, stage.width, stage.inset);
    }
    function snapTargetY(point) {
        return stage.placeWithin(point.fy, stage.panel.height, stage.height, stage.inset);
    }
    // Pixels back to a fraction: the exact inverse of `stage.placeWithin`, which
    // is the one arithmetic the panel is drawn by (vocem/placement.h has both, and
    // panel_geometry asserts the round trip). Dividing by the map's size -- the
    // inverse of the *old* forward map -- is what made a live drag fight the
    // placement and flash.
    function fractionFrom(position, box, extent, gap) {
        const travel = extent - box - gap * 2
        if (travel <= 0)
            return 0
        return Math.max(0, Math.min(1, (position - gap) / travel))
    }

    // Where the panel is now, as the pair of fractions the configuration stores.
    // ONE place, because two handlers needed it and the second copy is exactly what
    // shipped in 0.1.0-57: the drag was corrected and the *release* kept dividing
    // by the map's size, so letting go wrote a fraction from the arithmetic that
    // had been replaced and the panel jumped somewhere unrelated to the pointer.
    // Entry 33's shape, in QML, inside the fix for it.
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
        // The panel alone. This page answers one question -- where does the voice
        // panel sit -- and a message box drawn beside it only invited the panel to
        // be dragged relative to something this page cannot move.
        showMessage: false

        // The six anchors. Under the panel -- which is what keeps a press on an
        // overlap the panel's drag -- but at z 0, never negative: a child at
        // negative z paints below its parent's own background, so a z of -1 put
        // the anchors under the map's gradient. They still showed when the panel
        // sat at its default corner, an accident of the repaint that a screenshot
        // taken at exactly that position mistook for the feature working; the
        // panel is raised above them instead (the Binding below), so the stacking
        // is declared rather than accidental. Each anchor is centred on the exact
        // destination, nudged only as far as it takes to stay inside the map's
        // clipped edge.
        Repeater {
            model: root.snapPoints

            AnchorMark {
                id: anchor

                required property var modelData

                readonly property real targetX: root.snapTargetX(modelData)
                readonly property real targetY: root.snapTargetY(modelData)
                // Where the panel is right now is where it will stay without a
                // better instruction, so the anchor that says "here" has nothing
                // to offer -- hidden, not removed (see the note at the top).
                readonly property bool occupied:
                    Math.abs(stage.panel.x - targetX) < 0.5 &&
                    Math.abs(stage.panel.y - targetY) < 0.5
                // In reach of the magnet while the panel is being dragged: lit
                // now, taken at release.
                readonly property bool magnet:
                    grab.drag.active &&
                    Math.hypot(stage.panel.x - targetX,
                               stage.panel.y - targetY) < root.snapThreshold

                // The mark sits on the point of the display the panel will hug --
                // the right column mirrored against the right edge, the middles at
                // the exact middle -- NOT on the panel's future top-left corner,
                // which for the right column sat a whole panel-width short of the
                // edge and read as a snap that leaves a gap (the owner's report,
                // looking at it). The magnet and the occupied test keep speaking
                // panel coordinates; only the mark moved.
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

        // The handle, and only once the background has been turned down far enough
        // to leave nothing to take hold of. An outline reads as a boundary rather
        // than as a box, so it cannot be mistaken for a background the panel does
        // not have.
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

            // While the drag is active the bindings on x and y are suspended, so
            // the fraction is read back from where the item actually is.
            onPositionChanged: {
                if (drag.active) {
                    const at = root.panelFractions();
                    root.liveX = at.x;
                    root.liveY = at.y;
                    commit.restart();
                }
            }

            // The magnet: released in reach of an anchor, the anchor's exact
            // fractions are what is written -- not the pixels the pointer stopped
            // at. Out of reach, the drag means what it says.
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
