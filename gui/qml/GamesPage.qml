// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The Applications section: which applications the overlay appears in.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

// Nothing is scanned for: the overlay is inside every Vulkan and OpenGL
// process, each one records that it was there, and this page reads the
// records -- empty until something has run, exact once something has.
//
// Grouped by the record's own verdict and `why` token, so the page has no
// policy of its own; each group says in a line why it exists.

ScrollablePage {
    id: root

    settings: ["hiddenApps", "shownApps"]
    title: qsTr("Applications")
    // The rows are edits like every other setting: nothing reaches a game
    // until Apply is pressed.
    subtitle: qsTr("Everything the overlay has been loaded into, grouped by what it does there. It appears in games and leaves everything else alone. The box beside each row decides either way, and reaches a running game a couple of seconds after you apply.")
    // Only while there is nothing to list: the placeholder needs the column
    // to fill the page to centre itself, but a filling column hands the slack
    // to every Card and group and pushes them apart.
    stretchToBottom: root.apps.length === 0 ||
                     (root.filter !== "" && root.matched === 0)

    readonly property var apps: root.config.applications
    property string filter: ""

    function matches(app) {
        if (root.filter === "") {
            return true;
        }
        const needle = root.filter.toLowerCase();
        return app.name.toLowerCase().includes(needle) ||
               app.executable.toLowerCase().includes(needle);
    }
    function group(category) {
        return root.apps.filter(a => a.category === category && root.matches(a));
    }
    readonly property int matched: apps.filter(a => root.matches(a)).length

    // One row per application: its face, its name, and underneath where the
    // answer came from, the API, and the executable -- the kernel cuts a name
    // to fifteen characters, so only the path tells two Proton titles apart.
    // The record's own `why` token is on hover, quoted as recorded.
    component AppRow: SettingRow {
        required property var modelData
        required property int index

        label: modelData.name
        // The desktop's icon for it, or a generic face of its kind when there
        // is no desktop entry (a game under a path its store invented).
        icon: modelData.icon !== ""
              ? modelData.icon
              : "image://icon/" + root.config.icon(
                    modelData.category === "game"
                        ? ["applications-games", "input-gaming"]
                        : ["application-x-executable", "system-run"])
        description: {
            const parts = [];
            if (modelData.reason !== "") {
                parts.push(modelData.reason);
            }
            parts.push(modelData.api === "vulkan" ? qsTr("Vulkan") : qsTr("OpenGL"));
            parts.push(modelData.executable);
            return parts.join(" · ");
        }
        tooltip: (modelData.evidence !== "" ? "why = " + modelData.evidence + "\n" : "") +
                 qsTr("Last seen: %1").arg(
                     modelData.seen.toLocaleString(Qt.locale(), Locale.ShortFormat))
        first: index === 0

        CheckBox {
            checked: modelData.drawn
            onToggled: root.config.setApplicationDrawn(modelData.name, checked,
                                                       modelData.game)
            Accessible.name: qsTr("Show the overlay in %1").arg(modelData.name)
        }
    }

    // A folding group: its name, how many rows, and one line on why it exists.
    // Empty groups are not shown. While a filter is typed the folds are
    // ignored, so a search never hits rows the page keeps hidden.
    component AppSection: ColumnLayout {
        id: section

        property string name
        property string blurb
        property var entries: []
        // Closed by default: this is a history of everything the overlay has
        // been loaded into, and the live card above answers the usual question.
        property bool expanded: false
        readonly property bool open: expanded || root.filter !== ""

        visible: entries.length > 0
        spacing: Theme.smallSpacing
        Layout.fillWidth: true

        AbstractButton {
            id: header

            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + Theme.smallSpacing * 2
            activeFocusOnTab: true
            hoverEnabled: true

            // The card's padding, so this heading starts where a card's title
            // and its rows start. The hover frame is the background and spans
            // the whole width.
            leftPadding: Theme.cardPadding
            rightPadding: Theme.cardPadding

            Accessible.role: Accessible.Button
            Accessible.name: (section.open ? qsTr("Collapse %1") : qsTr("Expand %1"))
                                 .arg(section.name)

            onClicked: section.expanded = !section.expanded

            background: Rectangle {
                radius: Theme.cornerRadius
                color: header.hovered ? Theme.cardColour : "transparent"
                border.width: 2
                border.color: header.activeFocus ? Theme.palette.highlight : "transparent"
                antialiasing: true
            }

            contentItem: RowLayout {
                id: headerRow

                objectName: "groupHeading"
                spacing: Theme.smallSpacing

                Image {
                    source: "image://icon/" + root.config.icon(["go-next", "arrow-right"])
                    sourceSize.width: 16
                    sourceSize.height: 16
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    rotation: section.open ? 90 : 0
                    Behavior on rotation { NumberAnimation { duration: 120 } }
                }

                ColumnLayout {
                    spacing: 1
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: Theme.smallSpacing
                        Label {
                            text: section.name
                            font.bold: true
                        }
                        Label {
                            text: section.entries.length
                            opacity: 0.65
                        }
                    }
                    Label {
                        text: section.blurb
                        opacity: 0.65
                        font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }

        Card {
            visible: section.open
            Layout.fillWidth: true

            Repeater {
                model: section.entries
                AppRow {}
            }
        }
    }

    // An empty view: the HIG's placeholder message, informational because
    // nothing having run yet is not a problem.
    PlaceholderMessage {
        visible: root.apps.length === 0
        Layout.fillWidth: true
        Layout.fillHeight: true
        iconName: "applications-games"
        title: qsTr("Nothing has run yet")
        explanation: qsTr("Start any application that draws with Vulkan or OpenGL and "
                          + "it appears here within a few seconds.")
    }

    // What has the overlay right now, above the history. These come from the
    // journals the injected code opens at its first drawn frame
    // (vocem/journal.h), so a process that declined to draw is not here. The
    // record is looked up by name for the face and API; a live process with no
    // record yet still shows.
    Card {
        // The same heading the Debug section gives the same card.
        title: qsTr("Drawing Now")
        Layout.fillWidth: true
        visible: root.config.liveInstances.length > 0

        Repeater {
            model: root.config.liveInstances

            SettingRow {
                required property var modelData
                required property int index

                readonly property var record: {
                    for (const app of root.apps) {
                        if (app.name === modelData.name) {
                            return app;
                        }
                    }
                    return null;
                }

                label: modelData.name
                icon: record && record.icon !== ""
                      ? record.icon
                      : "image://icon/" + root.config.icon(["applications-games", "input-gaming"])
                description: {
                    const parts = [qsTr("drawing now")];
                    if (record) {
                        parts.push(record.api === "vulkan" ? qsTr("Vulkan") : qsTr("OpenGL"));
                    }
                    parts.push(qsTr("pid %1").arg(modelData.pid));
                    return parts.join(" · ");
                }
                first: index === 0

                // The same box the history row carries. Without a record there
                // is nothing to write a rule against, so it waits.
                CheckBox {
                    visible: record !== null
                    checked: record ? record.drawn : false
                    onToggled: root.config.setApplicationDrawn(modelData.name, checked,
                                                               record ? record.game : true)
                    Accessible.name: qsTr("Show the overlay in %1").arg(modelData.name)
                }
            }
        }
    }

    // Running now with the overlay off: games the detection recognised, and
    // nothing else -- the unrecognised shapes are mostly desktop services with
    // no entry naming them, and have their own group below.
    readonly property var runningWithout:
        root.apps.filter(a => a.running && !a.drawn && a.game)

    Card {
        title: qsTr("Running Without the Overlay")
        // In the card's heading, not among its rows: a paragraph beside a
        // SettingRow does not take the card's padding.
        description: qsTr("Recognised as games, with a live process right now, and "
                          + "the overlay is not drawn in them. The box beside each "
                          + "one changes that when you apply.")
        visible: root.runningWithout.length > 0
        Layout.fillWidth: true

        Repeater {
            model: root.runningWithout

            SettingRow {
                objectName: "runningWithoutRow"

                required property var modelData
                required property int index

                label: modelData.name
                icon: modelData.icon !== ""
                      ? modelData.icon
                      : "image://icon/" + root.config.icon(["applications-games",
                                                            "input-gaming"])
                description: modelData.reason
                first: index === 0

                CheckBox {
                    checked: false
                    onToggled: root.config.setApplicationDrawn(modelData.name, checked,
                                                               modelData.game)
                    Accessible.name: qsTr("Show the overlay in %1").arg(modelData.name)
                }
            }
        }
    }

    // The filter, once the history is long: it matches the name and the
    // executable, the two things a rule can be written against.
    TextField {
        visible: root.apps.length > 8
        placeholderText: qsTr("Filter by name or path")
        Accessible.name: qsTr("Filter applications by name or path")
        onTextChanged: root.filter = text
        Layout.fillWidth: true
    }

    AppSection {
        name: qsTr("Games")
        blurb: qsTr("Recognised as games. The overlay appears in these while you are in a voice channel.")
        entries: root.group("game")
    }

    AppSection {
        name: qsTr("Launchers and Game Tools")
        blurb: qsTr("The overlay stays out of the launcher itself and appears in the games it starts.")
        entries: root.group("launcher")
    }

    // Where the desktop's own tools accumulate.
    AppSection {
        name: qsTr("Other Applications")
        blurb: qsTr("Ordinary desktop applications. The overlay is not drawn in them.")
        entries: root.group("other")
    }

    AppSection {
        name: qsTr("Not Recognised")
        blurb: qsTr("These said nothing about what they are. If one of them is a game, ticking its box turns the overlay on there.")
        entries: root.group("unrecognised")
    }

    PlaceholderMessage {
        visible: root.filter !== "" && root.matched === 0 && root.apps.length > 0
        Layout.fillWidth: true
        Layout.fillHeight: true
        iconName: "system-search"
        title: qsTr("Nothing matches \"%1\"").arg(root.filter)
        explanation: qsTr("The filter looks at the name and at the path, which are "
                          + "the two things a rule can be written against.")
    }

    // In the bar beside Reset: it acts on the whole list, so it does not
    // scroll with it.
    barContent: Button {
        text: qsTr("Empty the List")
        enabled: root.apps.length > 0
        onClicked: root.config.forgetApplications()
        ToolTip.text: qsTr("The list is a history of what has run, kept in the cache. Emptying it does not change where the overlay appears.")
        ToolTip.visible: hovered
        ToolTip.delay: 600
    }
}
