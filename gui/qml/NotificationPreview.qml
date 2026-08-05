// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stand-in for the notification toast, matching what the overlay draws: same
// font, same measurements, same rounded avatar.
//
// As in PanelPreview, every figure here is the one common/src/panel.cpp uses, and
// the two are compared as numbers by scripts/compare-preview.py rather than judged
// from a screenshot. The message carries a size of its own on top of the shared
// one, and that size applies to everything in the box -- text, picture, padding
// and corners -- which is why the view scales the whole item rather than this file
// scaling each figure in it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Rectangle {
    id: root

    required property var config
    // As in PanelPreview: one unit is one overlay pixel at 1080 lines of display
    // height, and the view scales the whole box to whatever it stands for -- for
    // this box, the user's scale and the message's own size together.
    // The height of a line of text, from the setting: the layout is expressed
    // in these units and the overlay divides by the same number.
    readonly property real fontPixels: config.fontSize
    readonly property real paddingX: config.boxPaddingX
    readonly property real paddingY: config.boxPaddingY
    readonly property real avatarGap: config.avatarGap
    // panel.cpp: avatar_radius = GetTextLineHeight() * 0.9, so a diameter of 1.8
    // times the font size.
    readonly property real avatarDiameter: fontPixels * 1.8
    readonly property real textPixels: fontPixels * config.overlayFontRatio

    // The overlay's box is 320 units wide, and never wider than the display it has
    // to sit on. The view supplies that limit in these units, since only it knows
    // how much screen it stands for.
    property real widestBox: 0
    readonly property real boxWidth: widestBox > 0 ? Math.min(320, widestBox) : 320

    // Every colour below comes from include/vocem/theme.h, through the bridge, so
    // this preview and the drawing cannot hold different opinions about them. The
    // theme has already chosen the palette for a pale box or a dark one, which is a
    // decision this file used to make a second time, in its own words.
    readonly property var tokens: config.overlayTheme

    Component.onCompleted: Theme.requireTokens(root.tokens, "NotificationPreview", [
        "toastSurface", "toastTitle", "toastBody", "avatarPlaceholder", "avatarMark", "boxRadius",
        "toastHairline", "toastAccent", "toastAccentWidth",
        "textOutlineInk", "toastTextOutline",
    ])

    // The same permanent outline the overlay draws, from the same switch: four
    // offset copies of the text in the outline's ink. See PanelPreview for the
    // terms; this box only differs in which strength token governs it.
    readonly property real outlineStrength: root.tokens.toastTextOutline
    readonly property var outlineOffsets: [[1, 0], [-1, 0], [0, 1], [0, -1]]

    color: Qt.rgba(root.tokens.toastSurface.r, root.tokens.toastSurface.g,
                   root.tokens.toastSurface.b, config.notificationOpacity)
    antialiasing: true
    radius: root.tokens.boxRadius
    implicitWidth: boxWidth
    implicitHeight: Math.max(avatar.height, text.implicitHeight) + paddingY * 2

    // The blurple's bar, down the left edge as panel.cpp draws it: three units
    // wide, stopped where the corner curvature starts, outer corners rounded by
    // its own width. The clip is what rounds only the right-hand corners: the
    // rectangle is twice the bar's width with all four rounded, and the left
    // half -- square edge included -- is cut away.
    Item {
        x: 0
        y: root.tokens.boxRadius
        width: root.tokens.toastAccentWidth
        height: parent.height - root.tokens.boxRadius * 2
        clip: true

        Rectangle {
            x: -root.tokens.toastAccentWidth
            width: root.tokens.toastAccentWidth * 2
            height: parent.height
            radius: root.tokens.toastAccentWidth
            color: root.tokens.toastAccent
            antialiasing: true
        }
    }

    // The hairline, just inside the edge: the same treatment as the panel's,
    // its alpha premultiplied by this box's own opacity in theme_for().
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: root.tokens.boxRadius
        border.color: root.tokens.toastHairline
        border.width: 1
        antialiasing: true
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.paddingX
        anchors.rightMargin: root.paddingX
        anchors.topMargin: root.paddingY
        anchors.bottomMargin: root.paddingY
        spacing: root.avatarGap

        Item {
            id: avatar
            objectName: "messageAvatar"
            // Both dimensions come from the diameter rather than height following
            // width: inside a layout, width is not a free variable, and
            // "height: width" closed a loop through it.
            readonly property real diameter: root.avatarDiameter

            width: diameter
            height: diameter
            Layout.preferredWidth: diameter
            Layout.preferredHeight: diameter
            // Centred against the text, as panel.cpp centres it: whichever of the
            // two is taller decides the box, and the shorter one sits in the middle
            // of it. Drawn from the top, the picture of a two-line message sat five
            // units above the centre of its own box.
            Layout.alignment: Qt.AlignVCenter

            AvatarPlaceholder {
                anchors.fill: parent
                discColour: root.tokens.avatarPlaceholder
                markColour: root.tokens.avatarMark
            }

            // No picture, and no machinery to load one: the example carries no
            // face, so what stood here -- the desktop's `user-identity` icon,
            // masked to a circle and drawn over the placeholder -- could only
            // put a second silhouette on top of the first. ConfigBridge's
            // participants() says the rest.
        }

        ColumnLayout {
            id: text
            // No gap between the sender and the message beyond the leading each
            // line already carries. It used to be `rowSpacing`, the distance
            // between two participants, which is a distance between two rows of a
            // list and not between two lines of one message -- and it read as a
            // whole empty line under the name. panel.cpp pushes zero for the same
            // reason and in the same words.
            spacing: 0
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            Label {
                objectName: "messageTitle"
                text: root.config.notificationPreview.title
                color: root.tokens.toastTitle
                font: root.config.overlayFont(root.textPixels * Theme.pointsPerPixel, true)
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.preferredHeight: root.fontPixels

                Repeater {
                    model: root.outlineStrength > 0.0 ? root.outlineOffsets : []

                    Label {
                        required property var modelData
                        z: -1
                        x: modelData[0]
                        y: modelData[1]
                        width: parent.width
                        text: parent.text
                        font: parent.font
                        color: Qt.rgba(root.tokens.textOutlineInk.r,
                                       root.tokens.textOutlineInk.g,
                                       root.tokens.textOutlineInk.b, root.outlineStrength)
                        verticalAlignment: parent.verticalAlignment
                        elide: parent.elide
                    }
                }
            }

            Label {
                id: bodyLabel
                objectName: "messageBody"
                text: root.config.notificationPreview.body
                // Gone, not blank, when the example carries no body: the box
                // shrinks by the body's line exactly as the drawn toast does.
                // Qualified through the id on purpose: the unqualified `text`
                // in this binding evaluated non-empty while the property held
                // "" (measured through the geometry dump -- textLen 0, visible
                // true), and the qualified form is the one that reads the
                // Label's own property. toast_preview_honest holds this.
                visible: bodyLabel.text !== ""
                color: root.tokens.toastBody
                font: root.config.overlayFont(root.textPixels * Theme.pointsPerPixel, false)
                // Wrapped and not truncated, because the overlay wraps and does not
                // truncate either: a message that reads to the end here reads to the
                // end in the game.
                wrapMode: Text.WordWrap
                // A line of wrapped text is exactly the font size tall in ImGui.
                lineHeight: root.fontPixels
                lineHeightMode: Text.FixedHeight
                Layout.fillWidth: true

                // The copies wrap at the parent's own width, for the reason
                // panel.cpp gives its wrapped copies the identical wrap width: a
                // copy that breaks its lines elsewhere is a second paragraph.
                Repeater {
                    model: root.outlineStrength > 0.0 ? root.outlineOffsets : []

                    Label {
                        required property var modelData
                        z: -1
                        x: modelData[0]
                        y: modelData[1]
                        width: parent.width
                        text: parent.text
                        font: parent.font
                        color: Qt.rgba(root.tokens.textOutlineInk.r,
                                       root.tokens.textOutlineInk.g,
                                       root.tokens.textOutlineInk.b, root.outlineStrength)
                        wrapMode: parent.wrapMode
                        lineHeight: parent.lineHeight
                        lineHeightMode: parent.lineHeightMode
                    }
                }
            }
        }
    }
}
