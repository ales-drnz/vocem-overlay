// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The gaps inside the boxes.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["boxPaddingX", "boxPaddingY", "avatarGap", "rowSpacing"]
    title: qsTr("Spacing")
    subtitle: qsTr("The gaps inside both boxes, on a 1080p display. They grow with the panel size.")

    // Both boxes, live, beside the controls -- every gap edited on this page
    // moves them on the spot. The same components the geometry comparison
    // measures, fed by the bridge's edited copy.
    side: LivePreview {
        objectName: "spacingLive"
        anchors.fill: parent
        config: root.config
    }

    Card {
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Padding, left and right")
            description: qsTr("Horizontal padding inside both boxes.")
            first: true

            SpinRow {
                accessibleName: qsTr("Padding, left and right")
                from: 0; to: 48
                value: root.config.boxPaddingX
                defaultValue: root.config.defaultBoxPaddingX
                onMoved: function(chosen) { root.config.boxPaddingX = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Padding, top and bottom")
            description: qsTr("Vertical padding inside both boxes.")

            SpinRow {
                accessibleName: qsTr("Padding, top and bottom")
                from: 0; to: 48
                value: root.config.boxPaddingY
                defaultValue: root.config.defaultBoxPaddingY
                onMoved: function(chosen) { root.config.boxPaddingY = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Avatar to name")
            description: qsTr("Gap between an avatar and the name beside it.")

            SpinRow {
                accessibleName: qsTr("Avatar to name")
                from: 0; to: 48
                value: root.config.avatarGap
                defaultValue: root.config.defaultAvatarGap
                onMoved: function(chosen) { root.config.avatarGap = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Between people")
            description: qsTr("Gap between participants.")

            SpinRow {
                accessibleName: qsTr("Between people")
                from: 0; to: 48
                value: root.config.rowSpacing
                defaultValue: root.config.defaultRowSpacing
                onMoved: function(chosen) { root.config.rowSpacing = chosen; }
            }
        }
    }
}
