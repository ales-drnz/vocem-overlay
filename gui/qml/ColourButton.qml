// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A swatch that opens the system colour dialog. Small, because it sits in the
// same column as the sliders and switches on every other row.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Vocem

Item {
    id: root

    required property color colour
    // What the swatch goes back to. The reset appears only once the colour has
    // actually been changed, but it always occupies its space.
    property color defaultColour: colour
    property string title: qsTr("Choose a colour")
    signal picked(color colour)

    // Overridable: the rows whose default is "auto -- follow the ramp" cannot
    // say whether they were changed by comparing colours, because the ramp's own
    // value can be pinned and the swatch shows the effective colour either way.
    property bool changed: Qt.colorEqual(colour, defaultColour) === false
    // What the reset does. The plain rows pick their fixed default back; the
    // auto rows pass a way back of their own, because picking the ramp's current
    // value would pin it instead of following the ramp.
    property var resetAction: null

    implicitWidth: 96
    implicitHeight: Math.max(swatch.height, reset.height)

    // An AbstractButton rather than a Rectangle with a TapHandler on it. As a
    // rectangle it could only be clicked: no focus, no Space or Enter, no role for
    // a screen reader, and nothing to show that it was the focused control -- the
    // one setting on the page that the keyboard could not reach at all.
    AbstractButton {
        id: swatch

        width: 56
        height: 28
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        activeFocusOnTab: true
        hoverEnabled: true

        Accessible.role: Accessible.Button
        Accessible.name: root.title

        onClicked: dialog.open()

        background: Rectangle {
            radius: Theme.cornerRadius / 2
            color: root.colour
            antialiasing: true
            border.width: 1
            // A dark swatch on a dark card, or a white one on a light card, needs
            // an edge or it has no shape at all.
            border.color: Qt.rgba(Theme.textColour.r, Theme.textColour.g,
                                  Theme.textColour.b, 0.35)

            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: "transparent"
                border.width: 2
                border.color: Theme.palette.highlight
                visible: swatch.hovered || swatch.activeFocus
                antialiasing: true
            }
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    // Last in the row and at its right edge, which is where the reset sits on
    // every other kind of row (SliderRow, SpinRow): the way back to the default
    // is meant to be one column down the page. It used to be first, so on the
    // Appearance page the colour rows' resets stood 65 units left of the slider
    // rows' -- measured out of the geometry dump, two right edges where there
    // should have been one. Declared after the swatch as well as drawn after
    // it, so Tab reaches the swatch first; both are in the chain, which the
    // dump reports for every reset in the window (activeFocusOnTab).
    ToolButton {
        id: reset
        objectName: "rowReset"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        icon.name: "edit-undo"
        enabled: root.changed
        opacity: root.changed ? 1.0 : 0.0
        ToolTip.text: qsTr("Back to the default colour")
        ToolTip.visible: hovered
        ToolTip.delay: 600
        onClicked: root.resetAction ? root.resetAction() : root.picked(root.defaultColour)

        Behavior on opacity {
            NumberAnimation { duration: 120 }
        }
    }

    ColorDialog {
        id: dialog
        title: root.title
        selectedColor: root.colour
        // The overlay's colours have no alpha of their own -- transparency is a
        // separate setting, so offering an alpha channel here would be two ways to
        // say the same thing, disagreeing with each other.
        options: ColorDialog.NoEyeDropperButton
        onAccepted: root.picked(selectedColor)
    }
}
