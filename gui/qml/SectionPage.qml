// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The frame every section shares: a title, a line saying what the section is for,
// the content, and the bar along the bottom that puts the edits into effect, so
// margins and type sizes agree from page to page.
//
// The bar's buttons are DialogButtonBox standard buttons, so their labels and
// translations are the desktop's own. Reset puts this page's settings back to
// their defaults; Apply writes the file, the only moment a running game sees a
// change.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property string title
    property string subtitle: ""
    // The bridge, passed in: the window's ConfigBridge is an id in another file.
    required property var config
    // The room kept clear on either side of the content. A scrolling page sets
    // it to zero and pads its own column, so the scrollbar sits in the margin.
    property int contentMargin: Theme.pageMargin

    // The settings this page owns, by bridge property name. Reset walks them;
    // each one's default is the property of the same name with "default" in
    // front, as for the reset beside each row. A page with none gets no bar.
    property var settings: []

    default property alias content: holder.data

    // A page's own action in the bottom bar, beside Reset, for something that
    // acts at once rather than editing a setting (emptying the applications
    // list). In the bar, so it does not scroll with the content it acts on.
    property alias barContent: extras.data

    readonly property bool hasSettings: settings.length > 0

    onAtDefaultsChanged: reset.sync()

    readonly property bool atDefaults: {
        for (let i = 0; i < settings.length; ++i) {
            const key = settings[i];
            const current = root.config[key];
            const fallback =
                root.config["default" + key.charAt(0).toUpperCase() + key.slice(1)];
            // Numbers within a rounding of each other, everything else as the
            // strings they print as: a colour compared arithmetically is NaN,
            // which would count a changed colour as unchanged.
            if (typeof current === "number") {
                if (Math.abs(current - fallback) > 0.0001) {
                    return false;
                }
            } else if (String(current) !== String(fallback)) {
                return false;
            }
        }
        return true;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.pageMargin
        spacing: Theme.mediumSpacing

        Label {
            text: root.title
            font.pointSize: Qt.application.font.pointSize + 3
            font.bold: true
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pageMargin
            Layout.rightMargin: Theme.pageMargin
        }

        Label {
            text: root.subtitle
            visible: root.subtitle !== ""
            opacity: 0.7
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.topMargin: -Theme.smallSpacing
            Layout.leftMargin: Theme.pageMargin
            Layout.rightMargin: Theme.pageMargin
        }

        Item {
            id: holder
            // The page's content area, named so the geometry dump says where
            // each page put it and paddings can be compared as numbers.
            objectName: "pageContent"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: root.contentMargin
            Layout.rightMargin: root.contentMargin
            // One clearance under the content, whatever ends the page: the page
            // margin. With a bar the column's spacing supplies half of it
            // (mediumSpacing is two steps, pageMargin four, at any font size).
            Layout.bottomMargin: bar.visible ? Theme.mediumSpacing : Theme.pageMargin
        }

        // Along the bottom edge, full width, as in every Qt application: it
        // belongs to the window, so it does not take the page margin.
        Rectangle {
            id: bar

            objectName: "pageBar"
            // A page with settings gets Reset and Apply; a page with an action
            // of its own (Debug's emptying of the journals) gets the bar for that
            // alone. Separate visibilities, so no page shows a button that would
            // do nothing.
            visible: root.hasSettings || extras.children.length > 0
            color: Theme.headerColour
            // Tight around the buttons: it takes room from the page above it.
            implicitHeight: Math.max(apply.implicitHeight, extras.implicitHeight) +
                            Theme.smallSpacing * 2
            Layout.fillWidth: true

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.separator
            }

            // Two boxes, so Reset sits at the left edge and Apply at the right
            // as on this desktop; one DialogButtonBox puts both on the right.
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.pageMargin - reset.padding
                anchors.rightMargin: Theme.pageMargin - apply.padding
                anchors.topMargin: Theme.smallSpacing
                anchors.bottomMargin: Theme.smallSpacing
                spacing: Theme.mediumSpacing

                DialogButtonBox {
                    id: reset

                    background: null
                    visible: root.hasSettings
                    alignment: Qt.AlignLeft
                    standardButtons: DialogButtonBox.Reset

                    // The button is built with the box, so its enabled state is
                    // set rather than bound: a Binding whose target is a function
                    // call is evaluated once, before the button exists.
                    function sync() {
                        const button = standardButton(DialogButtonBox.Reset);
                        if (button) {
                            button.enabled = !root.atDefaults;
                        }
                    }

                    Component.onCompleted: sync()

                    // This page's settings back to their defaults: an edit like
                    // any other, which takes Apply to reach the game.
                    onReset: {
                        for (let i = 0; i < root.settings.length; ++i) {
                            const key = root.settings[i];
                            root.config[key] = root.config[
                                "default" + key.charAt(0).toUpperCase() + key.slice(1)];
                        }
                    }
                }

                RowLayout {
                    id: extras
                    spacing: Theme.smallSpacing
                }

                // Why the last Apply did not land (the file could not be
                // written), between the two buttons.
                InlineMessage {
                    objectName: "saveError"
                    visible: root.hasSettings && root.config.saveError !== ""
                    severity: InlineMessage.Severity.Error
                    text: root.config.saveError
                    Layout.fillWidth: true
                }

                Item { Layout.fillWidth: root.config.saveError === "" }

                DialogButtonBox {
                    id: apply

                    background: null
                    visible: root.hasSettings
                    alignment: Qt.AlignRight
                    standardButtons: DialogButtonBox.Apply

                    function sync() {
                        const button = standardButton(DialogButtonBox.Apply);
                        if (button) {
                            button.enabled = root.config.pending;
                        }
                    }

                    Component.onCompleted: sync()

                    onApplied: root.config.apply()
                }
            }

            Connections {
                target: root.config
                function onPendingChanged() { apply.sync(); }
            }
        }
    }
}
