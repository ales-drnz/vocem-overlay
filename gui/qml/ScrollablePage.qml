// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A section whose content is a column of cards that can be taller than the window.
//
// The page margin belongs to the column inside the flickable, not to the
// flickable, so the scrollbar sits in the margin and never over the content, as
// in Kirigami's ScrollablePage; the cards stay centred whether or not it scrolls.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    // The column below pads itself, so the page must not pad it as well.
    contentMargin: 0

    default property alias cards: column.data

    // Room beside the cards, outside the flickable: the live preview put here
    // stays put while the controls scroll. Empty, it takes no room.
    property alias side: sideHolder.data
    readonly property bool hasSide: sideHolder.children.length > 0

    // Whether the column stretches to the bottom of the window when the cards
    // are shorter. Only for a page whose whole content is one placeholder
    // message: Layout.fillHeight defaults to true for an item that is itself a
    // layout, as every Card is, so a stretched column hands its slack to all of
    // them (entry 65). tests/window_padding.cmake holds a page with a card to its
    // implicit height.
    property bool stretchToBottom: false

    Item {
        id: sideHolder

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.rightMargin: root.hasSide ? Theme.pageMargin : 0
        width: root.hasSide ? sideHolder.children[0].implicitWidth : 0
        visible: root.hasSide
    }

    ScrollView {
        id: view

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: sideHolder.left
        clip: true

        // The desktop style's scrollbar is not an overlay: ScrollView keeps room
        // for it beside the content, out of the page margin. Given back here, so
        // both sides are the page margin and the bar sits inside it.
        readonly property real barRoom: width - availableWidth

        // Written out rather than left to ScrollView so the column's height
        // can be set from here (stretchToBottom). No bottom margin of its own:
        // SectionPage keeps the clearance for every page, in one place.
        Flickable {
            id: flick

            contentWidth: width
            contentHeight: column.height
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: column

                // Named so the dump carries its height and implicit height: a
                // page taller than its contents hands out dead space, which a
                // screenshot does not make obvious.
                objectName: "pageColumn"

                x: Theme.pageMargin
                width: parent.width - Theme.pageMargin -
                       Math.max(0, Theme.pageMargin - view.barRoom)
                // Its own height, so a page that stretches can hand the
                // leftover viewport to its content; the clearance below is the
                // frame's.
                height: root.stretchToBottom
                        ? Math.max(implicitHeight, flick.height)
                        : implicitHeight
                spacing: Theme.largeSpacing
            }
        }
    }
}
