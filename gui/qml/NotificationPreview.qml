// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stand-in for the notification toast, matching what the overlay draws: same
// font, same measurements, same rounded avatar. Every figure is the one
// common/src/panel.cpp uses, compared as numbers by scripts/compare-preview.py.
// The message's own size applies to everything in the box, so the view scales
// the whole item rather than this file scaling each figure.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Rectangle {
    id: root

    required property var config
    // One unit is one overlay pixel at 1080 lines of display height; the view
    // scales the box by the user's scale and the message's own size.
    // The height of a line of text, from the setting.
    readonly property real fontPixels: config.fontSize
    readonly property real paddingX: config.boxPaddingX
    readonly property real paddingY: config.boxPaddingY
    readonly property real avatarGap: config.avatarGap
    // panel.cpp: avatar_radius = GetTextLineHeight() * 0.9, so a diameter of 1.8
    // times the font size.
    readonly property real avatarDiameter: fontPixels * 1.8
    readonly property real textPixels: fontPixels * config.overlayFontRatio

    // The overlay's box is 320 units wide, and never wider than its display; the
    // view supplies that limit, since only it knows how much screen it stands for.
    property real widestBox: 0
    readonly property real boxWidth: widestBox > 0 ? Math.min(320, widestBox) : 320

    // Every colour comes from include/vocem/theme.h through the bridge, which
    // has already chosen the palette for a pale box or a dark one.
    readonly property var tokens: config.overlayTheme

    Component.onCompleted: Theme.requireTokens(root.tokens, "NotificationPreview", [
        "toastSurface", "toastTitle", "toastBody", "avatarPlaceholder", "avatarMark", "boxRadius",
        "toastHairline", "toastAccent", "toastAccentWidth",
        "textOutlineInk", "toastTextOutline",
    ])

    // The overlay's text outline: four offset copies of the text in the
    // outline's ink (see PanelPreview), governed by the toast's strength token.
    readonly property real outlineStrength: root.tokens.toastTextOutline
    readonly property var outlineOffsets: [[1, 0], [-1, 0], [0, 1], [0, -1]]

    color: Qt.rgba(root.tokens.toastSurface.r, root.tokens.toastSurface.g,
                   root.tokens.toastSurface.b, config.notificationOpacity)
    antialiasing: true
    radius: root.tokens.boxRadius
    implicitWidth: boxWidth
    implicitHeight: Math.max(avatar.height, text.implicitHeight) + paddingY * 2

    // The accent bar down the left edge, as panel.cpp draws it: stopped where
    // the corner curvature starts, right-hand corners rounded by its own width.
    // The rectangle is twice the bar's width and the clip cuts its left half.
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
            // Both dimensions from the diameter: inside a layout, "height: width"
            // closes a binding loop.
            readonly property real diameter: root.avatarDiameter

            width: diameter
            height: diameter
            Layout.preferredWidth: diameter
            Layout.preferredHeight: diameter
            // Centred against the text, as panel.cpp centres it: the taller of
            // the two decides the box and the shorter sits in its middle.
            Layout.alignment: Qt.AlignVCenter

            AvatarPlaceholder {
                anchors.fill: parent
                discColour: root.tokens.avatarPlaceholder
                markColour: root.tokens.avatarMark
            }

            // No picture: the example carries no face (see ConfigBridge's
            // participants()).
        }

        ColumnLayout {
            id: text
            // No gap between sender and message beyond each line's own leading,
            // as in panel.cpp: rowSpacing is between participants, not lines.
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
                // Gone, not blank, when the example carries no body, as in the
                // drawn toast. Qualified through the id on purpose: the bare
                // `text` here evaluates non-empty while the property holds "".
                // toast_preview_honest holds this.
                visible: bodyLabel.text !== ""
                color: root.tokens.toastBody
                font: root.config.overlayFont(root.textPixels * Theme.pointsPerPixel, false)
                // Wrapped, not truncated, as the overlay does.
                wrapMode: Text.WordWrap
                // A line of wrapped text is exactly the font size tall in ImGui.
                lineHeight: root.fontPixels
                lineHeightMode: Text.FixedHeight
                Layout.fillWidth: true

                // The copies wrap at the parent's width, as in panel.cpp: a copy
                // that breaks its lines elsewhere is a second paragraph.
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
