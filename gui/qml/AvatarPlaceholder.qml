// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The picture of somebody whose picture has not arrived.
//
// The same silhouette the overlay draws, at the same proportions, so a preview of
// a waiting row looks like the waiting row a game gets. Those proportions live in
// common/src/panel.cpp as fractions of the radius; here they are fractions of the
// diameter, which is what a QML item measures in. Change one and change both.
//
// The head and the shoulders are two circles masked to the disc. The shoulders
// circle is mostly outside it: what shows is the part that overlaps, which is the
// shape a pair of shoulders makes. Masking rather than clipping, because clip is
// rectangular and would square off the disc exactly where it is roundest.

import QtQuick
import QtQuick.Effects

Item {
    id: root

    // The disc's colour, from the same theme token the overlay uses.
    property color discColour: "#4f545c"

    // The disc's own colour carried most of the way towards white on a dark disc,
    // or towards black on a light one. The same blend panel.cpp uses, and for the
    // same two reasons: it is quieter than plain white, and plain white is exactly
    // the message title's colour, which anything searching by colour confuses.
    readonly property color markColour: {
        const light = 0.2126 * discColour.r + 0.7152 * discColour.g + 0.0722 * discColour.b;
        const towards = light < 0.5 ? 1.0 : 0.0;
        const blend = c => c + (towards - c) * 0.45;
        return Qt.rgba(blend(discColour.r), blend(discColour.g), blend(discColour.b), 1.0);
    }

    Rectangle {
        id: disc
        anchors.fill: parent
        radius: width / 2
        color: root.discColour
        antialiasing: true
    }

    Item {
        id: mark
        anchors.fill: parent
        visible: false
        layer.enabled: true
        layer.smooth: true
        layer.samples: 4

        Rectangle {
            // Head: radius 0.28 of the disc's, centred 0.28 above its middle.
            width: parent.width * 0.28
            height: width
            radius: width / 2
            x: (parent.width - width) / 2
            y: parent.height * 0.22
            color: root.markColour
            antialiasing: true
        }

        Rectangle {
            // Shoulders: radius 0.72 of the disc's, centred 0.87 below its middle.
            width: parent.width * 0.72
            height: width
            radius: width / 2
            x: (parent.width - width) / 2
            y: parent.height * 0.575
            color: root.markColour
            antialiasing: true
        }
    }

    Rectangle {
        id: discMask
        anchors.fill: parent
        radius: width / 2
        visible: false
        antialiasing: true
        layer.enabled: true
        layer.smooth: true
        layer.samples: 4
    }

    MultiEffect {
        anchors.fill: parent
        source: mark
        maskEnabled: true
        maskSource: discMask
        maskSpreadAtMin: 1.0
        maskThresholdMin: 0.5
    }
}
