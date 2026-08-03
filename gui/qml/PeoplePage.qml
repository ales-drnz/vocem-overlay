// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which participants the panel shows.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["onlySpeaking", "hideSelf", "showMutedState"]
    title: qsTr("People")
    subtitle: qsTr("Which participants the panel shows.")

    // No card heading: it would say "who to show", which is what the
    // line under the title already says.
    Card {
        Layout.fillWidth: true

        SettingRow {
            label: qsTr("Only people who are speaking")
            description: qsTr("Hide participants who are silent.")
            first: true

            CheckBox {
                checked: root.config.onlySpeaking
                onToggled: root.config.onlySpeaking = checked
                Accessible.name: qsTr("Only people who are speaking")
            }
        }

        SettingRow {
            label: qsTr("Hide myself")
            description: qsTr("Exclude your own account from the list.")

            CheckBox {
                checked: root.config.hideSelf
                onToggled: root.config.hideSelf = checked
                Accessible.name: qsTr("Hide myself")
            }
        }

        SettingRow {
            label: qsTr("Show muted and deafened")
            description: qsTr("Mark muted and deafened participants on their avatar.")

            CheckBox {
                checked: root.config.showMutedState
                onToggled: root.config.showMutedState = checked
                Accessible.name: qsTr("Show muted and deafened")
            }
        }
    }
}
