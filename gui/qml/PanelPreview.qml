// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The voice panel as the game will draw it: a scale model, every measurement
// the one common/src/panel.cpp uses, in the same units. scripts/compare-preview.py
// measures the real panel from ImGui's vertices and this one from its scene
// graph; re-run it after changing any figure here.
//
// The overlay lays out in multiples of 16 * (H / 1080) * scale device pixels,
// so in a map mapHeight units tall standing for the whole screen H cancels:
//
//     distance in map units = N * scale * mapHeight / 1080
//
// Only the display's aspect ratio is needed, which matters because Qt does not
// reliably report the real resolution on a fractionally scaled Wayland session.
//
// Drawn at the reference size and scaled as a whole: Qt rounds font sizes and
// item sizes to whole pixels, so a panel laid out at a fortieth of a screen
// would quantise every distance. The overlay's own rounding to device pixels is
// not reproduced; it is worth half a pixel at most.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Rectangle {
    id: root

    required property var config

    // One unit is one overlay pixel at 1080 lines of display height. The
    // layout is expressed in lines of text, as the overlay's is.
    readonly property real fontPixels: config.fontSize
    // The gap between people: the layout's spacing, never part of a row's own
    // height, so it enters the pitch once (as in panel.cpp). Sideways it is
    // the same distance, which panel.cpp hands to SameLine.
    readonly property real rowGap: config.rowSpacing

    // Which way the people advance (panel.cpp's same setting); a person is
    // drawn identically in both.
    readonly property bool horizontal: config.panelLayout === 1
    // Where the surface is drawn: around everything, or behind each name.
    readonly property bool nameBox: config.panelBox === 1
    readonly property real paddingX: config.boxPaddingX
    readonly property real paddingY: config.boxPaddingY
    readonly property real avatarGap: config.avatarGap

    // Every colour and proportion comes from include/vocem/theme.h through the
    // bridge, so the preview and the drawing cannot disagree.
    readonly property var tokens: config.overlayTheme

    Component.onCompleted: Theme.requireTokens(root.tokens, "PanelPreview", [
        "panelSurface", "separator", "textChannel", "textSpeaking", "textIdle",
        "textMuted", "avatarPlaceholder", "avatarMark", "avatarScrim", "speakingRing",
        "badgeFill", "badgeRim", "badgeGlyph", "textOutlineInk",
        "panelHairline",
        "boxRadius", "ringOffset", "ringWidthFactor", "ringAllowance",
        "separatorAccent", "separatorAccentLength",
        "avatarRadiusFactor", "badgeOffsetFactor", "badgeRadiusFactor",
        "badgeStrokeFactor", "badgeRimStrokeFactor", "badgeAllowanceFactor",
        "nameBoxPaddingX", "nameBoxPaddingY",
        "panelTextOutline",
    ])

    // panel.cpp: radius = line_height * avatar_radius_factor * avatar_size.
    readonly property real avatarRadius:
        fontPixels * root.tokens.avatarRadiusFactor * config.avatarSize
    readonly property real avatarDiameter: avatarRadius * 2
    // The ring: outside the picture, stroke following the radius (floored at
    // one unit), around everybody -- transparent at rest, so a row does not
    // reflow when somebody starts talking.
    readonly property real ringOffset: root.tokens.ringOffset
    readonly property real ringWidth: Math.max(1, avatarRadius * root.tokens.ringWidthFactor)

    // How far a name's own box reaches past its glyphs; zero when the surface
    // is around everything, as in panel.cpp.
    readonly property real nameBoxPadX: nameBox ? root.tokens.nameBoxPaddingX : 0
    readonly property real nameBoxPadY: nameBox ? root.tokens.nameBoxPaddingY : 0
    // One line of text plus its box. The row is at least this tall, so two
    // pills never overlap into a darker band.
    readonly property real textBlock: fontPixels + nameBoxPadY * 2

    // The name box's colour: the panel's surface at the panel's opacity.
    readonly property color nameBoxColour: Qt.rgba(root.tokens.panelSurface.r,
                                                   root.tokens.panelSurface.g,
                                                   root.tokens.panelSurface.b,
                                                   config.opacity)

    // The text outline, as panel.cpp draws it: four offset copies in the
    // outline's ink, one per cardinal direction. Strength and ink arrive
    // resolved by theme_for().
    readonly property real outlineStrength: root.tokens.panelTextOutline
    readonly property var outlineOffsets: [[1, 0], [-1, 0], [0, 1], [0, -1]]

    // Room reserved for the speaking ring and the state badge, whether or not
    // anybody is speaking, so a row keeps its size (as panel.cpp does).
    readonly property real halo: Math.max(root.tokens.ringAllowance,
                                          avatarRadius * root.tokens.badgeAllowanceFactor)
    // The box the picture occupies; the gap to the name is measured from it.
    readonly property real pictureSize: (avatarRadius + halo) * 2
    // A row is a bare line tall or the picture, whichever is more. Ceiled as
    // panel.cpp ceils it: the badge allowance puts the picture on a fraction
    // (38.115 at the defaults).
    readonly property real rowSize: Math.ceil(Math.max(textBlock, pictureSize))

    // The channel name, the line under it with spacing either side, and the
    // one unit the line claims of the column.
    readonly property real channelBlock: config.showChannelName ? textBlock + rowGap * 2 + 1 : 0
    readonly property real firstRowY: paddingY + channelBlock

    // The overlay's own font at the overlay's size: the family decides where
    // the box ends, and the ratio corrects for ImGui and Qt meaning different
    // things by a font size.
    readonly property real textPixels: fontPixels * config.overlayFontRatio

    // How wide the box may grow. A column is capped at 520 units so one long
    // display name cannot cross the screen; a row is capped by the display
    // (panel.cpp says why). widestBox is the display's width in these units,
    // supplied by a view that stands for a display (as NotificationPreview is
    // told), zero otherwise. The 80-unit floor is panel.cpp's.
    property real widestBox: 0
    readonly property real widthLimit: {
        const display = widestBox > 0 ? widestBox : Infinity;
        const wanted = horizontal ? display : 520;
        return Math.max(80, Math.min(wanted, display));
    }

    // The surface at its set opacity, with no floor, so the slider changes the
    // picture all the way down. Nothing at all when the surface is drawn behind
    // the names instead.
    color: root.nameBox ? "transparent"
                        : Qt.rgba(root.tokens.panelSurface.r, root.tokens.panelSurface.g,
                                  root.tokens.panelSurface.b, config.opacity)
    // panel.cpp: WindowRounding at the reference size, scaled with everything else.
    radius: root.tokens.boxRadius
    antialiasing: true
    // panel.cpp uses AlwaysAutoResize between these two constraints.
    implicitWidth: Math.min(widthLimit, Math.max(80, contents.implicitWidth + paddingX * 2))
    implicitHeight: contents.implicitHeight + paddingY * 2

    // The hairline just inside the edge, as panel.cpp draws it. The token's
    // alpha is premultiplied by the opacity, so a faded panel fades it too.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: root.tokens.boxRadius
        border.color: root.tokens.panelHairline
        border.width: 1
        antialiasing: true
    }

    ColumnLayout {
        id: contents
        anchors.fill: parent
        anchors.leftMargin: root.paddingX
        anchors.rightMargin: root.paddingX
        anchors.topMargin: root.paddingY
        anchors.bottomMargin: root.paddingY
        spacing: root.rowGap

        // The channel name and, in the names-only shape, the box behind it.
        // An Item, because a Label cannot be behind itself.
        Item {
            objectName: "channelBlock"
            visible: root.config.showChannelName
            // The name still sets the panel's width, box included.
            implicitWidth: channelName.implicitWidth + root.nameBoxPadX * 2
            Layout.fillWidth: true
            Layout.preferredHeight: root.textBlock

            Rectangle {
                objectName: "channelNameBox"
                visible: root.nameBox
                // Ceiled, because ImGui::CalcTextSize ceils the advance the
                // overlay draws the box from (entry 90).
                width: Math.min(parent.width,
                                Math.ceil(channelName.implicitWidth) + root.nameBoxPadX * 2)
                height: root.textBlock
                radius: height / 2
                color: root.nameBoxColour
                antialiasing: true
            }

            Label {
                id: channelName

                objectName: "channelName"
                x: root.nameBoxPadX
                y: root.nameBoxPadY
                width: Math.max(0, parent.width - root.nameBoxPadX * 2)
                text: root.config.channelName
                color: root.tokens.textChannel

                // One copy per cardinal direction behind it, as the overlay draws.
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
                        color: Qt.rgba(root.tokens.textOutlineInk.r, root.tokens.textOutlineInk.g,
                                       root.tokens.textOutlineInk.b, root.outlineStrength)
                        verticalAlignment: parent.verticalAlignment
                        elide: parent.elide
                    }
                }
                font: root.config.overlayFont(root.textPixels * Theme.pointsPerPixel, true)
                // ImGui's line is exactly the font size tall, Qt's taller: the
                // fixed height keeps everything below where the overlay puts it.
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                height: root.fontPixels
            }
        }

        // The line under the channel name, as panel.cpp draws it: the accent
        // bar, then the hairline to the edge. One unit tall, like ImGui::Separator.
        Item {
            visible: root.config.showChannelName
            Layout.fillWidth: true
            Layout.preferredHeight: 1

            Rectangle {
                id: separatorAccent
                width: Math.min(root.tokens.separatorAccentLength, parent.width)
                height: 1
                color: root.tokens.separatorAccent
            }
            Rectangle {
                x: separatorAccent.width
                width: Math.max(0, parent.width - separatorAccent.width)
                height: 1
                color: root.tokens.separator
            }
        }

        // One grid for both layouts (one column, or one row), so a person's
        // geometry is described in one place.
        GridLayout {
            id: people

            columns: root.horizontal ? Math.max(1, root.config.participants.length) : 1
            rowSpacing: root.rowGap
            columnSpacing: root.rowGap
            Layout.fillWidth: !root.horizontal

            Repeater {
                model: root.config.participants

                Item {
                    id: person

                    required property var modelData

                    implicitWidth: root.pictureSize + root.avatarGap + label.implicitWidth +
                                   root.nameBoxPadX * 2
                    implicitHeight: root.rowSize
                    // Upright, a row fills the box so the name elides against
                    // the padding; sideways, a person is as wide as they are.
                    Layout.fillWidth: !root.horizontal
                    Layout.preferredHeight: root.rowSize

                    // The picture's box: the picture centred, the ring and badge
                    // in the room around it.
                    Item {
                        id: picture

                        width: root.pictureSize
                        height: root.rowSize

                        Item {
                            objectName: "avatar"
                            anchors.centerIn: parent
                            width: root.avatarDiameter
                            height: root.avatarDiameter

                            // Quieter for whoever is not talking (panel.cpp's
                            // picture_alpha): the picture and scrim take it, the
                            // badge and ring do not. A property for the geometry dump.
                            readonly property real pictureOpacity:
                                person.modelData.speaking ? 1.0 : root.config.avatarIdleOpacity

                            AvatarPlaceholder {
                                anchors.fill: parent
                                opacity: parent.pictureOpacity
                                discColour: root.tokens.avatarPlaceholder
                                markColour: root.tokens.avatarMark
                            }

                            // No picture is drawn: the example roster has no
                            // faces, only the placeholder.

                            // Muted or deafened: the picture dimmed and a badge.
                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                antialiasing: true
                                color: root.tokens.avatarScrim
                                opacity: parent.pictureOpacity
                                visible: root.config.showMutedState &&
                                         (person.modelData.muted || person.modelData.deafened)
                            }

                            StateBadge {
                                tokens: root.tokens
                                visible: root.config.showMutedState &&
                                         (person.modelData.muted || person.modelData.deafened)
                                deafened: person.modelData.deafened
                                diameter: parent.width * root.tokens.badgeRadiusFactor
                                x: parent.width * root.tokens.badgeOffsetFactor - diameter / 2
                                y: parent.height * root.tokens.badgeOffsetFactor - diameter / 2
                            }

                            // The ring, outside the picture: always there, clear
                            // unless this person is speaking.
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width + root.ringOffset * 2
                                height: width
                                radius: width / 2
                                color: "transparent"
                                antialiasing: true
                                // 1/255 at rest: ImGui culls alpha exactly 0, so
                                // this is the smallest it draws.
                                border.color: Qt.rgba(root.tokens.speakingRing.r,
                                                      root.tokens.speakingRing.g,
                                                      root.tokens.speakingRing.b,
                                                      person.modelData.speaking ? 1.0 : 1 / 255)
                                border.width: root.ringWidth
                            }
                        }
                    }

                    // The box behind this name in the names-only shape: a pill
                    // (radius half its height), declared first so it is behind,
                    // centred on the row as the name is.
                    Rectangle {
                        objectName: "nameBox"
                        visible: root.nameBox

                        x: picture.width + root.avatarGap
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.ceil(label.width) + root.nameBoxPadX * 2
                        height: root.textBlock
                        radius: height / 2
                        color: root.nameBoxColour
                        antialiasing: true
                    }

                    Label {
                        id: label
                        objectName: "name"

                        x: picture.width + root.avatarGap + root.nameBoxPadX
                        // Centred against the picture, as panel.cpp does.
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, Math.min(label.implicitWidth,
                                                    person.width - picture.width -
                                                        root.avatarGap - root.nameBoxPadX * 2))
                        height: root.fontPixels
                        verticalAlignment: Text.AlignVCenter

                        text: person.modelData.name
                        // Speaking at full strength, others greyed, muted dimmer
                        // still; panel.cpp decides, these are its numbers.
                        color: ((person.modelData.muted || person.modelData.deafened) &&
                                root.config.showMutedState)
                               ? root.tokens.textMuted
                               : person.modelData.speaking ? root.tokens.textSpeaking
                                                           : root.tokens.textIdle
                        font: root.config.overlayFont(root.textPixels * Theme.pointsPerPixel, false)
                        elide: Text.ElideRight

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
                }
            }
        }
    }
}
