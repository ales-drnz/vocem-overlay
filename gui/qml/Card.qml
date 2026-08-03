// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A group of settings in a rounded box, with an optional heading above it. Both
// the KDE and the GNOME guidelines land on the same shape for this -- related
// controls in one container, separated by thin lines, with the group's name
// outside it -- and it is what makes a settings page readable without a border
// around every single row.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ColumnLayout {
    id: root

    property string title: ""
    // One line saying what the group is for, under its name and outside the box.
    // Beside the title rather than inside the card because that is where this
    // window already says it -- the Applications page's own group headers, and
    // the Debug section's -- and because a paragraph dropped in among the rows
    // is the one thing that does not take the card's padding: it sat flush
    // against the edge while every row beside it was inset.
    property string description: ""
    // The rows. They are reparented into the card's own column, so a caller just
    // lists SettingRows and nothing else.
    default property alias content: column.data

    spacing: Theme.smallSpacing

    Label {
        objectName: "cardTitle"
        text: root.title
        visible: root.title !== ""
        font.bold: true
        // Same left edge as the labels inside the card below it, so a section
        // heading and its rows line up in one column.
        Layout.leftMargin: Theme.cardPadding
        Layout.bottomMargin: root.description === "" ? 2 : 0
    }

    Label {
        text: root.description
        visible: root.description !== ""
        opacity: 0.7
        wrapMode: Text.WordWrap
        font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
        Layout.fillWidth: true
        Layout.leftMargin: Theme.cardPadding
        Layout.rightMargin: Theme.cardPadding
        Layout.bottomMargin: 2
    }

    Rectangle {
        // Named so the geometry dump can be asked what the card actually
        // measured. `tests/window_padding.cmake` reads every card in the
        // window out of the dump and holds its height to the sum of the
        // blocks inside it -- which is how a loose item dropped in beside a
        // SettingRow, flush against the card's edge while its neighbour is
        // inset, becomes a number instead of something spotted by eye.
        objectName: "card"
        Layout.fillWidth: true
        implicitHeight: column.implicitHeight
        color: Theme.cardColour

        // The card is a group of settings, and its heading names the group. Said
        // out loud, that is what tells a screen reader where one group of rows
        // ends and the next begins.
        Accessible.role: Accessible.Grouping
        Accessible.name: root.title
        radius: Theme.cornerRadius
        border.color: Theme.cardBorder
        border.width: 1

        ColumnLayout {
            id: column
            anchors.fill: parent
            spacing: 0
        }
    }
}
