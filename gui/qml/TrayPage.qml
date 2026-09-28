// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the icon in the system tray is a picture of.
//
// A section of its own rather than rows on the Window page: the tray is always
// on screen, and this page carries a picture.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["trayVoiceIcon"]
    // The title repeats the sidebar entry's words (see AboutPage).
    title: qsTr("System tray")
    subtitle: qsTr("What the icon in the panel shows. Whichever you choose, it is always the way back to this window.")

    // The tray as the panel will draw it, following the choice below at once;
    // the panel itself follows the saved answer, so it changes only on Apply.
    TaskbarPreview {
        objectName: "trayPreview"
        config: root.config
        voiceIcon: root.config.trayVoiceIcon
        Layout.fillWidth: true
    }

    // Two mutually exclusive answers: radio buttons, as the KDE guidelines give
    // for a short set where every option is worth seeing at once.
    ButtonGroup { id: choice }

    Card {
        title: qsTr("Icon")
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Your voice state")
            description: qsTr("Speaking, muted, deafened, silent in a channel, or not in one.")
            first: true

            RadioButton {
                objectName: "trayVoiceChoice"
                ButtonGroup.group: choice
                checked: root.config.trayVoiceIcon
                onToggled: root.config.trayVoiceIcon = true
                Accessible.name: qsTr("Show your voice state in the tray")
            }
        }

        SettingRow {
            label: qsTr("The application's icon")
            description: qsTr("The same picture whatever is happening. Your voice state stays private.")

            RadioButton {
                objectName: "trayApplicationChoice"
                ButtonGroup.group: choice
                checked: !root.config.trayVoiceIcon
                onToggled: root.config.trayVoiceIcon = false
                Accessible.name: qsTr("Show the application's own icon in the tray")
            }
        }
    }
}
