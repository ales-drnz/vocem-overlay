// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The Appearance section: the colour, opacity and type of the voice panel and
// of the message box.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

ScrollablePage {
    id: root

    settings: ["panelLayout", "panelBox", "panelColour", "opacity", "speakingColour",
               "textIdleColour",
               "textSpeakingColour", "fontFamily", "fontSize", "avatarSize",
               "avatarIdleOpacity", "textShadow",
               "showChannelName", "notificationColour", "notificationOpacity",
               "notificationTextColour"]
    title: qsTr("Appearance")
    subtitle: qsTr("Colour and opacity of the two boxes, set independently.")

    // Both boxes, live, beside the controls: the same components the geometry
    // comparison measures, fed by the bridge's edited copy.
    side: LivePreview {
        objectName: "appearanceLive"
        anchors.fill: parent
        config: root.config
    }

    // ---- the voice panel
    Card {
        title: qsTr("Voice Panel")
        Layout.fillWidth: true

        // The preset row: one PanelPreview per preset, fed the theme the bridge
        // derives for it (presetThemes). Clicking one writes its settings, which
        // stay individually adjustable; the active chip is derived from the
        // values rather than stored.
        Item {
            id: presetRow

            // From theme.h through the bridge, the table the contrast test holds
            // to the floor. themes is index-aligned with presets (both walk
            // kPresets).
            readonly property var presets: root.config.overlayPresets
            readonly property var themes: root.config.presetThemes
            function presetLabel(id) {
                switch (id) {
                // Named for the shape of the box, not for any client's overlay.
                case "pills": return qsTr("Pills");
                case "dark": return qsTr("Dark");
                case "light": return qsTr("Light");
                case "purple": return qsTr("Purple");
                case "transparent": return qsTr("Transparent");
                }
                return id;
            }
            // All three writes: Pills and Dark share surface and opacity and
            // differ only in the box.
            function isActive(preset) {
                return Qt.colorEqual(root.config.panelColour, preset.colour) &&
                       Math.abs(root.config.opacity - preset.opacity) < 0.005 &&
                       root.config.panelBox === preset.box;
            }

            Layout.fillWidth: true
            implicitHeight: presetColumn.implicitHeight + Theme.cardPadding * 2

            ColumnLayout {
                id: presetColumn

                // The one padded block in a card that is not a SettingRow: the
                // name puts it inside the padding check (see Card.qml).
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
                            // The theme this preset would produce, from the bridge.
                            readonly property var presetTokens: presetRow.themes[index]

                            // Printed by the geometry dump so the test can hold each
                            // preview to a distinct surface and opacity.
                            readonly property real surfaceRgb:
                                Math.round(presetTokens.panelSurface.r * 255) * 65536 +
                                Math.round(presetTokens.panelSurface.g * 255) * 256 +
                                Math.round(presetTokens.panelSurface.b * 255)
                            readonly property real presetOpacity: modelData.opacity
                            // And its box, which tells apart chips that share the
                            // other two numbers.
                            readonly property real presetBox: modelData.box

                            // Stands in for the bridge with the preset's writes
                            // applied (as onClicked makes them) and their theme.
                            readonly property QtObject presetConfig: QtObject {
                                // Everything else passes through, the layout
                                // included, so the chip shows the user's shape.
                                readonly property int panelLayout: root.config.panelLayout
                                readonly property int panelBox: chip.modelData.box
                                readonly property real fontSize: root.config.fontSize
                                readonly property real rowSpacing: root.config.rowSpacing
                                readonly property real boxPaddingX: root.config.boxPaddingX
                                readonly property real boxPaddingY: root.config.boxPaddingY
                                readonly property real avatarGap: root.config.avatarGap
                                readonly property real avatarSize: root.config.avatarSize
                                readonly property real avatarIdleOpacity:
                                    root.config.avatarIdleOpacity
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
                                root.config.panelBox = modelData.box;
                            }

                            contentItem: Column {
                                spacing: 2

                                // The panel over a token of the dark scene, so
                                // "Transparent" shows no box. Drawn at reference
                                // size and scaled as a whole to fit the cell.
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

                                    // The active preset's frame.
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

        // First on the card: it decides the panel's shape and every preview
        // follows it. A ComboBox, not radios: one question with one answer.
        SettingRow {
            label: qsTr("Layout")
            description: qsTr("Which way the participants are listed.")

            ComboBox {
                objectName: "panelLayoutChoice"
                model: [qsTr("Vertical"), qsTr("Horizontal")]
                currentIndex: root.config.panelLayout
                onActivated: root.config.panelLayout = currentIndex
                Accessible.name: qsTr("Panel layout")
            }
        }

        // What the background goes behind: the panel's shape, so beside the
        // layout and above the surface's colour and opacity.
        SettingRow {
            label: qsTr("Box")
            description: qsTr("What the panel's background is drawn behind.")

            ComboBox {
                objectName: "panelBoxChoice"
                model: [qsTr("Whole panel"), qsTr("Names only")]
                currentIndex: root.config.panelBox
                onActivated: root.config.panelBox = currentIndex
                Accessible.name: qsTr("Panel box")
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

        // Opacity, not transparency: the figure rises as the box gets more
        // solid. The description says when the background is faint, because a
        // zero opacity looks exactly like a broken overlay.
        SettingRow {
            label: qsTr("Opacity")
            // "Little or no": the sentence appears below Config::kFaintBackground
            // (15%), not only at zero, and the panel still draws there.
            description: root.config.backgroundFaint
                         ? qsTr("Little or no background: names and avatars over the game.")
                         : root.config.panelBox === 1
                           ? qsTr("How solid the box behind each name is.")
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

        // The swatch shows what the overlay draws (the ramp's colour while the
        // setting is auto). Reset is its own action: picking the shown colour
        // would pin it, and a pinned colour does not follow the box.
        SettingRow {
            label: qsTr("Name colour")
            description: qsTr("The colour of a name while its owner is silent. Auto follows the box.")

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

        // A number, not a slider: the same units as the Spacing page's padding.
        SettingRow {
            label: qsTr("Text size")
            description: qsTr("The height of one line of text. Pictures and rows grow with it.")

            SpinRow {
                accessibleName: qsTr("Text size")
                from: 8; to: 48
                value: root.config.fontSize
                defaultValue: root.config.defaultFontSize
                onMoved: function(chosen) { root.config.fontSize = chosen; }
            }
        }

        // The fonts this machine has that the overlay can rasterise, from
        // fontconfig filtered to TrueType, the carried Inter first. A ComboBox
        // rather than a font dialog: size and style are settings of their own.
        SettingRow {
            label: qsTr("Font")
            description: qsTr("The typeface the overlay draws in.")

            ComboBox {
                id: fontChoice

                objectName: "fontFamilyChoice"

                // The built-in font is the empty string, here as in the file.
                // The list holds only the set family until the menu opens or the
                // box takes focus: this style's ComboBox measures every row to
                // size itself (about a second for a few hundred families), and
                // every page is built at startup (entry 104). Read through
                // ConfigBridge::fontFamilies(), a function, so the list arrives
                // as one copy.
                property bool familiesReady: false
                function ensureFamilies() {
                    familiesReady = true;
                    // Replacing the model resets currentIndex to 0 after the
                    // binding ran, so it is set again here -- as a binding,
                    // because a plain assignment would remove it (entry 225).
                    currentIndex = Qt.binding(function() { return fontSetIndex; });
                }

                readonly property var families:
                    familiesReady ? [""].concat(root.config.fontFamilies())
                                  : [root.config.fontFamily]

                // Built on focus too: arrow keys move through the families
                // without opening the menu.
                onActiveFocusChanged: if (activeFocus) ensureFamilies()

                // Each name is drawn in the face it names. The built-in entry
                // uses the carried face by name, not overlayFont(), which puts
                // the chosen family first; and it is found by value (""), not
                // index, since the list may be one entry long.
                function familyFor(index) {
                    const family = families[index];
                    return family === "" ? root.config.builtInFontFamily : family;
                }

                model: families.map(function(name) {
                    return name === "" ? qsTr("Built-in (Inter)") : name;
                })

                // The popup's width, measured from the names in the window's
                // font. Not WidestText (it sizes the box, not the popup, and is
                // costly on large models), not contentItem.children (a recycling
                // ListView holds only the visible rows), and not each family's
                // own face (that loads every font); rows elide what does not fit.
                FontMetrics { id: nameMetrics }

                readonly property real widestName: {
                    let widest = 0;
                    for (let i = 0; i < model.length; ++i) {
                        widest = Math.max(widest, nameMetrics.advanceWidth(model[i]));
                    }
                    return widest;
                }
                // From the setting, not from the current row: neither the model
                // reset nor a refused family can make it lie.
                displayText: root.config.fontFamily === "" ? qsTr("Built-in (Inter)")
                                                           : root.config.fontFamily
                // Published for the geometry dump, so a test can see the two
                // part (tests/font_picker_follows.cmake).
                readonly property int fontSetIndex:
                    Math.max(0, families.indexOf(root.config.fontFamily))
                readonly property int fontIndex: currentIndex
                currentIndex: fontSetIndex
                onActivated: root.config.fontFamily = families[currentIndex]
                Accessible.name: qsTr("Overlay font")
                font.family: familyFor(currentIndex)
                // The box sits beside a label, not across the page.
                implicitWidth: 220

                delegate: ItemDelegate {
                    objectName: "fontRow"

                    required property var modelData
                    required property int index

                    // The scrollbar's room comes out of the row's width.
                    width: ListView.view ? ListView.view.width - fontList.barRoom
                                         : implicitWidth
                    text: modelData
                    // A name in its own face can be wider than the list, which
                    // is sized from the window's font: it elides.
                    contentItem: Label {
                        // Named for the geometry dump: its implicitWidth says
                        // which face drew it (tests/appearance_previews.cmake).
                        objectName: "fontRowName"
                        text: parent.text
                        font: parent.font
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    font.family: fontChoice.familyFor(index)
                    // The window's size: this shows the typeface, not the
                    // overlay's size.
                    font.pointSize: Qt.application.font.pointSize
                    highlighted: fontChoice.highlightedIndex === index
                }

                // Ten rows tall, as wide as the names need, opening on the set
                // family, with a draggable scrollbar beside the text. Delegates
                // are recycled so a row's font is not reloaded as it scrolls.
                popup: Popup {
                    id: fontPopup

                    // Here, not onOpened: the list must be complete before the
                    // ListView instantiates its rows.
                    onAboutToShow: fontChoice.ensureFamilies()

                    // At least the box's width, at most half the window's. The
                    // paddings are the box's own; the bar's room is added
                    // because the rows give it up.
                    readonly property real wanted:
                        fontChoice.widestName + fontChoice.leftPadding +
                        fontChoice.rightPadding + fontList.barRoom
                    readonly property real ceiling:
                        fontChoice.Window.width > 0 ? fontChoice.Window.width * 0.5 : 480

                    y: fontChoice.height
                    width: Math.min(Math.max(fontChoice.width, wanted), ceiling)
                    implicitHeight: Math.min(fontList.contentHeight + 2,
                                             fontChoice.height * 10)
                    padding: 1

                    // Positioned on opened: before that the popup is not
                    // visible, so the list's model (bound to visibility) is null.
                    onOpened: fontList.positionViewAtIndex(fontChoice.currentIndex,
                                                           ListView.Center)

                    contentItem: ListView {
                        id: fontList

                        // The name the geometry dump reads the menu by
                        // (VOCEM_CONFIG_OPEN in gui/src/main.cpp).
                        objectName: "fontList"

                        readonly property real barRoom:
                            fontBar.visible ? fontBar.width : 0

                        clip: true
                        model: fontChoice.popup.visible ? fontChoice.delegateModel : null
                        currentIndex: fontChoice.highlightedIndex
                        reuseItems: true
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: ScrollBar {
                            id: fontBar
                            policy: ScrollBar.AlwaysOn
                        }
                    }
                }
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

        // Whoever is not talking shows quieter. From 10%, config.h's bound:
        // at zero the picture would not be drawn.
        SettingRow {
            label: qsTr("Quiet avatars")
            description: qsTr("Opacity of the avatar of anyone not talking, whose name is greyed as well.")

            SliderRow {
                accessibleName: qsTr("Avatar opacity when not talking")
                from: 10; to: 100; stepSize: 5
                decimals: 0
                suffix: "%"
                value: Math.round(root.config.avatarIdleOpacity * 100)
                defaultValue: Math.round(root.config.defaultAvatarIdleOpacity * 100)
                onMoved: function(chosen) { root.config.avatarIdleOpacity = chosen / 100; }
            }
        }

        SettingRow {
            // theme_for() reads text_shadow into both the panel's and the
            // message's outline, so the sentence names both.
            label: qsTr("Text outline")
            description: qsTr("An edge around the overlay's text, in the panel and in messages, for wherever a box does not cover it.")

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
        // Nothing here shows with messages off, so the card is greyed.
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
            description: qsTr("The colour of the message itself. Auto follows the box.")

            ColourButton {
                colour: root.config.effectiveNotificationTextColour
                changed: root.config.notificationTextColour !== "auto"
                title: qsTr("Message text colour")
                onPicked: function(chosen) { root.config.notificationTextColour = String(chosen); }
                resetAction: function() { root.config.notificationTextColour = "auto"; }
            }
        }

        // Down to zero, like the panel's; the description says when the
        // background is faint.
        SettingRow {
            label: qsTr("Opacity")
            description: root.config.notificationBackgroundFaint
                         ? qsTr("Little or no background: the message over the game.")
                         : qsTr("How solid the message background is.")

            SliderRow {
                accessibleName: qsTr("Message opacity")
                from: 0; to: 100; stepSize: 2
                decimals: 0
                suffix: "%"
                value: Math.round(root.config.notificationOpacity * 100)
                defaultValue: Math.round(root.config.defaultNotificationOpacity * 100)
                onMoved: function(chosen) { root.config.notificationOpacity = chosen / 100; }
            }
        }
    }
}
