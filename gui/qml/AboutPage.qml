// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this is, what is installed, and where the settings live.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    // No settings here, so no bar along the bottom. The title repeats the words
    // of the entry it was reached from, which is what the KDE guidelines ask of a
    // page heading.
    title: qsTr("About Vocem Overlay")
    subtitle: qsTr("Shows your Discord voice channel inside the game, through a Vulkan layer and an OpenGL hook. Not affiliated with Discord Inc.")

    // The version at the top, beside the name, rather than in small
    // grey type at the foot of the page: it is the thing an About page
    // is opened for.
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
            // Selectable, and with the button that puts it on the
            // clipboard: a path nobody can copy is a path to be typed
            // out by hand.
            description: root.config.configPath

            // Zero-sized as well as invisible. SettingRow sizes its control column
            // from what is in it, and a hidden text item as wide as the path pushed
            // the copy button back into the middle of the row instead of leaving it
            // at the right edge with every other control in the window.
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
