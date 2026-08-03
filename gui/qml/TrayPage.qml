// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the icon in the system tray is a picture of.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.
//
// A section rather than two more rows on the Window page, on the owner's ask:
// the tray is the one part of this application that is on screen all the time,
// it is the only way back to a window that has been put away, and this page
// carries a picture -- which is the line the rest of the window already draws
// between a section and a row.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["trayVoiceIcon"]
    title: qsTr("System Tray")
    subtitle: qsTr("What the icon in the panel shows. Whichever you choose, it is always the way back to this window.")

    // The tray as the panel will draw it, following the choice below on the
    // spot. The panel itself follows the saved answer, not this one -- a control
    // that reached out and changed the desktop before Apply would be the one
    // setting here that cannot be tried out.
    TaskbarPreview {
        objectName: "trayPreview"
        config: root.config
        voiceIcon: root.config.trayVoiceIcon
        Layout.fillWidth: true
    }

    // Two mutually exclusive answers, each with the reason somebody would want
    // it, which is what the KDE guidelines give radio buttons for: a short set
    // where every option is worth seeing at once.
    ButtonGroup { id: choice }

    Card {
        title: qsTr("Icon")
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Your voice state")
            description: qsTr("Shows whether you are speaking, muted, deafened, in a channel and silent, or not in a channel. A glance at the panel tells you if you are still muted.")
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
            description: qsTr("The same picture whatever is happening. Nobody can read your voice state over your shoulder, or on a stream.")

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
