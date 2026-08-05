// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the overlay is doing, what it left behind, and what the daemon said.
//
// The overlay's failure mode is silence: a layer that did not load, a segment
// from another ABI, a process that crashed, all look identical from the couch
// -- nothing on screen. This section is the running answer to "is it working,
// and if not, where does it stop", and it is the crash pop-up's successor
// (DESIGN entry 60 says why a section beats a window).
//
// Structured on the HIG rather than by instinct, which is what the first cut
// was: five cards stacked in one scrolling column, a grey line of text where
// an empty view should have been, and a log box with half a page of nothing
// under it.
//
//   * **Three tabs**, because these are "related views sharing the same level
//     of hierarchy", which is exactly what the guidelines give tabs for -- and
//     because the alternative, one long column, spends the main content area
//     on the two thirds nobody is looking at. Now: what is happening, what
//     happened, and what the daemon said.
//   * **The tab that opens is the one with something to say.** A crash report
//     waiting is the reason somebody comes here, so the page opens on it; with
//     nothing wrong it opens on the state of things.
//   * **An inline message above the tabs, only when something is wrong.** The
//     HIG's severities are colour, icon and words together -- never colour
//     alone, which is this project's own rule about the speaking ring. A page
//     with no banner is a page where nothing is wrong; a green "all fine" box
//     would be noise in the place the eye checks first.
//   * **ListView, not a Repeater**, for the sessions: the guidelines put
//     mostly-textual content in a list, and Qt's own advice is to keep a
//     delegate cheap and put what only an open row needs behind a Loader. The
//     first cut built a TextArea per crash at launch, on every page of the
//     window, whether or not anybody opened this one.
//   * **Placeholder messages for empty views**: an icon and a sentence, the
//     informational kind, because an empty view here means nothing is wrong.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    settings: []
    title: qsTr("Debug")
    subtitle: qsTr("What the overlay is doing right now, and what it left behind.")

    // The page's own action, in the bar every other section carries. There is
    // nothing to apply here and nothing to put back to a default, so the bar
    // holds this alone: it empties the Sessions list -- both kinds -- and it
    // says so rather than being called Reset, which on every other page means
    // "put the settings back". Enabled only when there is something to clear,
    // because a button that does nothing is a button that lies about the state
    // of the page. The daemon's log is not in it: those lines are journald's.
    barContent: Button {
        objectName: "debugClear"
        text: qsTr("Clear Journals")
        icon.name: "edit-clear-history"
        enabled: root.config.crashReports.length > 0 ||
                 root.config.journalHistory.length > 0
        onClicked: root.config.clearJournals()
    }

    // The daemon's journald lines are fetched when somebody actually looks,
    // not on a timer: this page is the only reader.
    onVisibleChanged: {
        if (visible) {
            root.config.refreshDaemonLog();
            tabs.currentIndex = root.openingTab();
        }
    }

    // Whichever tab has something to say. A crash waiting is why somebody
    // opens this section at all; anything else opens on the state of things.
    function openingTab() {
        return root.config.crashReports.length > 0 ? 1 : 0;
    }

    readonly property bool healthy:
        config.vulkanLayerInstalled && config.openglPreloadActive &&
        config.segmentAbiVersion === config.abiVersion

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.mediumSpacing

        // ---- the one banner, and only when it is earned
        InlineMessage {
            objectName: "debugBanner"
            visible: !root.healthy
            Layout.fillWidth: true
            severity: root.config.segmentAbiVersion !== 0 &&
                      root.config.segmentAbiVersion !== root.config.abiVersion
                      ? InlineMessage.Severity.Error
                      : InlineMessage.Severity.Warning
            text: {
                if (root.config.segmentAbiVersion !== 0 &&
                    root.config.segmentAbiVersion !== root.config.abiVersion) {
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
            // Inside a scrolling view, because this tab grows: the health card
            // is fixed, but "Drawing Now" lists one row per process the overlay
            // is painting in, and a session with several games open ran off the
            // bottom of the window with nothing to scroll and no bar to say so.
            ScrollView {
                id: nowView

                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: nowView.availableWidth
                    // At least the viewport, so the placeholder message can
                    // still centre itself on an empty tab (it asks for the
                    // slack with Layout.fillHeight, and inside a scrolling
                    // column there is none unless it is granted here).
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
                            description: root.config.openglPreloadActive
                                         ? qsTr("Active in this session.")
                                         : qsTr("Not in this session's environment. It reaches "
                                                + "new sessions at the next login.")

                            Label {
                                text: root.config.openglPreloadActive ? qsTr("Ready")
                                                                      : qsTr("Next login")
                                color: root.config.openglPreloadActive ? Theme.online : Theme.busy
                                font.bold: true
                            }
                        }

                        // Both halves of the shared-state contract. A reader that
                        // meets a segment from another ABI refuses it -- correctly
                        // -- and from outside that refusal looks exactly like "no
                        // daemon", so this row is where the difference becomes a
                        // sentence.
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
                        // The card's own heading rather than a Label beside it
                        // wearing the same left margin by hand: one way of heading
                        // a group, so the two cannot drift apart.
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
                                        // Frames the overlay was willing to draw in
                                        // against the frames it painted: "12000 / 0"
                                        // is an overlay attached and idle, which is a
                                        // different story from one that is absent.
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
            // One list, two kinds, told apart by a section header: the
            // journals of processes that ended without unwinding -- a crash or
            // a forced stop -- and the sessions that ended cleanly. A list
            // because this is textual content that can be long, and a
            // ListView because it recycles its rows and can leave the
            // expensive half (the journal's text) behind a Loader that only
            // an opened row instantiates.
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

                    // A list that can be longer than the window, and was the one
                    // long list in this window with no bar beside it: twenty
                    // sessions are kept, and past the third or fourth there was
                    // nothing on screen to say the rest were there.
                    //
                    // Beside the rows, never over them. A ListView's attached
                    // scrollbar is an overlay by default and sat on top of the
                    // cards, half over the Journal buttons; every other scrolling
                    // view in this window keeps room for its bar (ScrollablePage
                    // says the same thing about the page margin). The room comes
                    // out of the delegate's width, which is the only place a
                    // ListView has to give it from.
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

                        // A card by every measure except the component: the
                        // border says whether the session ended badly, which
                        // Card has no opinion to offer about. Named as one
                        // anyway, so the padding check holds these rows to the
                        // same inset as every other card in the window.
                        objectName: "card"

                        required property var modelData
                        required property int index

                        readonly property bool crashed: modelData.kind === 0
                        readonly property var entry: modelData.entry
                        // Reset on reuse, which is what ListView asks of any
                        // state a recycled delegate keeps.
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

                            // No sentence per row here. Every crashed row used
                            // to carry the same three lines about what ending
                            // without shutting down means, which on a list of
                            // eight is the same paragraph eight times: the
                            // "Ended Badly" header above them says it once, for
                            // all of them, which is what a section header is
                            // for.

                            // The journal's text is what makes a row expensive,
                            // so it exists only while a row is open: Qt's own
                            // advice for delegates, and the reason the first cut
                            // cost 780 MB with one oversized journal.
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
