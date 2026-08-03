// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One window: a status bar that always says what is going on, a navigation
// sidebar, and one section at a time in the content area.
//
// Deliberately not a tree of checkboxes. Two of the sections carry a model of the
// display, because where a box sits and how large it is against a screen are
// questions only a picture answers; the rest are lists of controls, because a
// picture of a colour or a padding is the control itself.
//
// Nothing ever requires a terminal. The bar at the top carries the daemon's state,
// the one sentence that says what to do about it, and the single button that does
// it, which is why DaemonStatus is part of the shared state rather than something
// the daemon only writes to its log.

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
    // Started with the session, the window has nothing to show: the tray icon is
    // the point. Without a tray to hide behind it shows itself anyway, rather than
    // running as a process nobody can see.
    property bool startHidden: false

    visible: !startHidden || !tray.available
    title: qsTr("Vocem Overlay")

    ConfigBridge { id: config }

    // Expected on the desktop, and it does the right thing already: closing puts
    // the window away behind its tray icon rather than ending the process.
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

    // Closing with keep-running on puts the window away behind the tray icon,
    // and the overlay stays up -- the one case in which it does. Every other
    // close is a Quit in full: with keep-running off by the user's word, or with
    // no tray to hide behind (where hiding would strand the process with no
    // interface), the close stops the daemon -- taking the overlay out of every
    // running game -- and ends the process, exactly the sequence the tray's own
    // Quit runs. Reopening the application brings the whole overlay back.
    onClosing: function(close) {
        if (tray.available && config.keepRunning) {
            close.accepted = false;
            window.hide();
        } else {
            config.quitOverlay();
        }
    }

    // Which section is open. A property of the window rather than of the sidebar
    // so that the development screenshot path can step through them.
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

            // The state, as a mark that changes shape as well as colour -- the
            // HIG's rule that colour is never the only carrier, and the same
            // rule the overlay's ring lives by. A filled disc when the daemon is
            // connected, a hollow ring while it is waiting for something, a
            // square when it is not running at all (the shape every media
            // player means "stopped" by) -- and a halo that pulses while
            // something is pending, because "waiting" and "stuck" look
            // identical otherwise.
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

                    // From the state rather than from the sentence beside it: the
                    // sentence is translated, and comparing against a translation is
                    // a comparison that stops being true in every language but one.
                    readonly property bool stopped: config.state === ConfigBridge.NotRunning
                    readonly property bool settled: config.state === ConfigBridge.Connected
                    // Not readonly: a Behavior animates writes, and a readonly
                    // property cannot be written even by its own animation.
                    property color stateColour:
                        settled ? Theme.online : (stopped ? Theme.offline : Theme.busy)

                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    // The square keeps a whisper of corner so it reads as drawn
                    // rather than as unfinished.
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

            // The two switches that decide whether anything is drawn at all, and
            // the only ones up here. They are worth reaching whatever section is
            // open, and they are not questions of appearance -- everything that
            // is lives under Appearance.
            //
            // They are also the only Switches left in the window. The KDE
            // guidelines reserve a switch for a control that takes effect the
            // moment it is clicked and ask for a checkbox otherwise, and these
            // two are exactly that: written straight to the file by
            // persistNow(), reaching a running game within a couple of seconds
            // with no Apply in between. Every boolean on a page under an Apply
            // bar waits for that button, so every one of them is a checkbox
            // now -- they were switches, promising an immediacy the code does
            // not have.
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

                // Each entry names the icons it will accept, best first. They are
                // all `preferences` artwork that Breeze carries at 22, which is the
                // size drawn: two of the earlier names existed only at 32 and were
                // downscaled from the full-colour System Settings artwork, which is
                // what made a column of identically laid out rows look ragged.
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
                        // "Applications", because the page lists everything the
                        // overlay has been loaded into, not only the games it
                        // draws in -- and "Window" beside it, because two entries
                        // called Applications and Application would be a reading
                        // test.
                        { title: qsTr("Applications"),
                          icons: ["applications-games", "input-gaming"] },
                        { title: qsTr("Window"),
                          icons: ["preferences-other", "preferences-desktop"] },
                        // Beside Window, because both are about what this
                        // application does with itself rather than about what
                        // the overlay does in a game -- and after it, so the
                        // sections that were already numbered keep their
                        // numbers as far as the harness is concerned.
                        // Not the bell: that is the Notifications section's
                        // icon, and two entries wearing one picture is a column
                        // you have to read twice.
                        { title: qsTr("System tray"),
                          icons: ["preferences-desktop-plasma", "preferences-desktop"] },
                        // Last of the sections that say something rather than
                        // set something: what the overlay is doing, and what it
                        // left behind. The crash pop-up's successor lives here.
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

                // About sits at the bottom, away from the sections that change
                // something -- the same place Tokodon and Kasts put it. Its index
                // follows the list above rather than being written out, which is
                // what it was: a literal 5 beside a Repeater of five entries.
                SidebarItem {
                    text: qsTr("About")
                    iconName: config.icon(["dialog-information", "help-about"])
                    current: sidebar.currentIndex === sections.count
                    onClicked: sidebar.currentIndex = sections.count
                    Layout.fillWidth: true
                }
            }

            // Not a real ListView any more -- the entries are two separate groups --
            // so the current index lives here, and the arrow keys are wired by hand.
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
