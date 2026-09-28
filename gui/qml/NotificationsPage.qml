// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Where a message appears, how large it is, and for how long.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    // notificationPreviewDisplay included: a persisted setting the page edits
    // belongs to its Reset.
    settings: ["notificationCorner", "notificationMargin", "notificationScale",
               "notificationSeconds", "notificationPreviewDisplay"]
    title: qsTr("Notifications")
    subtitle: qsTr("Where direct messages and mentions appear, and for how long.")

    ColumnLayout {
        anchors.fill: parent
        // A small step between the dropdown and its map, a large one before
        // the card, as on the Panel page.
        spacing: Theme.smallSpacing

        // Which display the map below depicts; the overlay is sized for the
        // largest, and the map says so when a smaller one is chosen.
        DisplayPicker {
            id: messageDisplayPicker
            objectName: "messageDisplayPicker"

            config: root.config
            selection: root.config.notificationPreviewDisplay
            accessibleLabel: qsTr("Display the message map depicts")
            onPicked: function(name) { root.config.notificationPreviewDisplay = name; }
        }

        // With messages switched off its marks are disabled rather than the
        // map faded, so the picture stays legible.
        NotificationScreen {
            config: root.config
            shownDisplay: messageDisplayPicker.shownDisplay
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        Card {
            Layout.fillWidth: true
            Layout.topMargin: Theme.mediumSpacing
            enabled: root.config.notificationsEnabled

            SettingRow {
                label: qsTr("Message size")
                description: qsTr("Independent of the voice panel.")
                first: true

                SliderRow {
                    accessibleName: qsTr("Message size")
                    from: 0.5; to: 3.0; stepSize: 0.05
                    suffix: "×"
                    value: root.config.notificationScale
                    defaultValue: root.config.defaultNotificationScale
                    onMoved: function(chosen) { root.config.notificationScale = chosen; }
                }
            }

            // The message's own distance from the edge, independent of the
            // panel's; the map's anchor marks move with it, placed by the same
            // arithmetic (vocem/placement.h).
            SettingRow {
                label: qsTr("Distance from the edge")
                description: qsTr("How close to the edge of the display a message may sit.")

                SliderRow {
                    accessibleName: qsTr("Distance from the edge")
                    from: 0; to: 120; stepSize: 1
                    decimals: 0
                    value: root.config.notificationMargin
                    defaultValue: root.config.defaultNotificationMargin
                    onMoved: function(chosen) { root.config.notificationMargin = chosen; }
                }
            }

            // How long a message stays is when it is drawn, not how it
            // looks, so it belongs here rather than under Appearance.
            SettingRow {
                label: qsTr("Show for")
                description: qsTr("How long a message stays before it fades.")

                SliderRow {
                    accessibleName: qsTr("Show for")
                    from: 1; to: 30; stepSize: 0.5
                    decimals: 1
                    suffix: qsTr(" s")
                    value: root.config.notificationSeconds
                    defaultValue: root.config.defaultNotificationSeconds
                    onMoved: function(chosen) { root.config.notificationSeconds = chosen; }
                }
            }
        }
    }
}
