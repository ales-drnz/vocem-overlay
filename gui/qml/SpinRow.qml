// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A number that is typed or stepped rather than dragged, and the way back to the
// default. The counterpart of SliderRow, and the same block width, so that a page
// of spin boxes and a page of sliders line up their controls in one column.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

RowLayout {
    id: root

    required property real value
    required property real defaultValue
    required property string accessibleName

    property int from: 0
    property int to: 48

    signal moved(real value)

    readonly property bool changed: Math.round(value) !== Math.round(defaultValue)

    width: Theme.controlWidth
    height: implicitHeight
    spacing: Theme.smallSpacing

    Item { Layout.fillWidth: true }

    SpinBox {
        from: root.from
        to: root.to
        value: Math.round(root.value)
        editable: true
        onValueModified: root.moved(value)
        Accessible.name: root.accessibleName
    }

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
