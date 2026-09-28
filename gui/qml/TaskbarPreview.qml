// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The tray, as the panel will draw it: a stand-in, not to scale, right about
// which picture sits in the row and its 22-pixel size (what Tray.qml asks the
// theme for). A few neighbours from the session's icon theme sit beside ours,
// so the preview shows whether it can be told apart.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property var config

    // Which picture the tray wears: the edited setting, while the panel itself
    // follows the saved one (see ConfigBridge::appliedTrayVoiceIcon).
    property bool voiceIcon: true

    // The state to draw the icon in: the live one. The legend below shows all
    // five, so the meanings are readable without being in a call.
    property int state: root.config.selfVoice

    implicitHeight: layout.implicitHeight

    // How many of this page's pictures loaded: the five in the legend and the
    // one in the strip. An Image that fails keeps its size and paints nothing,
    // so the geometry dump needs this to tell a drawn icon from a missing one
    // (gui/CMakeLists.txt carries the artwork). Recomputed, not counted up, so
    // it cannot drift when the strip's source changes.
    property int loadedIcons: 0

    function recount() {
        let total = ours.status === Image.Ready ? 1 : 0;
        for (let i = 0; i < legend.count; ++i) {
            const entry = legend.itemAt(i);
            if (entry && entry.iconStatus === Image.Ready) {
                ++total;
            }
        }
        root.loadedIcons = total;
    }

    Component.onCompleted: root.recount()

    function iconFor(state) {
        switch (state) {
        case ConfigBridge.Speaking: return "io.github.ales_drnz.vocem_overlay-speaking";
        case ConfigBridge.Muted: return "io.github.ales_drnz.vocem_overlay-muted";
        case ConfigBridge.Deafened: return "io.github.ales_drnz.vocem_overlay-deafened";
        case ConfigBridge.InChannelIdle: return "io.github.ales_drnz.vocem_overlay-idle";
        default: return "io.github.ales_drnz.vocem_overlay-offline";
        }
    }

    function nameFor(state) {
        switch (state) {
        case ConfigBridge.Speaking: return qsTr("Speaking");
        case ConfigBridge.Muted: return qsTr("Muted");
        case ConfigBridge.Deafened: return qsTr("Deafened");
        case ConfigBridge.InChannelIdle: return qsTr("In a channel, not speaking");
        default: return qsTr("Not in a channel");
        }
    }

    // The strip, darker than the window, so it reads as a taskbar and not as
    // another card.
    ColumnLayout {
        id: layout

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Theme.smallSpacing

    Rectangle {
        id: strip

        Layout.fillWidth: true
        implicitHeight: row.implicitHeight + Theme.mediumSpacing * 2
        radius: Theme.cornerRadius
        color: Theme.dark ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(0, 0, 0, 0.10)
        border.color: Theme.cardBorder
        border.width: 1

        Accessible.role: Accessible.Graphic
        Accessible.name: root.voiceIcon
                         ? qsTr("A system tray showing the overlay's icon as your voice state: %1")
                               .arg(root.nameFor(root.state))
                         : qsTr("A system tray showing the overlay's own application icon")

        RowLayout {
            id: row

            anchors.fill: parent
            anchors.leftMargin: Theme.cardPadding
            anchors.rightMargin: Theme.cardPadding
            anchors.topMargin: Theme.mediumSpacing
            anchors.bottomMargin: Theme.mediumSpacing
            spacing: Theme.mediumSpacing

            Item { Layout.fillWidth: true }

            // A few of the desktop's own, so ours has something to be told
            // apart from; a missing one leaves a gap, not a broken-image mark.
            Repeater {
                model: [["preferences-system-network", "network"],
                        ["audio-volume-high", "volume"],
                        ["preferences-system-bluetooth", "bluetooth"]]

                Image {
                    required property var modelData
                    source: "image://icon/" + root.config.icon([modelData[0], modelData[1]])
                    sourceSize.width: 22
                    sourceSize.height: 22
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    opacity: 0.55
                }
            }

            // Ours, named so the geometry dump says which picture is shown.
            Image {
                id: ours

                objectName: root.voiceIcon ? "trayVoiceIcon" : "trayApplicationIcon"

                source: "image://icon/" + (root.voiceIcon ? root.iconFor(root.state)
                                                          : "io.github.ales_drnz.vocem_overlay")
                sourceSize.width: 22
                sourceSize.height: 22
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                onStatusChanged: root.recount()

                // The frame a tray puts round a hovered item, so the eye finds
                // ours in the row.
                Rectangle {
                    anchors.centerIn: parent
                    width: 30
                    height: 30
                    z: -1
                    radius: Theme.cornerRadius / 2
                    color: Qt.rgba(Theme.palette.highlight.r, Theme.palette.highlight.g,
                                   Theme.palette.highlight.b, 0.22)
                    border.width: 1
                    border.color: Qt.rgba(Theme.palette.highlight.r, Theme.palette.highlight.g,
                                          Theme.palette.highlight.b, 0.5)
                    antialiasing: true
                }
            }

            // A clock, so the strip reads as a panel at a glance. Fixed text: a
            // running clock would move for no reason.
            Label {
                text: "20:45"
                opacity: 0.55
                Layout.leftMargin: Theme.smallSpacing
            }
        }
    }

        // All five states the icon can show, at the panel's size, since the
        // strip shows only the current one. Hidden when the application's own
        // picture is chosen.
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: Theme.smallSpacing
            visible: root.voiceIcon
            spacing: Theme.largeSpacing

            Repeater {
                id: legend

                model: [ConfigBridge.Speaking, ConfigBridge.InChannelIdle, ConfigBridge.Muted,
                        ConfigBridge.Deafened, ConfigBridge.NotInChannel]

                Row {
                    required property var modelData
                    // Read by recount() above.
                    readonly property int iconStatus: mark.status

                    spacing: Theme.smallSpacing

                    Image {
                        id: mark
                        source: "image://icon/" + root.iconFor(modelData)
                        sourceSize.width: 22
                        sourceSize.height: 22
                        width: 22
                        height: 22
                        anchors.verticalCenter: parent.verticalCenter
                        onStatusChanged: root.recount()
                    }

                    Label {
                        text: root.nameFor(modelData)
                        opacity: 0.7
                        font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }
}
