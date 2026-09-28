// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One setting: what it is on the left, the control that changes it on the right,
// and a line underneath saying what it does -- part of the row rather than a
// tooltip, so nobody has to hover to find it. Two lines and no more; a warning is
// said by the description, which can change with the setting.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property string label
    property string description: ""
    property bool first: false
    // A picture in front of the label, for rows that stand for an application.
    // Empty, it takes no room.
    property string icon: ""
    // Raw material behind the row, shown on hover (the application rows put the
    // record's evidence token here), for quoting into a report. A tooltip is
    // never the only place something is said: the description carries the words.
    property string tooltip: ""

    HoverHandler { id: rowHover; enabled: root.tooltip !== "" }
    ToolTip.visible: rowHover.hovered
    ToolTip.text: root.tooltip
    ToolTip.delay: 600

    // The control goes here.
    default property alias control: holder.data

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight + Theme.cardPadding * 2
    Layout.fillWidth: true

    // Rows are divided by a line rather than by gaps, so the card reads as one
    // object. The first row has nothing above it.
    Rectangle {
        visible: !root.first
        width: parent.width
        height: 1
        color: Theme.separator
    }

    RowLayout {
        id: layout

        // The padded content of one block in a card, under the name the
        // padding check measures (see Card.qml).
        objectName: "cardContent"

        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadding
        anchors.rightMargin: Theme.cardPadding
        anchors.topMargin: Theme.cardPadding
        anchors.bottomMargin: Theme.cardPadding
        spacing: Theme.mediumSpacing

        // Centred against the control, so the label and description sit in the
        // middle of the row's height with one line or two.
        Image {
            source: root.icon
            visible: root.icon !== ""
            sourceSize.width: 32
            sourceSize.height: 32
            fillMode: Image.PreserveAspectFit
            Layout.preferredWidth: visible ? 32 : 0
            Layout.preferredHeight: visible ? 32 : 0
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            spacing: 1
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            Label {
                text: root.label
                // Plain: a row's label is a process name or a record's field,
                // somebody else's text, which AutoText would draw as markup.
                textFormat: Text.PlainText
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: root.description
                // Plain, and more exposed than the label: the Applications page
                // feeds it a record's `reason` and `executable`, written from
                // inside every GL and Vulkan process of the session (a Flatpak
                // game's too). Under AutoText a known tag makes the string rich,
                // and a rich Text loads <img> sources through the engine's
                // network access manager.
                textFormat: Text.PlainText
                visible: root.description !== ""
                opacity: 0.65
                font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Item {
            id: holder
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
