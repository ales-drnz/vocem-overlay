// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A slider, the figure it is at, and the way back to the default.
//
// The track must not change length under the cursor: the block has a fixed
// width, the reset is always there and merely disabled, and the figure's box is
// measured from the widest string it can hold ("10.00×", not "1.00×").

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

RowLayout {
    id: root

    required property real value
    required property real defaultValue
    // What a screen reader announces: the row's label is a sibling, not this
    // control's name.
    required property string accessibleName

    property real from: 0.0
    property real to: 1.0
    property real stepSize: 0.01
    // How the figure is written; only the caller knows what it measures.
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
        // Emitted for the arrow keys as well as the pointer.
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
            // The label's own font, by id: TextMetrics is not an item and has
            // no `parent` to take a font from.
            font: figure.font
            text: root.to.toFixed(root.decimals) + root.suffix
        }
    }

    // The same reset as ColourButton's: one way back to the default everywhere.
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
