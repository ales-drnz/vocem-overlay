// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The voice panel as the game will draw it.
//
// This is a scale model, not an impression. Every measurement below is the one
// common/src/panel.cpp uses, expressed in the same units, so that what is shown
// here is the size the panel really is relative to the screen. The two are
// compared as numbers rather than by eye: `scripts/compare-preview.py` measures
// the real panel out of the vertices ImGui produces and this preview out of its
// own scene graph, and prints the difference. Every figure here was arrived at
// that way, and changing one without re-running it is how three earlier attempts
// each closed one gap and opened another.
//
// The arithmetic that makes a faithful preview possible: the overlay builds its
// font at 16 * (H / 1080) * scale device pixels and lays everything out as a
// multiple of that, so a distance of N overlay units is N * (H / 1080) * scale
// device pixels. Rendered into a map that is mapHeight units tall and stands for
// the whole screen, one device pixel is mapHeight / H units -- and H cancels:
//
//     distance in map units = N * scale * mapHeight / 1080
//
// So a faithful preview needs the display's aspect ratio and nothing else. That
// matters, because the real resolution is not something Qt will reliably report on
// a fractionally scaled Wayland session -- Screen.width is logical, and
// Screen.devicePixelRatio disagreed with the window's own by a third.
//
// Drawn at the reference size and scaled as a whole, rather than laid out at
// whatever size the view happens to be. That is not a stylistic choice: Qt takes a
// whole number of pixels for a font size and Qt Quick's layouts round item sizes to
// whole pixels, so a panel laid out directly at a fortieth of a screen quantises
// every distance in it -- measured, two identical rows came out 35.3 and 32.9
// overlay units apart in the position map. At the reference size those roundings
// are one overlay pixel, which is the same rounding the overlay itself does.
//
// What is deliberately not reproduced: the overlay rounds its font size and its
// spacings to whole *device* pixels, and a device pixel is not a unit this view
// has. It is worth half a pixel of the real thing at most.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

