// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this is, what is installed, and where the settings live.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    // No settings here, so no bar along the bottom. The title repeats the
    // sidebar entry's words, as the KDE guidelines ask of a page heading.
    title: qsTr("About Vocem Overlay")
    subtitle: qsTr("Shows your Discord voice channel inside the game, through a Vulkan layer and an OpenGL hook. Not affiliated with Discord Inc.")

    // The version at the top: it is what an About page is opened for.
    RowLayout {
        spacing: Theme.mediumSpacing
        Layout.fillWidth: true
        Layout.leftMargin: Theme.cardPadding

        Image {
            source: "image://icon/io.github.ales_drnz.vocem_overlay"
            sourceSize.width: 48
            sourceSize.height: 48
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
            fillMode: Image.PreserveAspectFit
        }

        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true

            Label {
                text: qsTr("Version %1").arg(root.config.version)
                font.bold: true
            }
            Label {
                text: qsTr("BSD 3-Clause licence")
                opacity: 0.7
                font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
            }
        }
    }

    Card {
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Settings file")
            // Shown as the description, with a button that copies it.
            description: root.config.configPath

            // The copy's source, zero-sized as well as invisible: SettingRow
            // sizes its control column from its children, and a path-wide item
            // would push the button off the right edge.
            TextEdit {
                id: pathHolder
                visible: false
                width: 0
                height: 0
                text: root.config.configPath
            }

            Button {
                icon.name: root.config.icon(["edit-copy"])
                display: AbstractButton.IconOnly
                text: qsTr("Copy the path to the settings file")
                ToolTip.text: text
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: {
                    pathHolder.selectAll();
                    pathHolder.copy();
                    pathHolder.deselect();
                }
            }
        }

        SettingRow {
            label: qsTr("Discord permission")
            description: qsTr("Discard the stored token and request authorisation again.")

            Button {
                text: qsTr("Reconnect")
                enabled: root.config.state !== ConfigBridge.Working
                onClicked: root.config.reauthorise()
            }
        }
    }
}
