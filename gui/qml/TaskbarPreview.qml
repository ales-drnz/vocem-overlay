// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The tray, as the panel will draw it.
//
// A stand-in for a system tray rather than a model of one: unlike the two maps,
// nothing here is to scale against anything, because a panel's size is the
// desktop's business and not this application's. What it has to be right about
// is the one thing the setting changes -- which picture sits in the row -- and
// the size that picture is drawn at, which is 22 pixels, the size Tray.qml asks
// the theme for and the size Breeze carries this artwork at.
//
// The neighbours are there because an icon alone tells you nothing: the question
// somebody is answering on this page is "will I be able to tell which one is
// mine, and will it say something I would rather it did not". So the row holds a
// few of the things that are actually in a tray on this desktop, drawn from the
// session's own icon theme, and ours among them.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property var config

    // Which picture the tray wears. Bound to the edited setting, so the choice
    // moves this on the spot; the panel itself follows the saved one
    // (ConfigBridge::appliedTrayVoiceIcon says why).
    property bool voiceIcon: true

    // The state to draw the icon in. The live one by default -- this page is
    // about a thing on the panel right now, and showing anything else would be
    // a picture of somebody else's session -- and pinned by the legend below,
    // which walks all five so the meanings are readable without being in a call.
    property int state: root.config.selfVoice

    implicitHeight: layout.implicitHeight

    // How many of this page's own pictures actually came up: the five states in
    // the legend and the one in the strip. An Image whose source will not load
    // keeps the size its layout gave it and paints nothing, so a geometry dump
    // cannot tell a drawn icon from a missing one -- and missing is exactly what
    // they were until the artwork was carried in the binary (gui/CMakeLists.txt
    // says why). Recomputed on every change rather than counted up, so it cannot
    // drift when the choice changes the strip's source.
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

    // The strip. Darker than the window and squared off at the bottom, because a
    // panel sits against an edge of the screen rather than floating in a page --
    // the shape is what makes it read as somebody's taskbar and not as another
    // card.
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

            // A few of the desktop's own, so ours has something to be told apart
            // from. Names the session's theme is certain to carry; one that is
            // missing simply leaves a gap rather than a broken-image mark.
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

            // Ours. Named so the geometry dump can say which picture the page
            // put in the tray, which is the whole of what this setting decides.
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

                // The frame a tray puts round the item the pointer is on, so the
                // eye finds ours in the row without an arrow pointing at it.
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

            // A clock, because every panel has one and it is what makes the
            // strip read as a panel at a glance. Fixed text: a running clock in
            // a settings window is a thing that moves for no reason.
            Label {
                text: "20:45"
                opacity: 0.55
                Layout.leftMargin: Theme.smallSpacing
            }
        }
    }

        // What the picture in the strip can say, all five of them, at the size
        // the panel draws them. The strip alone shows whichever state you happen
        // to be in while you are reading this page -- which is usually "not in a
        // channel" -- so on its own it answers "will I see an icon" and not
        // "what will it tell me". Only while the state is what the icon carries:
        // with the application's own picture chosen there is nothing to read.
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
                    // Read by recount() above; the entry has no other reason to
                    // know its own picture's state.
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
