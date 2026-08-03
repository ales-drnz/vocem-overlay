// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A section whose content is a column of cards that can be taller than the window.
//
// The page margin belongs to the column inside the flickable rather than to the
// flickable itself, which is what gives the scrollbar somewhere to sit. The pages
// used to keep the page margin on the ScrollView and then subtract a gutter from
// the content's width on the right only, so the cards sat 24 from the left edge of
// the window and 42 from the right -- a page that was visibly off-centre against
// the two that carry a picture, and off-centre by a different amount depending on
// whether it scrolled.
//
// Kirigami's ScrollablePage is arranged the same way, and so is every other Qt
// application on the desktop: the bar overlays the margin, never the content.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    // The column below pads itself, so the page must not pad it as well.
    contentMargin: 0

    default property alias cards: column.data

    // Room beside the cards, outside the flickable: what a page puts here -- the
    // live preview -- stays put while the controls scroll, because it answers for
    // whichever control was just edited, wherever that control has scrolled to.
    // Empty on the pages that have nothing to show there, and then it takes no
    // room at all.
    property alias side: sideHolder.data
    readonly property bool hasSide: sideHolder.children.length > 0

    // Whether the column stretches to the bottom of the window when the cards
    // are shorter than it. Off for a page of settings, and on only for a page
    // whose whole content is one thing that should be centred or filled -- an
    // empty view's placeholder message.
    //
    // Not "a card only grows if it asks", which is what this said and what it
    // is not: Layout.fillHeight defaults to TRUE for an item that is itself a
    // layout, and a Card and the Applications page's groups are ColumnLayouts.
    // So a stretched column hands its slack to all of them at once. Switched on
    // for a page that also has cards, it spread the Applications page's two
    // group headings 253 units apart where they belong 127 apart. Anything that
    // turns this on with content on the page has to answer
    // tests/window_padding.cmake, which holds a page carrying a card to its own
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
        // for it beside the content rather than over it, and that room comes out
        // of the page margin -- which put the cards 24 from the left edge and 45
        // from the right. Given back here, so both sides are the page margin and
        // the bar, narrower than that margin, still has room of its own.
        readonly property real barRoom: width - availableWidth

        // The Flickable is written out rather than left to ScrollView so that
        // the column's height can be set from here (stretchToBottom below).
        //
        // It keeps no bottom margin of its own. It used to, and that was the
        // page margin counted twice: SectionPage already holds the content
        // area clear of the bar and of the window's edge, so a page scrolled
        // to the end left 36 units under its last card where a page that does
        // not scroll left 12, and About left 48. The clearance belongs to the
        // frame, in one place, for every page.
        Flickable {
            id: flick

            contentWidth: width
            contentHeight: column.height
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: column

                // Named so the dump carries both its height and its implicit
                // one: a page that is taller than its contents is a page
                // handing dead space to whatever is on it, and that is not
                // something a screenshot makes obvious.
                objectName: "pageColumn"

                x: Theme.pageMargin
                width: parent.width - Theme.pageMargin -
                       Math.max(0, Theme.pageMargin - view.barRoom)
                // Its own height rather than the layout's implicit one, so a
                // page that stretches can hand the leftover room to whichever
                // card asked for it. The room is the whole viewport: the
                // clearance below it is the frame's, outside this.
                height: root.stretchToBottom
                        ? Math.max(implicitHeight, flick.height)
                        : implicitHeight
                spacing: Theme.largeSpacing
            }
        }
    }
}
