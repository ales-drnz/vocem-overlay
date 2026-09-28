// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The settings window: a status bar that always says what is going on, a
// navigation sidebar, and one section at a time in the content area.
//
// Two sections carry a model of the display, because where a box sits and how
// large it is are questions only a picture answers; the rest are lists of
// controls. Nothing requires a terminal: the bar carries the daemon's state,
// what to do about it, and the button that does it, which is why DaemonStatus
// is part of the shared state rather than only the daemon's log.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ApplicationWindow {
    id: window

    objectName: "settings"

    width: 1160
    height: 720
    minimumWidth: 920
    minimumHeight: 580
    // Started with the session, the window stays hidden behind the tray icon;
    // without a tray it shows itself rather than run invisibly.
    property bool startHidden: false

    visible: !startHidden || !tray.available
    title: qsTr("Vocem Overlay")

    ConfigBridge { id: config }

    // The desktop's close shortcut; what a close does is decided in onClosing.
    Shortcut {
        sequences: [StandardKey.Close]
        onActivated: window.close()
    }

    Tray {
        id: tray
        config: config
        onOpenRequested: {
            window.show();
            window.raise();
            window.requestActivate();
        }
    }

    // With keep-running on and a tray, closing hides the window and the overlay
    // stays up. Every other close is a full Quit, as the tray's Quit runs it:
    // the daemon stops, taking the overlay out of every running game, and the
    // process ends. Without a tray, hiding would strand an invisible process.
    onClosing: function(close) {
        if (tray.available && config.keepRunning) {
            close.accepted = false;
            window.hide();
        } else {
            config.quitOverlay();
        }
    }

    // The open section; a window property so the screenshot harness can step
    // through them.
    property alias section: sidebar.currentIndex

    // ---------------------------------------------------------------- status bar
    header: Rectangle {
        implicitHeight: statusLayout.implicitHeight + Theme.mediumSpacing * 2
        color: Theme.headerColour

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.separator
        }

        RowLayout {
            id: statusLayout
            anchors.fill: parent
            anchors.leftMargin: Theme.pageMargin
            anchors.rightMargin: Theme.pageMargin
            anchors.topMargin: Theme.mediumSpacing
            anchors.bottomMargin: Theme.mediumSpacing
            spacing: Theme.mediumSpacing

            // The state as a mark that changes shape as well as colour (colour
            // is never the only carrier): a filled disc when connected, a
            // hollow ring while waiting, a square when not running, and a
            // pulsing halo while something is pending, so "waiting" does not
            // look like "stuck".
            Item {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22

                Rectangle {
                    id: halo
                    anchors.centerIn: parent
                    width: 22
                    height: 22
                    radius: 11
                    color: dot.stateColour
                    opacity: 0.0

                    SequentialAnimation on opacity {
                        running: config.actionAvailable || config.state === ConfigBridge.Waiting
                        loops: Animation.Infinite
                        NumberAnimation { from: 0.30; to: 0.0; duration: 1100; easing.type: Easing.OutQuad }
                        PauseAnimation { duration: 300 }
                    }
                }

                Rectangle {
                    id: dot

                    // From the state, not the translated sentence beside it.
                    readonly property bool stopped: config.state === ConfigBridge.NotRunning
                    readonly property bool settled: config.state === ConfigBridge.Connected
                    // Not readonly: a Behavior animates writes, and a readonly
                    // property cannot be written even by its own animation.
                    property color stateColour:
                        settled ? Theme.online : (stopped ? Theme.offline : Theme.busy)

                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    // A little corner, so the square reads as drawn.
                    radius: stopped ? 2 : 5
                    color: settled || stopped ? stateColour : "transparent"
                    border.width: settled || stopped ? 0 : 2
                    border.color: stateColour

                    Behavior on stateColour {
                        ColorAnimation { duration: 150 }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: config.statusText
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: config.hintText
                    opacity: 0.7
                    font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Button {
                text: config.actionText
                visible: config.actionText !== ""
                enabled: config.actionAvailable
                highlighted: true
                onClicked: config.performAction()
            }

            // The two switches that decide whether anything is drawn at all,
            // reachable from every section. They are the window's only
            // Switches: the KDE guidelines keep a switch for a control that
            // takes effect on the click, and these are written straight to the
            // file and reach a running game within a couple of seconds, with no
            // Apply. Every setting under an Apply bar is a checkbox.
            Switch {
                text: qsTr("Voice panel")
                checked: config.panelEnabled
                onToggled: config.panelEnabled = checked
            }

            Switch {
                text: qsTr("Notifications")
                checked: config.notificationsEnabled
                onToggled: config.notificationsEnabled = checked
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------------------------------------------------------- sidebar
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: Theme.sidebarColour

            Rectangle {
                anchors.right: parent.right
                height: parent.height
                width: 1
                color: Theme.separator
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.mediumSpacing
                anchors.bottomMargin: Theme.mediumSpacing
                spacing: 0

                // Each entry names the icons it accepts, best first: all
                // `preferences` artwork Breeze carries at 22, the size drawn, so
                // no row is a downscaled full-colour icon.
                Repeater {
                    id: sections

                    model: [
                        { title: qsTr("Panel"),
                          icons: ["preferences-system-windows-move",
                                  "preferences-system-windows"] },
                        { title: qsTr("Notifications"),
                          icons: ["preferences-desktop-notification-bell",
                                  "preferences-system-notifications",
                                  "preferences-desktop-notification"] },
                        { title: qsTr("Appearance"),
                          icons: ["preferences-desktop-theme-global",
                                  "preferences-desktop-theme"] },
                        { title: qsTr("Spacing"),
                          icons: ["preferences-desktop-display"] },
                        { title: qsTr("People"),
                          icons: ["preferences-desktop-user", "system-users"] },
                        // "Applications": the page lists everything the overlay
                        // has been loaded into, not only games. "Window" beside
                        // it, not "Application", to keep the two apart.
                        { title: qsTr("Applications"),
                          icons: ["applications-games", "input-gaming"] },
                        { title: qsTr("Window"),
                          icons: ["preferences-other", "preferences-desktop"] },
                        // Beside Window: both are about this application itself.
                        // After it, so the earlier sections keep their indices
                        // for the harness. Not the bell, which is the
                        // Notifications section's icon.
                        { title: qsTr("System tray"),
                          icons: ["preferences-desktop-plasma", "preferences-desktop"] },
                        // Last of the sections that report rather than set: what
                        // the overlay is doing and what it left behind.
                        { title: qsTr("Debug"),
                          icons: ["utilities-log-viewer", "text-x-log",
                                  "applications-utilities"] }
                    ]

                    SidebarItem {
                        required property var modelData
                        required property int index

                        text: modelData.title
                        iconName: config.icon(modelData.icons)
                        current: sidebar.currentIndex === index
                        onClicked: sidebar.currentIndex = index
                        Layout.fillWidth: true
                    }
                }

                Item { Layout.fillHeight: true }

                // About at the bottom, away from the sections that change
                // something (as Tokodon and Kasts do). Its index follows the
                // list above.
                SidebarItem {
                    text: qsTr("About")
                    iconName: config.icon(["dialog-information", "help-about"])
                    current: sidebar.currentIndex === sections.count
                    onClicked: sidebar.currentIndex = sections.count
                    Layout.fillWidth: true
                }
            }

            // The entries are two separate groups, not one ListView, so the
            // current index lives here and the arrow keys are wired by hand.
            QtObject {
                id: sidebar
                property int currentIndex: 0
            }

            Keys.onUpPressed: sidebar.currentIndex = Math.max(0, sidebar.currentIndex - 1)
            Keys.onDownPressed:
                sidebar.currentIndex = Math.min(sections.count, sidebar.currentIndex + 1)
            activeFocusOnTab: true
        }

        // ---------------------------------------------------------------- content
        StackLayout {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sidebar.currentIndex

            PanelPage { config: config }
            NotificationsPage { config: config }
            AppearancePage { config: config }
            SpacingPage { config: config }
            PeoplePage { config: config }
            GamesPage { config: config }
            ApplicationPage { config: config }
            TrayPage { config: config }
            DebugPage { config: config }
            AboutPage { config: config }
        }
    }
}
