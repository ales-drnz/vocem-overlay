// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One setting: what it is on the left, the control that changes it on the right,
// and a line underneath saying what it does. The description is part of the row
// rather than a tooltip, because a setting whose effect has to be discovered by
// hovering is a setting nobody finds.
//
// Two lines and no more. There used to be a third, for a warning a row might have
// to carry, kept in the layout at zero opacity so that showing it would not change
// the row's height -- which meant every row in the window was one blank line taller
// than its text, and the pair of lines sat visibly above the middle of the control
// beside them. What that line had to say is said by the description instead, which
// can change with the setting.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property string label
    property string description: ""
    property bool first: false
    // A picture in front of the label, for rows that stand for something with a
    // face of its own -- an application. Left empty by everything else, and then
    // it takes no room at all.
    property string icon: ""
    // Raw material behind the row, shown on hover -- the application rows put the
    // record's own evidence token here. A tooltip and not a third line: the
    // description already carries the meaning in words, and this is for quoting
    // into a log or a report, not for everybody's eyes. Settings whose effect
    // needs explaining still use the description; a tooltip is never the only
    // place something is said.
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

    // Rows are divided by a line rather than by gaps, which keeps the card reading
    // as one object. The first row has nothing above it to divide from.
    Rectangle {
        visible: !root.first
        width: parent.width
        height: 1
        color: Theme.separator
    }

    RowLayout {
        id: layout

        // The padded content of one block in a card, under the name every
        // other such block carries, so the padding check can measure all of
        // them at once (Card.qml says what it checks).
        objectName: "cardContent"

        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadding
        anchors.rightMargin: Theme.cardPadding
        anchors.topMargin: Theme.cardPadding
        anchors.bottomMargin: Theme.cardPadding
        spacing: Theme.mediumSpacing

        // Centred against the control rather than filling the row, so the label
        // and its description sit in the middle of the row's height whether there
        // are one line or two of them.
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
                // which is somebody else's text, and AutoText would draw a
                // name that looks like markup as markup.
                textFormat: Text.PlainText
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: root.description
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
