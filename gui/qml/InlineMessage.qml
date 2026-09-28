// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A banner that says something is wrong, in the place it is wrong about.
//
// KDE's HIG inline message, without depending on Kirigami: three severities in
// Breeze's colours (Theme.palette.highlight, Theme.busy, Theme.offline), never
// colour alone -- an icon and the words carry the meaning too. Shown only when
// there is something to say.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Rectangle {
    id: root

    enum Severity { Information, Warning, Error }

    property int severity: InlineMessage.Severity.Information
    property string text: ""
    // The action's words, empty for a banner that only informs.
    property string actionText: ""
    signal actionTriggered()

    readonly property color accent:
        severity === InlineMessage.Severity.Error ? Theme.offline
        : severity === InlineMessage.Severity.Warning ? Theme.busy
        : Theme.palette.highlight

    readonly property string iconName:
        severity === InlineMessage.Severity.Error ? "dialog-error"
        : severity === InlineMessage.Severity.Warning ? "dialog-warning"
        : "dialog-information"

    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + Theme.cardPadding * 2
    radius: Theme.cornerRadius
    color: Qt.rgba(accent.r, accent.g, accent.b, 0.12)
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.45)
    border.width: 1

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    RowLayout {
        id: layout

        anchors.fill: parent
        anchors.margins: Theme.cardPadding
        spacing: Theme.mediumSpacing

        Image {
            source: "image://icon/" + root.iconName
            sourceSize.width: 22
            sourceSize.height: 22
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignTop
        }

        Label {
            text: root.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
        }

        Button {
            text: root.actionText
            visible: root.actionText !== ""
            Layout.alignment: Qt.AlignVCenter
            onClicked: root.actionTriggered()
        }
    }
}
