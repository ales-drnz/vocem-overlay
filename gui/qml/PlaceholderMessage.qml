// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What an empty view says for itself.
//
// KDE's HIG placeholder message, the quiet informational kind: an icon, a
// title and an explanation, centred in whatever it is given, so an empty tab
// reads as deliberately empty rather than failed to load. Like InlineMessage,
// without depending on Kirigami.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ColumnLayout {
    id: root

    // Named for the geometry dump: tests/empty_views.cmake asks that every
    // empty view in the window is one of these.
    objectName: "placeholder"

    property string iconName: "dialog-information"
    property string title: ""
    property string explanation: ""

    spacing: Theme.mediumSpacing

    Accessible.role: Accessible.StaticText
    Accessible.name: root.title + ". " + root.explanation

    Item { Layout.fillHeight: true }

    Image {
        source: "image://icon/" + root.iconName
        sourceSize.width: 48
        sourceSize.height: 48
        Layout.preferredWidth: 48
        Layout.preferredHeight: 48
        Layout.alignment: Qt.AlignHCenter
        opacity: 0.5
    }

    Label {
        text: root.title
        visible: root.title !== ""
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignHCenter
    }

    Label {
        text: root.explanation
        visible: root.explanation !== ""
        opacity: 0.7
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        // Kept off the edges: a centred paragraph is hard to read full-width.
        Layout.maximumWidth: Math.min(parent.width, 420)
        Layout.alignment: Qt.AlignHCenter
    }

    Item { Layout.fillHeight: true }
}
