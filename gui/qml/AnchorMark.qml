// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The anchor mark: a ring with a point in it, the one glyph both maps use for
// "the box can go here". Born on the Panel page's six anchors; the corner
// buttons on Notifications wore a themed icon button until the owner asked the
// two pages to speak the same sign -- and a mark that exists twice starts
// drifting, which is how the two maps disagreed about scale once already.
//
// Shape as well as colour, on a map that is a picture of a game: a flat glyph
// here has no shape until the pointer arrives (the Notifications page learnt
// that first).

import QtQuick
import QtQuick.Controls
import Vocem

AbstractButton {
    id: mark

    // Lit: hover, focus, or the magnet in reach. Emphasised: the magnet alone,
    // which also swells the ring so a drag can see its landing spot grow.
    property bool lit: hovered || activeFocus
    property bool emphasised: false

    width: 16
    height: 16
    activeFocusOnTab: true
    hoverEnabled: true

    Accessible.role: Accessible.Button
    ToolTip.text: text
    ToolTip.visible: hovered
    ToolTip.delay: 600

    background: Rectangle {
        radius: width / 2
        color: mark.lit
               ? Qt.rgba(Theme.palette.highlight.r, Theme.palette.highlight.g,
                         Theme.palette.highlight.b, 0.45)
               : Qt.rgba(0, 0, 0, 0.30)
        border.width: mark.emphasised ? 2 : 1
        border.color: mark.lit ? Theme.palette.highlight : Qt.rgba(1, 1, 1, 0.55)
        antialiasing: true
        scale: mark.emphasised ? 1.3 : 1.0
        Behavior on scale {
            NumberAnimation { duration: 100 }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 4
            height: 4
            radius: 2
            color: Qt.rgba(1, 1, 1, 0.9)
            antialiasing: true
        }
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
