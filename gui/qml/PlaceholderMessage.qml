// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What an empty view says for itself.
//
// KDE's HIG asks for a placeholder message in an empty view -- an icon, an
// explanation, and where there is something to do about it, one action --
// rather than a line of grey text where the content would have been. The
// distinction it draws is between the *informational* kind, which is quiet
// because the emptiness is normal, and the *actionable* kind, which is not.
// Every empty view in the Debug section is the first kind: no game is drawing
// the overlay because no game is running, and that is not a problem to solve.
//
// Kirigami's component again, and again not the dependency: see InlineMessage
// for why. Centred in whatever it is given, so a tab with nothing in it reads
// as deliberately empty rather than as failed to load.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ColumnLayout {
    id: root

    // Named so the dump says which views were empty and what stood in for
    // them: tests/empty_views.cmake asks that every empty view in the window
    // is one of these rather than a card holding one line of grey text.
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
        // Kept off the edges: a centred paragraph that runs the whole width of
        // a wide window is harder to read than one that does not.
        Layout.maximumWidth: Math.min(parent.width, 420)
        Layout.alignment: Qt.AlignHCenter
    }

    Item { Layout.fillHeight: true }
}
