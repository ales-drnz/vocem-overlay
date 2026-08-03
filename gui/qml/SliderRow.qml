// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A slider, the figure it is at, and the way back to the default. Six settings are
// this row, and they were six copies of it: the same three controls in a block
// 210 units wide, with the reset button inside that block and shown only once
// there was something to reset.
//
// Which meant the track changed length under the cursor. Pushing a slider off its
// default made a button appear beside it, the block gave the button its width, and
// the slider lost seventy units of it -- so the value under the handle moved
// without the handle moving. The block is a fixed width here, every control in it
// has a width that does not depend on what it currently reads, and the reset is
// always there and merely disabled: a control that comes and goes moves its
// neighbours, and one faded to nothing is a tab stop with no picture on it.
//
// The figure's box is measured from the widest string it can ever hold rather than
// from the one it holds, for the same reason: "1.00×" and "10.00×" are not the
// same width, and the difference came off the slider.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

RowLayout {
    id: root

    required property real value
    required property real defaultValue
    // What a screen reader announces. The label in the left-hand column of the row
    // is a sibling of this control, not its name, so without this a slider is read
    // out as "slider" and nothing more.
    required property string accessibleName

    property real from: 0.0
    property real to: 1.0
    property real stepSize: 0.01
    // A multiplier, a percentage and a duration are three different sentences, and
    // only the caller knows which one this is.
    property int decimals: 2
    property string suffix: ""

    signal moved(real value)

    readonly property bool changed: Math.abs(value - defaultValue) > stepSize / 2

    // The holder in SettingRow is a plain Item, so the block gives itself its size.
    width: Theme.controlWidth
    height: implicitHeight
    spacing: Theme.smallSpacing

    Slider {
        from: root.from
        to: root.to
        stepSize: root.stepSize
        value: root.value
        // Emitted for the arrow keys as well as for the pointer, so this is the
        // whole of the keyboard path too.
        onMoved: root.moved(value)

        Layout.fillWidth: true
        Layout.minimumWidth: 140

        Accessible.name: root.accessibleName
    }

    Label {
        id: figure

        text: root.value.toFixed(root.decimals) + root.suffix
        horizontalAlignment: Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        Layout.preferredWidth: widest.width
        Layout.alignment: Qt.AlignVCenter

        TextMetrics {
            id: widest
            // The label's own font, by id. `parent` is undefined here: TextMetrics
            // is not an item and has no parent item to take a font from, so this
            // was measuring the widest string in the default font -- which is not
            // the one the figure is drawn in.
            font: figure.font
            text: root.to.toFixed(root.decimals) + root.suffix
        }
    }

    // The same affordance, with the same icon, that ColourButton uses for the same
    // job: one way back to the default everywhere in the window.
    ToolButton {
        objectName: "rowReset"
        icon.name: "edit-undo"
        enabled: root.changed
        ToolTip.text: qsTr("Back to the default")
        ToolTip.visible: hovered
        ToolTip.delay: 600
        Accessible.name: qsTr("Reset %1").arg(root.accessibleName)
        onClicked: root.moved(root.defaultValue)
    }
}
