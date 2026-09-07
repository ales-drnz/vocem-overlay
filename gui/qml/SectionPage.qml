// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The frame every section shares: a title, a line saying what the section is for,
// the content, and the bar along the bottom that puts the edits into effect.
// Having one of these rather than a heading pasted into each page is what makes
// the margins and the type sizes agree from page to page.
//
// The bar is a DialogButtonBox with the standard buttons, so its labels, its
// order and its translations are the desktop's own -- on an Italian session it
// reads "Azzera" and "Applica" without this file knowing either word. Reset puts
// the settings of this page back to their defaults; Apply writes the file, which
// is the only moment a running game sees anything change.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Item {
    id: root

    required property string title
    property string subtitle: ""
    // The bridge. Passed in rather than reached for: this file is a component,
    // and the window's ConfigBridge is an id in another one.
    required property var config
    // The room kept clear on either side of the content. A page whose content
    // scrolls sets this to zero and pads its own column instead, so that the
    // scrollbar has the margin to sit in rather than eating into it. Everything
    // else keeps the page margin and is therefore the same distance from both
    // edges as the heading above it.
    property int contentMargin: Theme.pageMargin

    // The settings this page owns, by the name of the property on the bridge. The
    // page's Reset button walks them, and each one's default is the property of
    // the same name with "default" in front -- which is the convention the reset
    // beside every individual row already follows.
    //
    // A page with none of them -- About -- gets no bar at all.
    property var settings: []

    default property alias content: holder.data

    // A page's own action in the bottom bar, beside Reset -- for something that
    // acts at once on the page's subject rather than editing a setting, like
    // emptying the applications list. In the bar rather than in the card column,
    // so it does not scroll with the content it acts on.
    property alias barContent: extras.data

    readonly property bool hasSettings: settings.length > 0

    onAtDefaultsChanged: reset.sync()

    readonly property bool atDefaults: {
        for (let i = 0; i < settings.length; ++i) {
            const key = settings[i];
            const current = root.config[key];
            const fallback =
                root.config["default" + key.charAt(0).toUpperCase() + key.slice(1)];
            // Numbers within a rounding of each other, everything else -- colours,
            // switches -- as the strings they print as. Comparing a colour
            // arithmetically produces NaN, and NaN fails every test it is put in,
            // so a changed colour would have counted as unchanged.
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
            // The page's content area, named so the dump says where each page
            // put it: "the paddings do not agree between pages" is a claim
            // about these four numbers, and it was being settled by eye.
            objectName: "pageContent"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: root.contentMargin
            Layout.rightMargin: root.contentMargin
            // One clearance under the content, whatever ends the page. With no
            // bar the page margin is the whole gap to the window's edge; with a
            // bar the column's own spacing supplies half of it, and the two
            // together come to the page margin again -- mediumSpacing is two
            // steps and pageMargin is four, at any font size. It was 0 here,
            // so the last card sat 12 above the bar on a page that does not
            // scroll and 36 above it on one that does (measured, both sizes).
            Layout.bottomMargin: bar.visible ? Theme.mediumSpacing : Theme.pageMargin
        }

        // Along the bottom edge, full width and against the frame, which is where
        // this bar goes in every Qt application: it belongs to the window rather
        // than to the column of settings, so it does not take the page margin.
        Rectangle {
            id: bar

            objectName: "pageBar"
            // A page with settings gets Reset and Apply; a page with an action
            // of its own gets the bar for that action alone. Debug is the second
            // kind: it has nothing to apply and one thing to do -- empty the
            // journals -- and an action that belongs to the whole page belongs
            // where every other page's actions are, not in the scrolling content
            // it acts on. Both halves are separate visibilities rather than one,
            // so neither kind of page shows a button that would do nothing.
            visible: root.hasSettings || extras.children.length > 0
            color: Theme.headerColour
            // Tight around the buttons: the bar is a frame for two controls, not a
            // band across the window, and it takes room from the page above it.
            implicitHeight: Math.max(apply.implicitHeight, extras.implicitHeight) +
                            Theme.smallSpacing * 2
            Layout.fillWidth: true

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.separator
            }

            // Two boxes rather than one, so that Reset sits at the left edge and
            // Apply at the right, which is where a settings window on this desktop
            // puts them. One DialogButtonBox lays its buttons out by the platform's
            // own rule and put both of them on the right. The standard buttons are
            // still what is used, so the words, the icons and their translations
            // are the desktop's -- this file does not know the Italian for either.
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

                    // The button is the style's, built when the box is, so its
                    // enabled state is set on it rather than bound to it: a
                    // Binding whose target is a function call is evaluated once,
                    // before the button exists, and then never again -- which left
                    // both of these permanently enabled.
                    function sync() {
                        const button = standardButton(DialogButtonBox.Reset);
                        if (button) {
                            button.enabled = !root.atDefaults;
                        }
                    }

                    Component.onCompleted: sync()

                    // This page's settings back to their defaults, which is what
                    // the standard button means and what the reset beside each row
                    // does one at a time. It is an edit like any other: it takes
                    // Apply to reach the game.
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

                // Why the last Apply did not land, between the two buttons:
                // the file could not be written, and a grey Apply button
                // used to be the whole of what the window said about it.
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
