// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A group of settings in a rounded box, separated by thin lines, with an
// optional heading and description above it, as the KDE and GNOME guidelines
// describe.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ColumnLayout {
    id: root

    property string title: ""
    // One line saying what the group is for, under its name and outside the
    // box, where the window's other group headers put it.
    property string description: ""
    // The rows, reparented into the card's own column: a caller lists SettingRows.
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
        // Named for the geometry dump: `tests/window_padding.cmake` holds every
        // card's height to the sum of the padded blocks inside it, so a loose
        // unpadded item shows up as a number.
        objectName: "card"
        Layout.fillWidth: true
        implicitHeight: column.implicitHeight
        color: Theme.cardColour

        // A named group, so a screen reader says where one group of rows ends
        // and the next begins.
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
