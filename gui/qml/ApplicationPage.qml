// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the window does with itself, as opposed to what the overlay does in a game.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["keepRunning", "startAtLogin"]
    title: qsTr("Window")
    subtitle: qsTr("What happens when you close this window, and whether it opens at login.")

    Card {
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Keep running when closed")
            description: qsTr("Closing hides the window and the overlay keeps running. With this off, closing quits and the overlay leaves every game.")
            first: true

            CheckBox {
                checked: root.config.keepRunning
                onToggled: root.config.keepRunning = checked
                Accessible.name: qsTr("Keep running when closed")
            }
        }

        SettingRow {
            label: qsTr("Start at login")
            description: qsTr("Starts hidden, with only the tray icon. The overlay itself does not need this: the session starts it.")

            CheckBox {
                checked: root.config.startAtLogin
                onToggled: root.config.startAtLogin = checked
                Accessible.name: qsTr("Start at login")
            }
        }
    }
}
