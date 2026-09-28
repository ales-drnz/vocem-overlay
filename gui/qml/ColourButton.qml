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
    // changed, but always occupies its space.
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

    // An AbstractButton, so the swatch takes focus, Space and Enter, and has a
    // role for a screen reader.
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

    // Last in the row and at its right edge, where the reset sits on every
    // other kind of row (SliderRow, SpinRow), so the resets form one column.
    // Declared after the swatch, so Tab reaches the swatch first.
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
        // No alpha: transparency is a separate setting.
        options: ColorDialog.NoEyeDropperButton
        onAccepted: root.picked(selectedColor)
    }
}
