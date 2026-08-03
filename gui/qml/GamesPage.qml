// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which applications the overlay appears in.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

// Nothing is scanned for and nothing is guessed. The overlay is already inside
// every Vulkan and OpenGL process on the machine, so each one writes down that it
// was there and this reads the result -- which is why the list is empty until
// something has run, and why it is exact once something has.
//
// Grouped, not flat: everything that has ever run through a session lands here,
// and one undifferentiated list buried the four games somebody actually cares
// about under every tool the desktop had started. The groups follow the record's
// own evidence -- the verdict and the `why` token each process wrote down -- so
// the page needs no policy of its own; each group says in a line why it exists,
// and the group where the detection had nothing to go on is the one that says
// what the box beside each row is for.

ScrollablePage {
    id: root

    settings: ["hiddenApps", "shownApps"]
    title: qsTr("Applications")
    // What the box does and when. It used to say "takes effect in a running
    // game within a couple of seconds", which is true of the write and not of
    // the click: these rows are edits like every other setting on a page with
    // an Apply button, and nothing reaches a game until Apply is pressed.
    subtitle: qsTr("Everything the overlay has been loaded into, grouped by what it does there. It appears in games and leaves everything else alone. The box beside each row decides either way, and reaches a running game a couple of seconds after you apply.")
    // Only while there is nothing to list. A placeholder message centres itself
    // in whatever room it is given and has none unless the column may fill the
    // page -- but a column that fills the page hands the slack to every item
    // that is itself a layout, which a Card and a group are, so with content on
    // the page it pushed the groups a fifth of a screen apart. Measured at
    // 1160x720 with eight applications in two groups: headings at 250 and 503
    // where they belong at 179 and 306.
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

    // One row per application: its face, its name, and underneath it where the
    // answer came from, the API that drew, and the executable behind the name --
    // the name is what the kernel reports and it is cut to fifteen characters, so
    // two Proton titles can arrive looking alike and only the path tells them
    // apart. The record's own token stays reachable on hover: a case being argued
    // about is quoted from the record, not from a translation.
    component AppRow: SettingRow {
        required property var modelData
        required property int index

        label: modelData.name
        // Whatever the desktop knows this application by. Not every application
        // has an entry to be known by -- a game under a path its store invented
        // has none -- so the row falls back to a generic face of its kind.
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

    // A group with a header that folds it: the name, how many rows are under it,
    // and one line saying why the group exists. A group with nothing in it is not
    // shown at all -- an empty heading is a question, not information. While a
    // filter is typed the folds are ignored, because a search that can hit rows
    // the page then keeps hidden is a search that lies.
    component AppSection: ColumnLayout {
        id: section

        property string name
        property string blurb
        property var entries: []
        // Closed by default, on the owner's ask: this page is a history of every
        // application the overlay has ever been loaded into, and four open groups
        // put the answer somebody came for -- what has the overlay right now --
        // below a screenful of everything else. The live card above opens the page
        // instead; a group is opened by the person who wants it.
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

            // The card's padding, so this heading starts where a card's own
            // title starts and where the rows under it start. It used to inset
            // its arrow by the small step from inside instead: two kinds of
            // group heading on one page, this one's content beginning at 224
            // where a card's title begins at 239. The hover frame is the
            // background and still spans the whole width.
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

    // An empty view says so with a placeholder message rather than with a card
    // holding one row of grey text: the HIG's pattern for a view that is empty,
    // and the shape the Debug section already uses. Informational, because
    // nothing having run yet is not a problem to solve.
    PlaceholderMessage {
        visible: root.apps.length === 0
        Layout.fillWidth: true
        Layout.fillHeight: true
        iconName: "applications-games"
        title: qsTr("Nothing has run yet")
        explanation: qsTr("Start any application that draws with Vulkan or OpenGL and "
                          + "it appears here within a few seconds.")
    }

    // What has the overlay *right now*, above everything else. The list below is a
    // history -- everything the overlay has ever been loaded into -- and it could
    // not answer the question somebody opens this page with. These come from the
    // journals the injected code opens at its first **drawn** frame
    // (vocem/journal.h), so a process that was loaded and declined to draw is
    // not here: what this card lists is the overlay actually on screen somewhere.
    //
    // The record behind each one is looked up by name, for its face and its API,
    // and a live process with no record yet still shows -- it is drawing, which is
    // the whole claim.
    Card {
        // The same heading the Debug section gives the same card, so two pages
        // showing the same thing name it the same way.
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

                // The same box the row below carries, so the answer to "take it
                // out of this one" is where the eye already is. Without a record
                // there is nothing to write a rule against yet, so the box waits.
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

    // Running at this moment with the overlay switched off -- the second thing
    // somebody opens this page to find out, right under the first.
    //
    // A GAME the detection recognised, and nothing else. The first cut also
    // admitted the shapes a missed game arrives in (`nothing`, `not-ours:`),
    // on the reasoning that those are the cases somebody hunts -- and on the
    // owner's own machine that filled the card with plasmashell, the desktop
    // portal and the polkit agent, which all say `nothing` because they have
    // no entry naming them. A card about the game you are playing that lists
    // three desktop services is a card nobody reads. What the detection could
    // not recognise still has its own group below, which says what the box
    // is for; this card answers "I am in a game and the overlay is off".
    readonly property var runningWithout:
        root.apps.filter(a => a.running && !a.drawn && a.game)

    Card {
        title: qsTr("Running Without the Overlay")
        // In the card's heading rather than among its rows: a paragraph
        // dropped in beside a SettingRow is the one thing in a card that does
        // not take the card's padding, and it sat flush against the edge --
        // 17 units of unaccounted height at the default size and 34 at the
        // minimum, where it wraps to a second line.
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

    // The filter, once the history is long enough for the groups alone not to be
    // enough. It matches the name and the executable, which are the two things a
    // rule can be written against.
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

    // This is where the desktop's own tools accumulate, and they are the reason
    // the flat list stopped being readable. Every group folds now, so it needs no
    // exception of its own.
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

    // In the bar beside Reset rather than in a card of its own: it acts on the
    // whole list at once, and a button that empties the page has no business
    // scrolling along inside it.
    barContent: Button {
        text: qsTr("Empty the List")
        enabled: root.apps.length > 0
        onClicked: root.config.forgetApplications()
        ToolTip.text: qsTr("The list is a history of what has run, kept in the cache. Emptying it does not change where the overlay appears.")
        ToolTip.visible: hovered
        ToolTip.delay: 600
    }
}
