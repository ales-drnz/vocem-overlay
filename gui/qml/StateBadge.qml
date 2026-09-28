// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The muted / deafened badge, matching what panel.cpp draws on an avatar in game:
// a red disc with a crossed-out microphone, or with headphones for someone who
// cannot hear either.
//
// Drawn with shapes rather than a theme icon, so it looks like the thing in the
// game.

import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property bool deafened: false
    property real diameter: 16
    // The badge's colours and proportions, from include/vocem/theme.h through the
    // bridge; passed in, since this component has no configuration of its own.
    required property var tokens

    width: diameter
    height: diameter

    // panel.cpp works in the badge's radius and this file in its diameter, so
    // every factor is halved: its sixteenths of the radius are thirty-seconds here.
    readonly property real stroke:
        Math.max(1, diameter * root.tokens.badgeStrokeFactor * 0.5)
    readonly property real rim:
        Math.max(1, diameter * root.tokens.badgeRimStrokeFactor * 0.5)

    // The rim, outside the disc in the surface's own colour at full opacity, as
    // the overlay draws it: the badge is carved out of whatever it hangs over.
    Rectangle {
        anchors.centerIn: parent
        width: root.diameter + root.rim * 2
        height: width
        radius: width / 2
        color: "transparent"
        border.width: root.rim
        border.color: root.tokens.badgeRim
        antialiasing: true
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: root.tokens.badgeFill
        antialiasing: true
    }

    // --- headphones
    Shape {
        anchors.fill: parent
        visible: root.deafened
        antialiasing: true
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            strokeColor: root.tokens.badgeGlyph
            strokeWidth: root.stroke
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            startX: root.diameter * 0.25; startY: root.diameter * 0.5625
            PathArc {
                x: root.diameter * 0.75; y: root.diameter * 0.5625
                radiusX: root.diameter * 0.25; radiusY: root.diameter * 0.25
                direction: PathArc.Clockwise
            }
        }
    }

    Row {
        visible: root.deafened
        anchors.centerIn: parent
        anchors.verticalCenterOffset: root.diameter * 0.109375
        spacing: root.diameter * 0.3125

        Repeater {
            model: 2
            Rectangle {
                width: root.diameter * 0.1875
                height: root.diameter * 0.21875
                radius: width / 2
                color: root.tokens.badgeGlyph
                antialiasing: true
            }
        }
    }

    // --- microphone, crossed out
    Rectangle {
        visible: !root.deafened
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.diameter * 0.28125
        width: root.diameter * 0.25
        height: root.diameter * 0.25
        radius: width / 2
        color: root.tokens.badgeGlyph
        antialiasing: true
    }

    Rectangle {
        visible: !root.deafened
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.diameter * 0.6875
        width: root.stroke
        height: root.diameter * 0.09375
        color: root.tokens.badgeGlyph
        antialiasing: true
    }

    Rectangle {
        visible: !root.deafened
        anchors.centerIn: parent
        width: root.diameter * 0.795
        height: root.stroke * 1.25
        radius: height / 2
        color: root.tokens.badgeGlyph
        antialiasing: true
        rotation: 45
    }
}