Rectangle {
    id: root

    required property var config

    // One unit is one overlay pixel at 1080 lines of display height, where the
    // overlay's text is 16 pixels. The view scales the whole item to whatever it
    // stands for; nothing in here needs to know what that is.
    // The height of a line of text, from the setting: the layout is expressed
    // in these units and the overlay divides by the same number.
    readonly property real fontPixels: config.fontSize
    // The gap between one person and the next -- the layout's spacing, never a
    // part of any row's own height: a row that carried it inside itself counted
    // it twice in the pitch, and a picture derived from line-plus-gap resized
    // with the spacing slider. panel.cpp works from the bare line for the same
    // reasons, in the same words. Sideways it is the same distance turned
    // ninety degrees, which is what panel.cpp hands to SameLine.
    readonly property real rowGap: config.rowSpacing

    // Which way the people advance. panel.cpp reads the same setting and calls
    // it the same thing; everything else about a person -- the picture, the
    // ring, the badge, the name beside it -- is identical in both.
    readonly property bool horizontal: config.panelLayout === 1
    readonly property real paddingX: config.boxPaddingX
    readonly property real paddingY: config.boxPaddingY
    readonly property real avatarGap: config.avatarGap

    // Every colour and proportion below comes from include/vocem/theme.h, through
    // the bridge, so this preview and the drawing cannot hold different opinions
    // about them. What used to be here was a hand-written copy of each one.
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
        "panelTextOutline",
    ])

    // panel.cpp: radius = line_height * avatar_radius_factor * avatar_size --
    // the bare line, so only the text-size and avatar sliders reach the picture.
    readonly property real avatarRadius:
        fontPixels * root.tokens.avatarRadiusFactor * config.avatarSize
    readonly property real avatarDiameter: avatarRadius * 2
    // The ring, as panel.cpp draws it: this far outside the picture, with a
    // stroke that follows the radius (floored at one unit, as the overlay floors
    // it at a device pixel), and around *everybody* -- transparent at rest,
    // which is what keeps a row from reflowing when somebody starts talking.
    readonly property real ringOffset: root.tokens.ringOffset
    readonly property real ringWidth: Math.max(1, avatarRadius * root.tokens.ringWidthFactor)

    // The outline around the text, on the same terms as panel.cpp: permanent
    // while the switch is on, drawn as four offset copies of the text in the
    // outline's ink -- one per cardinal direction, which is what the overlay
    // draws. Already resolved against the switch by theme_for(); the strength and
    // the ink arrive as tokens and nothing is re-derived here.
    readonly property real outlineStrength: root.tokens.panelTextOutline
    readonly property var outlineOffsets: [[1, 0], [-1, 0], [0, 1], [0, -1]]

    // What the picture's decorations need beyond it, and therefore what the row
    // reserves for them: the speaking ring is drawn 2.5 units outside the picture
    // with a 2.5-unit stroke centred on that circle, and the state badge hangs off
    // the corner. panel.cpp reserves it whether or not anybody is speaking, so that
    // a row does not change size when somebody starts, and so does this.
    readonly property real halo: Math.max(root.tokens.ringAllowance,
                                          avatarRadius * root.tokens.badgeAllowanceFactor)
    // The box the picture occupies, and what the gap to the name is measured from.
    readonly property real pictureSize: (avatarRadius + halo) * 2
    // A row is a bare line of text tall, or the picture, whichever is more --
    // the gap between rows is the layout's, entering the pitch exactly once.
    // Ceiled to a whole unit exactly as panel.cpp ceils the row: the badge
    // allowance at 0.375 of the radius lands the picture on a fraction (38.115
    // at the defaults), and the half-unit the overlay rounds up was the largest
    // single divergence in the comparison until this rounded with it.
    readonly property real rowSize: Math.ceil(Math.max(fontPixels, pictureSize))

    // The channel name, the line under it, and the spacing on either side of
    // that line -- plus the one unit the line itself claims of the column.
    readonly property real channelBlock: config.showChannelName ? fontPixels + rowGap * 2 + 1 : 0
    readonly property real firstRowY: paddingY + channelBlock

    // The overlay's own font, at the size the overlay would draw it. Both halves
    // matter: the family, because the box ends where its longest name ends, and the
    // correction, because ImGui and Qt do not mean the same thing by a font size.
    readonly property real textPixels: fontPixels * config.overlayFontRatio

    // The surface, at the opacity it is set to, with no floor under it: there was
    // one, so that the box could always be seen on the page it is dragged from;
    // it meant the picture stopped changing halfway down the slider, on the only
    // page that shows the panel at all. What keeps it grabbable there is an
    // outline, which is the window's own furniture rather than a claim about the
    // game.
    color: Qt.rgba(root.tokens.panelSurface.r, root.tokens.panelSurface.g,
                   root.tokens.panelSurface.b, config.opacity)
    // panel.cpp: WindowRounding at the reference size, scaled with everything else.
    radius: root.tokens.boxRadius
    antialiasing: true
    // panel.cpp uses AlwaysAutoResize between these two constraints, so the box
    // ends where its longest name ends.
    implicitWidth: Math.min(520, Math.max(80, contents.implicitWidth + paddingX * 2))
    implicitHeight: contents.implicitHeight + paddingY * 2

    // The hairline, just inside the edge, exactly as panel.cpp draws it by hand:
    // Qt paints a Rectangle's border inside its bounds, which is the same place
    // as ImGui's stroke at half a thickness of inset. The token carries its
    // alpha, premultiplied by the opacity, so a faded panel takes its hairline
    // with it without this file knowing why.
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

        Label {
            objectName: "channelName"
            visible: root.config.showChannelName
            text: root.config.channelName
            color: root.tokens.textChannel

            // Behind it, one copy per cardinal direction: the same five draws the
            // overlay makes.
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
            // A line of text is exactly the font size tall in ImGui, where Qt's own
            // line box is taller than that. Fixing the height is what keeps
            // everything below it where the overlay puts it.
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.preferredHeight: root.fontPixels
        }

        // The line under the channel name, as panel.cpp draws it by hand: the
        // blurple accent bar, then the hairline to the edge. One unit tall in
        // the layout, because that is what the overlay's line claims of the
        // column it sits in (a one-thickness item, like ImGui::Separator's).
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

        // One container for both layouts: a grid of one column is the list this
        // has always drawn, and a grid of one row is the same people advancing
        // sideways. Two containers with the Repeater moved between them would be
        // two places for a person's geometry to be described.
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

                    implicitWidth: root.pictureSize + root.avatarGap + label.implicitWidth
                    implicitHeight: root.rowSize
                    // Upright, a row takes the width of the box so its name can
                    // elide against the far padding; sideways, a person is
                    // exactly as wide as they are, because that width is what
                    // the next person starts after.
                    Layout.fillWidth: !root.horizontal
                    Layout.preferredHeight: root.rowSize

                    // The picture's box. The picture is centred in it and the ring and
                    // the badge live in the room around it, so nothing a row draws
                    // crosses into the padding or into the row below.
                    Item {
                        id: picture

                        width: root.pictureSize
                        height: root.rowSize

                        Item {
                            objectName: "avatar"
                            anchors.centerIn: parent
                            width: root.avatarDiameter
                            height: root.avatarDiameter

                            AvatarPlaceholder {
                                anchors.fill: parent
                                discColour: root.tokens.avatarPlaceholder
                                markColour: root.tokens.avatarMark
                            }

                            // No picture is loaded here, and there is no longer
                            // any machinery to load one. The example roster has
                            // no faces (the window has no Discord pictures to
                            // show), so what stood here -- an Image of the
                            // desktop's `user-identity` icon, masked to a circle
                            // and drawn over the placeholder -- could only ever
                            // put a second, different silhouette on top of the
                            // first. That was the grey shadow under the figure.

                            // Muted or deafened: the picture is dimmed and a badge names
                            // which of the two, the same as in game.
                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                antialiasing: true
                                color: root.tokens.avatarScrim
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

                            // The ring, outside the picture rather than on its edge,
                            // matching what the overlay draws: always there, clear
                            // unless this person is speaking.
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width + root.ringOffset * 2
                                height: width
                                radius: width / 2
                                color: "transparent"
                                antialiasing: true
                                // 1/255 at rest, matching the overlay to the part:
                                // ImGui culls alpha exactly 0, so the ring's resting
                                // alpha is the smallest one it will actually draw.
                                border.color: Qt.rgba(root.tokens.speakingRing.r,
                                                      root.tokens.speakingRing.g,
                                                      root.tokens.speakingRing.b,
                                                      person.modelData.speaking ? 1.0 : 1 / 255)
                                border.width: root.ringWidth
                            }
                        }
                    }

                    Label {
                        id: label
                        objectName: "name"

                        x: picture.width + root.avatarGap
                        // panel.cpp centres the name against the picture rather than
                        // leaving it where ImGui puts an item by default, which is the
                        // top of the row and looked high beside a large avatar.
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, Math.min(label.implicitWidth,
                                                    person.width - picture.width - root.avatarGap))
                        height: root.fontPixels
                        verticalAlignment: Text.AlignVCenter

                        text: person.modelData.name
                        // The same three the overlay draws: whoever is speaking at
                        // full strength, everybody else greyed, and a muted
                        // participant dimmer still. panel.cpp is where the colours
                        // are decided; these are the same numbers.
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
