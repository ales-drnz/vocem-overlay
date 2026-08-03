// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the window does with itself, as opposed to what the overlay does in a game.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

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
            description: qsTr("Closing the window hides it behind the tray icon, and the overlay keeps running. Turn this off and closing quits the application, which takes the overlay out of every running game until you open it again.")
            first: true

            CheckBox {
                checked: root.config.keepRunning
                onToggled: root.config.keepRunning = checked
                Accessible.name: qsTr("Keep running when closed")
            }
        }

        SettingRow {
            label: qsTr("Start at login")
            description: qsTr("Starts hidden, with only the tray icon. The overlay does not need this at login, because the session starts the daemon on its own. It matters after you quit, when opening this application is what brings the overlay back.")

            CheckBox {
                checked: root.config.startAtLogin
                onToggled: root.config.startAtLogin = checked
                Accessible.name: qsTr("Start at login")
            }
        }
    }
}
