// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A banner that says something is wrong, in the place it is wrong about.
//
// KDE's HIG calls this an inline message and reserves it for "messages that
// should get the user's attention but not interrupt their current task", with
// three severities carrying three colours -- highlight for the benign, neutral
// for warnings, negative for errors -- and one rule over the top of them:
// never colour alone, so an icon and the words carry the meaning too. That is
// this project's own rule about the speaking ring, arriving from the other
// direction.
//
// Kirigami has this component and this window does not depend on Kirigami:
// the application is Qt Quick Controls with the desktop style, and adding a
// framework for one banner is not a trade worth making. So the shape is
// borrowed and the implementation is the desktop style's own palette, which
// is where those three colours live anyway (Theme.online / busy / offline are
// Breeze's Positive, Neutral and Negative).
//
// Shown only when there is something to say: a page with no banner is a page
// where nothing is wrong, which is worth more than a green "all fine" box.

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
