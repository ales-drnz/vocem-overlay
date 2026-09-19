// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The two boxes, live, beside the controls that edit them.
//
// The same PanelPreview and NotificationPreview the maps use -- the components
// scripts/compare-preview.py holds against the overlay's own geometry -- drawn
// unchanged from the bridge's edited copy, so every control on the page moves
// them on the spot, before Apply. Outside the scrolling column on purpose: the
// preview answers for whichever control was just edited, wherever that control
// has scrolled to.
//
// Each box is drawn at the overlay's reference size and scaled as a whole
// (never laid out small; PanelPreview says why), to fit this column and never
// above 1 -- at 1 a box is the size the overlay draws it on 1080 lines with
// the size settings at 1. The column is not a model of the display: how large
// the boxes are against a screen is the Panel and Notifications pages'
// question, and the caption says so.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property var config

    implicitWidth: 248

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.smallSpacing

        Label {
            text: qsTr("Preview")
            font.bold: true
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("Follows every edit before you apply it. Each box is scaled to fit this column. To see their size against the display, use the Panel and Notifications pages.")
            opacity: 0.65
            font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // The voice panel, on a token of a dark game scene.
        Rectangle {
            id: panelScene

            gradient: Theme.gameBackdrop
            radius: Theme.cornerRadius
            border.color: Qt.rgba(1, 1, 1, 0.12)
            border.width: 1
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            Layout.topMargin: Theme.smallSpacing

            readonly property real room: width - Theme.cardPadding * 2
            readonly property real roomY: height - Theme.cardPadding * 2

            Item {
                id: panelWrap
                objectName: "panel"

                readonly property real factor:
                    Math.min(1,
                             panelScene.room / Math.max(1, panelContent.implicitWidth),
                             panelScene.roomY / Math.max(1, panelContent.implicitHeight))

                anchors.centerIn: parent
                // Dimmed with the switch, as the message preview below is.
                opacity: root.config.panelEnabled ? 1.0 : 0.35
                width: panelContent.implicitWidth * factor
                height: panelContent.implicitHeight * factor

                PanelPreview {
                    id: panelContent

                    config: root.config
                    transformOrigin: Item.TopLeft
                    scale: panelWrap.factor
                    width: implicitWidth
                    height: implicitHeight
                }
            }
        }

        // The message box, on the same terms. Dimmed, as the maps dim it, while
        // notifications are switched off.
        Rectangle {
            id: messageScene

            gradient: Theme.gameBackdrop
            radius: Theme.cornerRadius
            border.color: Qt.rgba(1, 1, 1, 0.12)
            border.width: 1
            clip: true
            Layout.fillWidth: true
            implicitHeight: messageWrap.height + Theme.cardPadding * 2

            readonly property real room: width - Theme.cardPadding * 2

            Item {
                id: messageWrap
                objectName: "message"

                readonly property real factor:
                    Math.min(1, messageScene.room / Math.max(1, messageContent.implicitWidth))

                anchors.centerIn: parent
                width: messageContent.implicitWidth * factor
                height: messageContent.implicitHeight * factor
                opacity: root.config.notificationsEnabled ? 1.0 : 0.35

                NotificationPreview {
                    id: messageContent

                    config: root.config
                    transformOrigin: Item.TopLeft
                    scale: messageWrap.factor
                    width: implicitWidth
                    height: implicitHeight
                }
            }
        }
    }
}
