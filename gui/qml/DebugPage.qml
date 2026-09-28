// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The Debug section: what the overlay is doing, what it left behind, and what
// the daemon said. The overlay fails silently, so this is where "is it working,
// and if not, where does it stop" gets answered.
//
// Three tabs (now / sessions / daemon log); the page opens on the one with
// something to say. A banner appears only when something is wrong, in colour,
// icon and words together. Sessions are a recycling ListView whose journal
// text is loaded only for an open row. Empty views show a placeholder message.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    settings: []
    title: qsTr("Debug")
    subtitle: qsTr("What the overlay is doing right now, and what it left behind.")

    // The page's one action: it empties the Sessions list, both kinds, and says
    // so rather than "Reset", which elsewhere means "put the settings back".
    // Enabled only when there is something to clear. The daemon's log is
    // journald's and not touched.
    barContent: Button {
        objectName: "debugClear"
        text: qsTr("Clear Journals")
        icon.name: "edit-clear-history"
        enabled: root.config.crashReports.length > 0 ||
                 root.config.journalHistory.length > 0
        onClicked: root.config.clearJournals()
    }

    // The daemon's journald lines are fetched when the page is shown, not on a
    // timer: this page is the only reader.
    onVisibleChanged: {
        if (visible) {
            root.config.refreshDaemonLog();
            tabs.currentIndex = root.openingTab();
        }
    }

    // A crash waiting opens Sessions; anything else opens on Now.
    function openingTab() {
        return root.config.crashReports.length > 0 ? 1 : 0;
    }

    // The daemon publishes a segment this window cannot read; used by the
    // banner and the Shared state row.
    readonly property bool abiMismatch:
        config.segmentAbiVersion !== 0 && config.segmentAbiVersion !== config.abiVersion

    // A missing segment is not unhealthy: it means the daemon is stopped, which
    // the header already says (entry 108).
    readonly property bool healthy:
        config.vulkanLayerInstalled && config.openglPreloadActive && !abiMismatch

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.mediumSpacing

        // ---- the one banner, only when something is wrong
        InlineMessage {
            objectName: "debugBanner"
            // Not before the preload question is answered: half of it is an
            // asynchronous systemctl call.
            visible: root.config.openglPreloadKnown && !root.healthy
            Layout.fillWidth: true
            severity: root.abiMismatch ? InlineMessage.Severity.Error
                                       : InlineMessage.Severity.Warning
            // The three causes in reading order; together they are exactly what
            // `healthy` is false for, so the last is a real statement.
            text: {
                if (root.abiMismatch) {
                    return qsTr("The daemon publishes shared state v%1 and this window "
                                + "expects v%2. One half is from an older install. "
                                + "Games keep an empty overlay until the mismatched "
                                + "half is updated and restarted.")
                               .arg(root.config.segmentAbiVersion).arg(root.config.abiVersion);
                }
                if (!root.config.vulkanLayerInstalled) {
                    return qsTr("The Vulkan layer is not installed. Vulkan games cannot show "
                                + "the overlay, and that is most of them, Proton "
                                + "included.");
                }
                return qsTr("The OpenGL preload is not in this session's environment. "
                            + "It reaches processes started after the next login; until "
                            + "then, native OpenGL games have no overlay.");
            }
        }

        TabBar {
            id: tabs

            objectName: "debugTabs"
            Layout.fillWidth: true

            TabButton { text: qsTr("Now") }
            TabButton {
                text: root.config.crashReports.length > 0
                      ? qsTr("Sessions (%1)").arg(root.config.crashReports.length)
                      : qsTr("Sessions")
            }
            TabButton { text: qsTr("Daemon Log") }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ------------------------------------------------------------ now
            //
            // Scrolls, because "Drawing Now" has one row per process the
            // overlay paints in.
            ScrollView {
                id: nowView

                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: nowView.availableWidth
                    // At least the viewport, so the placeholder can centre itself
                    // with Layout.fillHeight on an empty tab.
                    height: Math.max(implicitHeight, nowView.availableHeight)
                    spacing: Theme.largeSpacing

                    Card {
                        title: qsTr("Health")
                        Layout.fillWidth: true

                        SettingRow {
                            label: qsTr("Vulkan layer")
                            description: root.config.vulkanLayerInstalled
                                         ? qsTr("Installed. Every Vulkan game loads it by itself.")
                                         : qsTr("Not found. Vulkan games cannot show the overlay.")
                            first: true

                            Label {
                                text: root.config.vulkanLayerInstalled ? qsTr("Ready")
                                                                       : qsTr("Missing")
                                color: root.config.vulkanLayerInstalled ? Theme.online
                                                                        : Theme.offline
                                font.bold: true
                            }
                        }

                        SettingRow {
                            label: qsTr("OpenGL preload")
                            // Three answers: the service manager carrying the
                            // preload is not this session having it, since a
                            // program started from the desktop inherits the
                            // environment fixed at login.
                            description: !root.config.openglPreloadKnown
                                         ? qsTr("Asking the session's service manager.")
                                         : root.config.openglPreloadActive
                                           ? qsTr("Active in this session.")
                                           : root.config.openglPreloadInManager
                                             ? qsTr("The service manager has it, and the desktop "
                                                    + "this window was started from does not. "
                                                    + "Programs started after the next login "
                                                    + "get it.")
                                             : qsTr("Not in this session's environment. It "
                                                    + "reaches new sessions at the next login.")

                            Label {
                                text: !root.config.openglPreloadKnown ? qsTr("Checking")
                                      : root.config.openglPreloadActive ? qsTr("Ready")
                                                                        : qsTr("Next login")
                                color: !root.config.openglPreloadKnown ? Theme.busy
                                       : root.config.openglPreloadActive ? Theme.online : Theme.busy
                                font.bold: true
                            }
                        }

                        // Both halves of the shared-state contract. A reader
                        // refuses a segment from another ABI, which from outside
                        // looks like "no daemon"; this row tells them apart.
                        SettingRow {
                            label: qsTr("Shared state")
                            description: root.config.segmentAbiVersion === 0
                                         ? qsTr("No segment: the daemon is not running.")
                                         : (root.config.segmentAbiVersion === root.config.abiVersion
                                            ? qsTr("ABI v%1, daemon and window agree.")
                                                  .arg(root.config.abiVersion)
                                            : qsTr("The daemon publishes v%1, this window expects "
                                                   + "v%2.").arg(root.config.segmentAbiVersion)
                                                            .arg(root.config.abiVersion))

                            Label {
                                readonly property bool agree:
                                    root.config.segmentAbiVersion === root.config.abiVersion
                                text: root.config.segmentAbiVersion === 0
                                      ? qsTr("none")
                                      : (agree ? qsTr("v%1").arg(root.config.abiVersion)
                                               : qsTr("v%1 ≠ v%2")
                                                     .arg(root.config.segmentAbiVersion)
                                                     .arg(root.config.abiVersion))
                                color: root.config.segmentAbiVersion === 0
                                       ? Theme.busy : (agree ? Theme.online : Theme.offline)
                                font.bold: true
                            }
                        }
                    }

                    Card {
                        // The card's own heading, so there is one way of heading
                        // a group.
                        title: qsTr("Drawing Now")
                        visible: root.config.liveInstances.length > 0
                        Layout.fillWidth: true

                        Repeater {
                            model: root.config.liveInstances

                            SettingRow {
                                objectName: "debugLiveRow"

                                required property var modelData
                                required property int index

                                label: modelData.name
                                description: {
                                    const parts = [];
                                    if (modelData.api === "vulkan") parts.push(qsTr("Vulkan"));
                                    else if (modelData.api === "opengl") parts.push(qsTr("OpenGL"));
                                    parts.push(qsTr("pid %1").arg(modelData.pid));
                                    if (modelData.frames !== undefined) {
                                        // Frames seen against frames painted:
                                        // "12000 / 0" is an overlay attached and
                                        // idle, not an absent one.
                                        parts.push(qsTr("%1 frames seen, %2 drawn")
                                                       .arg(modelData.frames)
                                                       .arg(modelData.drawn));
                                    }
                                    return parts.join(" · ");
                                }
                                first: index === 0
                            }
                        }
                    }

                    PlaceholderMessage {
                        visible: root.config.liveInstances.length === 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        iconName: "applications-games"
                        title: qsTr("The overlay is not drawing anywhere")
                        explanation: qsTr("A game appears here at the first frame the overlay "
                                          + "paints in it, with the frames it has seen and "
                                          + "the frames it has drawn.")
                    }

                    Item { Layout.fillHeight: root.config.liveInstances.length > 0 }
                }
            }

            // ------------------------------------------------------- sessions
            //
            // One list, two kinds, split by a section header: journals of
            // processes that ended without unwinding (a crash or a forced stop)
            // and sessions that ended cleanly. The journal text sits behind a
            // Loader that only an opened row instantiates.
            ColumnLayout {
                spacing: Theme.mediumSpacing

                ListView {
                    id: sessions

                    objectName: "debugSessions"
                    visible: count > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: Theme.smallSpacing
                    reuseItems: true

                    // Up to twenty sessions are kept, so the list scrolls. The
                    // bar sits beside the rows, not over them: its room comes
                    // out of the delegate's width (as ScrollablePage does with
                    // the page margin).
                    readonly property real barRoom:
                        sessionsBar.visible ? sessionsBar.width + Theme.smallSpacing : 0

                    ScrollBar.vertical: ScrollBar {
                        id: sessionsBar
                        policy: ScrollBar.AsNeeded
                    }

                    model: {
                        const rows = [];
                        for (const report of root.config.crashReports) {
                            rows.push({ kind: 0, entry: report });
                        }
                        for (const done of root.config.journalHistory) {
                            rows.push({ kind: 1, entry: done });
                        }
                        return rows;
                    }

                    section.property: "kind"
                    section.delegate: Label {
                        required property string section
                        text: section === "0" ? qsTr("Ended Badly") : qsTr("Finished Cleanly")
                        font.bold: true
                        topPadding: Theme.mediumSpacing
                        bottomPadding: Theme.smallSpacing
                        leftPadding: Theme.cardPadding
                    }

                    delegate: Rectangle {
                        id: row

                        // A card in all but component (its border says whether
                        // the session ended badly). Named as one so the padding
                        // check holds it to the same inset.
                        objectName: "card"

                        required property var modelData
                        required property int index

                        readonly property bool crashed: modelData.kind === 0
                        readonly property var entry: modelData.entry
                        // Reset on reuse, as ListView asks of recycled state.
                        property bool open: false
                        ListView.onReused: open = false

                        width: sessions.width - sessions.barRoom
                        implicitHeight: rowLayout.implicitHeight + Theme.cardPadding * 2
                        radius: Theme.cornerRadius
                        color: Theme.cardColour
                        border.color: row.crashed
                                      ? Qt.rgba(Theme.offline.r, Theme.offline.g,
                                                Theme.offline.b, 0.45)
                                      : Theme.cardBorder
                        border.width: 1

                        ColumnLayout {
                            id: rowLayout

                            objectName: "cardContent"

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.cardPadding
                            spacing: Theme.smallSpacing

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.mediumSpacing

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Label {
                                        objectName: row.crashed ? "debugCrash" : "debugSession"
                                        text: row.entry.process
                                        textFormat: Text.PlainText
                                        font.bold: true
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: {
                                            const parts = [];
                                            if (row.entry.api === "vulkan")
                                                parts.push(qsTr("Vulkan"));
                                            else if (row.entry.api === "opengl")
                                                parts.push(qsTr("OpenGL"));
                                            else if (row.entry.api === "daemon")
                                                parts.push(qsTr("daemon"));
                                            parts.push(qsTr("pid %1").arg(row.entry.pid));
                                            parts.push(row.entry.when);
                                            return parts.join(" · ");
                                        }
                                        opacity: 0.7
                                        font.pointSize:
                                            Math.max(7, Qt.application.font.pointSize - 1)
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }

                                Button {
                                    text: row.open ? qsTr("Hide") : qsTr("Journal")
                                    onClicked: row.open = !row.open
                                }
                            }

                            // The "Ended Badly" header explains the crashed rows
                            // once. The journal text exists only while a row is
                            // open: it is what makes a row expensive.
                            Loader {
                                active: row.open
                                visible: row.open
                                Layout.fillWidth: true
                                Layout.preferredHeight: active ? 160 : 0

                                sourceComponent: ScrollView {
                                    TextArea {
                                        readOnly: true
                                        selectByMouse: true
                                        text: root.config.journalText(row.entry.path)
                                        font.family: "monospace"
                                        wrapMode: TextEdit.NoWrap
                                    }
                                }
                            }

                            RowLayout {
                                visible: row.open
                                Layout.fillWidth: true

                                Item { Layout.fillWidth: true }

                                Button {
                                    text: qsTr("Copy")
                                    onClicked: root.config.crashCopy(row.entry.path)
                                }
                                Button {
                                    text: qsTr("Dismiss")
                                    visible: row.crashed
                                    onClicked: root.config.crashDismiss(row.entry.path)
                                }
                            }
                        }
                    }
                }

                PlaceholderMessage {
                    visible: sessions.count === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    iconName: "document-open-recent"
                    title: qsTr("Nothing has ended yet")
                    explanation: qsTr("Every process the overlay draws in keeps a journal. "
                                      + "One that exits cleanly leaves it here as history; "
                                      + "one that dies leaves it as a report.")
                }
            }

            // ----------------------------------------------------- daemon log
            ColumnLayout {
                spacing: Theme.mediumSpacing

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextArea {
                        objectName: "debugDaemonLog"
                        readOnly: true
                        selectByMouse: true
                        text: root.config.daemonLog
                        font.family: "monospace"
                        wrapMode: TextEdit.NoWrap
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("The last lines vocemd wrote to the system journal.")
                        opacity: 0.7
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Button {
                        text: qsTr("Refresh")
                        icon.name: "view-refresh"
                        onClicked: root.config.refreshDaemonLog()
                    }
                }
            }
        }
    }
}
