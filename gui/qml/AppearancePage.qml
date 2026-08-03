// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The colour and the opacity of the two boxes.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["panelColour", "opacity", "speakingColour", "textIdleColour",
               "textSpeakingColour", "fontSize", "avatarSize", "textShadow",
               "showChannelName", "notificationColour", "notificationOpacity",
               "notificationTextColour"]
    title: qsTr("Appearance")
    subtitle: qsTr("Colour and opacity of the two boxes, set independently.")

    // Both boxes, live, beside the controls -- back by the owner's ask, as a
    // real preview this time: the same components the geometry comparison
    // measures, fed by the bridge's edited copy.
    side: LivePreview {
        objectName: "appearanceLive"
        anchors.fill: parent
        config: root.config
    }

    // ---- the voice panel
    Card {
        title: qsTr("Voice Panel")
        Layout.fillWidth: true

        // The preset row: one real preview per preset, each the panel as that
        // preset would draw it -- PanelPreview fed the theme the bridge derives
        // for the preset (presetThemes), never a swatch mixed here. Clicking one
        // applies the preset. A preset is a starting point, not a mode: it
        // writes the two settings below and they stay individually adjustable,
        // which is why the active preview is derived from the values rather
        // than stored anywhere.
        Item {
            id: presetRow

            // The values come from theme.h through the bridge -- the same table
            // the contrast test holds to the floor -- so this row cannot offer a
            // surface the measurement does not cover. The themes list is index-
            // aligned with the presets list; both walk kPresets.
            readonly property var presets: root.config.overlayPresets
            readonly property var themes: root.config.presetThemes
            function presetLabel(id) {
                switch (id) {
                case "dark": return qsTr("Dark");
                case "light": return qsTr("Light");
                case "purple": return qsTr("Purple");
                case "transparent": return qsTr("Transparent");
                }
                return id;
            }
            function isActive(preset) {
                return Qt.colorEqual(root.config.panelColour, preset.colour) &&
                       Math.abs(root.config.opacity - preset.opacity) < 0.005;
            }

            Layout.fillWidth: true
            implicitHeight: presetColumn.implicitHeight + Theme.cardPadding * 2

            ColumnLayout {
                id: presetColumn

                // The one block in this window that is inside a card without
                // being a SettingRow, so it pads itself -- under the name every
                // padded block carries, which is what puts it inside the
                // padding check instead of leaving two hand-written margins to
                // agree with the rest by luck (Card.qml says what is checked).
                objectName: "cardContent"

                anchors.fill: parent
                anchors.leftMargin: Theme.cardPadding
                anchors.rightMargin: Theme.cardPadding
                anchors.topMargin: Theme.cardPadding
                anchors.bottomMargin: Theme.cardPadding
                spacing: 1

                Label {
                    text: qsTr("Preset")
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("Sets colour and opacity together. You can still change both below.")
                    opacity: 0.65
                    font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Flow {
                    spacing: Theme.smallSpacing
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.smallSpacing

                    Repeater {
                        model: presetRow.presets

                        AbstractButton {
                            id: chip

                            objectName: "presetPreview"

                            required property var modelData
                            required property int index
                            readonly property bool active: presetRow.isActive(modelData)
                            // The theme this preset would produce on top of the
                            // current settings, from the bridge -- the one source
                            // the overlay itself draws from.
                            readonly property var presetTokens: presetRow.themes[index]

                            // What the geometry dump prints for this item, so the
                            // regression test can hold each preview to a distinct
                            // surface: the derived surface as one number, and the
                            // opacity the preset applies.
                            readonly property real surfaceRgb:
                                Math.round(presetTokens.panelSurface.r * 255) * 65536 +
                                Math.round(presetTokens.panelSurface.g * 255) * 256 +
                                Math.round(presetTokens.panelSurface.b * 255)
                            readonly property real presetOpacity: modelData.opacity

                            // PanelPreview reads one config object. This stands in
                            // for the bridge with the preset's two writes applied
                            // -- the same two onClicked makes -- and the theme
                            // those writes produce; everything else passes
                            // through, so the previews follow the other controls
                            // live.
                            readonly property QtObject presetConfig: QtObject {
                                readonly property real fontSize: root.config.fontSize
                                readonly property real rowSpacing: root.config.rowSpacing
                                readonly property real boxPaddingX: root.config.boxPaddingX
                                readonly property real boxPaddingY: root.config.boxPaddingY
                                readonly property real avatarGap: root.config.avatarGap
                                readonly property real avatarSize: root.config.avatarSize
                                readonly property real opacity: chip.modelData.opacity
                                readonly property bool showChannelName:
                                    root.config.showChannelName
                                readonly property bool showMutedState:
                                    root.config.showMutedState
                                readonly property string channelName: root.config.channelName
                                readonly property var participants: root.config.participants
                                readonly property real overlayFontRatio:
                                    root.config.overlayFontRatio
                                readonly property var overlayTheme: chip.presetTokens
                                function overlayFont(points, strong) {
                                    return root.config.overlayFont(points, strong);
                                }
                            }

                            padding: 0
                            activeFocusOnTab: true
                            hoverEnabled: true

                            Accessible.role: Accessible.Button
                            Accessible.name: presetRow.presetLabel(modelData.id)

                            onClicked: {
                                root.config.panelColour = modelData.colour;
                                root.config.opacity = modelData.opacity;
                            }

                            contentItem: Column {
                                spacing: 2

                                // The panel as this preset draws it, over a token
                                // of the dark scene, so "Transparent" honestly
                                // shows no box. Drawn at the overlay's reference
                                // size and scaled as a whole to fit the cell --
                                // never laid out small.
                                Rectangle {
                                    id: cell

                                    width: 148
                                    height: 96
                                    gradient: Theme.gameBackdrop
                                    radius: Theme.cornerRadius
                                    border.color: Qt.rgba(1, 1, 1, 0.12)
                                    border.width: 1
                                    clip: true

                                    Item {
                                        readonly property real factor: Math.min(
                                            1,
                                            (cell.width - 12) /
                                                Math.max(1, sample.implicitWidth),
                                            (cell.height - 12) /
                                                Math.max(1, sample.implicitHeight))

                                        anchors.centerIn: parent
                                        width: sample.implicitWidth * factor
                                        height: sample.implicitHeight * factor

                                        PanelPreview {
                                            id: sample

                                            config: chip.presetConfig
                                            transformOrigin: Item.TopLeft
                                            scale: parent.factor
                                            width: implicitWidth
                                            height: implicitHeight
                                        }
                                    }

                                    // The active preset, marked on the cell itself:
                                    // the highlight frame, present exactly when the
                                    // two settings stand at this preset's values.
                                    Rectangle {
                                        objectName: "presetActiveMark"
                                        anchors.fill: parent
                                        radius: Theme.cornerRadius
                                        color: "transparent"
                                        border.width: 2
                                        border.color: Theme.palette.highlight
                                        visible: chip.active
                                        antialiasing: true
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Theme.cornerRadius
                                        color: "transparent"
                                        border.width: 2
                                        border.color: Theme.palette.highlight
                                        opacity: 0.5
                                        visible: !chip.active &&
                                                 (chip.hovered || chip.activeFocus)
                                        antialiasing: true
                                    }
                                }

                                Label {
                                    width: cell.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: presetRow.presetLabel(chip.modelData.id)
                                    font.bold: chip.active
                                }
                            }

                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }
                }
            }
        }

        SettingRow {
            label: qsTr("Colour")
            description: qsTr("Background of the voice panel.")

            ColourButton {
                colour: root.config.panelColour
                defaultColour: root.config.defaultPanelColour
                title: qsTr("Voice panel colour")
                onPicked: function(chosen) { root.config.panelColour = chosen; }
            }
        }

        // Opacity, not transparency: the figure rises as the box
        // becomes more solid, so the other word said the opposite of
        // what the number did.
        // The description carries the warning rather than a third line
        // under the row: an opacity that reached zero by accident looks
        // exactly like a broken overlay, so the row has to say which of
        // the two it is.
        SettingRow {
            label: qsTr("Opacity")
            description: root.config.backgroundFaint
                         ? qsTr("No background: names and avatars only.")
                         : qsTr("How solid the panel background is.")

            SliderRow {
                accessibleName: qsTr("Panel opacity")
                from: 0; to: 100; stepSize: 2
                decimals: 0
                suffix: "%"
                value: Math.round(root.config.opacity * 100)
                defaultValue: Math.round(root.config.defaultOpacity * 100)
                onMoved: function(chosen) { root.config.opacity = chosen / 100; }
            }
        }

        SettingRow {
            label: qsTr("Speaking ring")
            description: qsTr("Ring around participants who are speaking.")

            ColourButton {
                colour: root.config.speakingColour
                defaultColour: root.config.defaultSpeakingColour
                title: qsTr("Speaking ring colour")
                onPicked: function(chosen) { root.config.speakingColour = chosen; }
            }
        }

        // The two text colours a user may pin. The swatch shows what the overlay
        // actually draws -- the ramp's colour while the setting is auto -- so the
        // reset does not go blank, it reveals what auto stands for. The reset is
        // its own action rather than picking the shown colour back: picking the
        // ramp's current value would pin it, and a pinned colour stays put when
        // the box changes where auto follows it.
        SettingRow {
            label: qsTr("Name colour")
            description: qsTr("The colour of names when somebody is not speaking. Auto picks pale text on a dark box, and dark text on a pale one.")

            ColourButton {
                colour: root.config.effectiveTextIdleColour
                changed: root.config.textIdleColour !== "auto"
                title: qsTr("Idle name colour")
                onPicked: function(chosen) { root.config.textIdleColour = String(chosen); }
                resetAction: function() { root.config.textIdleColour = "auto"; }
            }
        }

        SettingRow {
            label: qsTr("Speaking name")
            description: qsTr("The name of whoever is speaking. Auto follows the box.")

            ColourButton {
                colour: root.config.effectiveTextSpeakingColour
                changed: root.config.textSpeakingColour !== "auto"
                title: qsTr("Speaking name colour")
                onPicked: function(chosen) { root.config.textSpeakingColour = String(chosen); }
                resetAction: function() { root.config.textSpeakingColour = "auto"; }
            }
        }

        // A number rather than a slider: it is a size in the same units as the
        // padding on the Spacing page, and it is read off the box as often as it
        // is dragged.
        SettingRow {
            label: qsTr("Text size")
            description: qsTr("The height of one line of text. Pictures and rows grow with it. Padding and the distance from the edge do not. To scale everything at once, use Panel size.")

            SpinRow {
                accessibleName: qsTr("Text size")
                from: 8; to: 48
                value: root.config.fontSize
                defaultValue: root.config.defaultFontSize
                onMoved: function(chosen) { root.config.fontSize = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Avatars")
            description: qsTr("Avatar diameter.")

            SliderRow {
                accessibleName: qsTr("Avatar size")
                from: 0.5; to: 2.0; stepSize: 0.05
                suffix: "×"
                value: root.config.avatarSize
                defaultValue: root.config.defaultAvatarSize
                onMoved: function(chosen) { root.config.avatarSize = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Text outline")
            description: qsTr("A dark edge around the names. You cannot see it on a solid box, and it appears as soon as the background stops covering the text.")

            CheckBox {
                checked: root.config.textShadow
                onToggled: root.config.textShadow = checked
                Accessible.name: qsTr("Text outline")
            }
        }

        SettingRow {
            label: qsTr("Channel name")
            description: qsTr("Show the channel name above the list.")

            CheckBox {
                checked: root.config.showChannelName
                onToggled: root.config.showChannelName = checked
                Accessible.name: qsTr("Show the channel name")
            }
        }
    }

    // ---- messages
    Card {
        title: qsTr("Messages")
        Layout.fillWidth: true
        // Nothing on this card can be seen with messages switched off,
        // so the style greys it rather than leaving it live.
        enabled: root.config.notificationsEnabled

        SettingRow {
            label: qsTr("Colour")
            description: qsTr("Background of the message box.")
            first: true

            ColourButton {
                colour: root.config.notificationColour
                defaultColour: root.config.defaultNotificationColour
                title: qsTr("Message colour")
                onPicked: function(chosen) { root.config.notificationColour = chosen; }
            }
        }

        SettingRow {
            label: qsTr("Text colour")
            description: qsTr("The colour of the message text. Auto follows the box. The sender's name stays bold either way.")

            ColourButton {
                colour: root.config.effectiveNotificationTextColour
                changed: root.config.notificationTextColour !== "auto"
                title: qsTr("Message text colour")
                onPicked: function(chosen) { root.config.notificationTextColour = String(chosen); }
                resetAction: function() { root.config.notificationTextColour = "auto"; }
            }
        }

        SettingRow {
            label: qsTr("Opacity")
            description: qsTr("How solid the message background is. A solid background is the easiest to read in a game.")

            SliderRow {
                accessibleName: qsTr("Message opacity")
                from: 20; to: 100; stepSize: 2
                decimals: 0
                suffix: "%"
                value: Math.round(root.config.notificationOpacity * 100)
                defaultValue: Math.round(root.config.defaultNotificationOpacity * 100)
                onMoved: function(chosen) { root.config.notificationOpacity = chosen / 100; }
            }
        }
    }
}
