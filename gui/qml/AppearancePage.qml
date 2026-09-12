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

    settings: ["panelLayout", "panelBox", "panelColour", "opacity", "speakingColour",
               "textIdleColour",
               "textSpeakingColour", "fontFamily", "fontSize", "avatarSize",
               "avatarIdleOpacity", "textShadow",
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
                // Named for the shape of the box it draws, not for the client
                // whose overlay wears that shape -- the reason "Purple" is not
                // named after anybody either.
                case "pills": return qsTr("Pills");
                case "dark": return qsTr("Dark");
                case "light": return qsTr("Light");
                case "purple": return qsTr("Purple");
                case "transparent": return qsTr("Transparent");
                }
                return id;
            }
            // All three of a preset's writes, because two of them are no longer
            // enough to tell one chip from another: Pills and Dark are the same
            // surface at the same opacity, drawn in two different places.
            function isActive(preset) {
                return Qt.colorEqual(root.config.panelColour, preset.colour) &&
                       Math.abs(root.config.opacity - preset.opacity) < 0.005 &&
                       root.config.panelBox === preset.box;
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
                            // And where the preset puts that surface, so the
                            // regression test can tell two chips apart that
                            // share the other two numbers.
                            readonly property real presetBox: modelData.box

                            // PanelPreview reads one config object. This stands in
                            // for the bridge with the preset's two writes applied
                            // -- the same two onClicked makes -- and the theme
                            // those writes produce; everything else passes
                            // through, so the previews follow the other controls
                            // live.
                            readonly property QtObject presetConfig: QtObject {
                                // Everything but the preset's own two writes
                                // passes through, the layout included: a chip
                                // that drew a column while the panel is a row
                                // would be showing a surface on a shape the
                                // user does not have.
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

        // Which way the people run. First on the card, above the colours,
        // because it decides the panel's shape and every preview on this page
        // follows it -- and a ComboBox rather than two radio buttons: the KDE
        // guidelines keep radios for a set worth seeing at once, and these two
        // are one question with one answer, shown by the previews beside it.
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

        // Where that background is drawn. Under the layout, because the two are
        // one question about the panel's shape asked twice -- which way the
        // people run, and what the surface goes behind -- and above the colour
        // and the opacity, which are about the surface itself whichever shape it
        // is on. A ComboBox for the layout's reason: one question with one
        // answer, shown by the previews beside it.
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

        // Opacity, not transparency: the figure rises as the box
        // becomes more solid, so the other word said the opposite of
        // what the number did.
        // The description carries the warning rather than a third line
        // under the row: an opacity that reached zero by accident looks
        // exactly like a broken overlay, so the row has to say which of
        // the two it is.
        SettingRow {
            label: qsTr("Opacity")
            // "Little or no", because the sentence appears below 15% and not at
            // zero (Config::kFaintBackground): at 8% the panel still draws a
            // background, and a row that answered "No background" there was
            // telling the user the opposite of what the overlay does -- in the
            // one sentence that exists to tell an empty box from a broken one.
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

        // The two text colours a user may pin. The swatch shows what the overlay
        // actually draws -- the ramp's colour while the setting is auto -- so the
        // reset does not go blank, it reveals what auto stands for. The reset is
        // its own action rather than picking the shown colour back: picking the
        // ramp's current value would pin it, and a pinned colour stays put when
        // the box changes where auto follows it.
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

        // A number rather than a slider: it is a size in the same units as the
        // padding on the Spacing page, and it is read off the box as often as it
        // is dragged.
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

        // The typeface. The list is what this machine has that the overlay can
        // rasterise, with the carried Inter first: what the window offers is
        // what the game can draw, which is why the list comes from fontconfig
        // filtered to TrueType rather than from Qt's font database.
        //
        // A ComboBox with a long model rather than a font dialog: KDE's own
        // font pickers are a dialog because they choose a size and a style as
        // well, and both of those are settings of their own here.
        SettingRow {
            label: qsTr("Font")
            description: qsTr("The typeface the overlay draws in.")

            ComboBox {
                id: fontChoice

                objectName: "fontFamilyChoice"

                // The families this box offers, and when it goes and gets them.
                // The built-in font is the empty string in the settings file, in
                // this list as in the file.
                //
                // Until somebody actually goes for the font, the box carries one
                // entry: the family that is set. Handing this style's ComboBox
                // the machine's whole font list is what costs -- it walks every
                // item to size itself (qqc2-desktop-style's ComboBox.qml,
                // `onCountChanged`, one `boundingRect` per row) -- and measured
                // here with 271 families installed it is 1.05 s, paid at
                // *startup*, because every page of this window is built when the
                // window is. Everybody paid it, including everybody who never
                // opens this page. Now the menu asks on its way open and the
                // keyboard asks when the box takes focus, so the second is spent
                // by the person who wants a font, once, and by nobody else.
                //
                // Read through a function call, and kept: the list comes over as
                // a copy that way. As a property it arrives as a sequence that
                // re-reads itself on every indexed access, and `concat` walks it
                // -- see ConfigBridge::fontFamilies().
                property bool familiesReady: false
                function ensureFamilies() {
                    familiesReady = true;
                    // A ComboBox puts its current index back to 0 whenever its
                    // model is replaced, so the index is set again here, after
                    // the list has grown. A binding cannot do it: it runs on the
                    // count change and the reset comes after it, and then
                    // nothing re-evaluates. Measured before this line: with
                    // "DejaVu Sans" set, the menu opened at the top of the list
                    // with "Built-in (Inter)" highlighted.
                    currentIndex = Math.max(0, families.indexOf(root.config.fontFamily));
                }

                readonly property var families:
                    familiesReady ? [""].concat(root.config.fontFamilies())
                                  : [root.config.fontFamily]

                // Whichever road is taken to the list, it is built before it is
                // needed: the menu's, and the keyboard's -- arrow keys on a
                // focused box move through the families without opening
                // anything, and a box holding one row would have nowhere to
                // move.
                onActiveFocusChanged: if (activeFocus) ensureFamilies()

                // Each name written in the font it names, in the list and in the
                // box: a list of forty family names all set in the desktop's own
                // font has to be applied one at a time before any of them can be
                // seen. The built-in entry is the exception -- Inter is what it
                // says, and the window carries it, so it draws itself too.
                //
                // The carried face by name, not through overlayFont(): that
                // function puts the *chosen* family at the head of its list, and
                // QFont.family is the head of the list, so once a font was
                // picked the row labelled "Built-in (Inter)" was drawn in the
                // very face the user would be leaving -- a label saying one
                // thing over letters saying another.
                // By the value in the list and not by the index, because the
                // list is one entry long until it is asked for: the built-in
                // entry is the empty family wherever it sits.
                function familyFor(index) {
                    const family = families[index];
                    return family === "" ? root.config.builtInFontFamily : family;
                }

                model: families.map(function(name) {
                    return name === "" ? qsTr("Built-in (Inter)") : name;
                })

                // How wide the list has to be, measured from the names rather
                // than picked. Three ways to arrive at this were considered:
                //
                //   * Qt's own `implicitContentWidthPolicy: WidestText`, which
                //     sizes the *box* and not the popup, is documented as
                //     expensive on large models, and needs a TextInput content
                //     item -- none of which fits a list of every family on the
                //     machine;
                //   * the recipe that circulates for popups, which loops over
                //     `contentItem.children` -- in a recycling ListView those
                //     are only the rows currently on screen, so the width would
                //     change as the list scrolls;
                //   * measuring the strings, which is what this does: one pass
                //     of advanceWidth over the names with the *window's* font.
                //
                // The window's font and not each family's own, deliberately: a
                // row is drawn in the face it names, but measuring two hundred
                // families in their own faces means loading two hundred fonts to
                // open a menu. Font pickers elsewhere settle this the same way --
                // preview the name in its face, size the list once, elide what
                // does not fit -- and the rows elide.
                FontMetrics { id: nameMetrics }

                readonly property real widestName: {
                    let widest = 0;
                    for (let i = 0; i < model.length; ++i) {
                        widest = Math.max(widest, nameMetrics.advanceWidth(model[i]));
                    }
                    return widest;
                }
                // What the box says, from the setting itself rather than from
                // whichever row the list thinks is current. Two things stop
                // being able to make it lie: the ComboBox's own reset of its
                // index when the model changes, and a family this machine
                // cannot resolve -- setFontFamily() refuses that one and the box
                // has already moved, and it used to be put back by a binding
                // that only re-ran because the refusal emitted configChanged.
                // The name shown is the name in the settings, always.
                displayText: root.config.fontFamily === "" ? qsTr("Built-in (Inter)")
                                                           : root.config.fontFamily
                currentIndex: Math.max(0, families.indexOf(root.config.fontFamily))
                onActivated: root.config.fontFamily = families[currentIndex]
                Accessible.name: qsTr("Overlay font")
                font.family: familyFor(currentIndex)
                // A machine's font list is long, and the box is beside a label
                // rather than across the page.
                implicitWidth: 220

                delegate: ItemDelegate {
                    objectName: "fontRow"

                    required property var modelData
                    required property int index

                    // The bar's room comes out of the row, which is the only
                    // place a ListView has to give it from: without this the
                    // longest family names ran under the scrollbar.
                    width: ListView.view ? ListView.view.width - fontList.barRoom
                                         : implicitWidth
                    text: modelData
                    // A name drawn in its own face can be wider than the same
                    // name measured in the window's, and the list is sized from
                    // the second: what does not fit ends in an ellipsis rather
                    // than under the scrollbar.
                    contentItem: Label {
                        // Named for the geometry dump: a row's own implicitWidth
                        // is the one number that says which face it was drawn in
                        // (the row's width is the list's). That is how the
                        // built-in row is held to the built-in font -- see
                        // tests/appearance_previews.cmake.
                        objectName: "fontRowName"
                        text: parent.text
                        font: parent.font
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    font.family: fontChoice.familyFor(index)
                    // The size stays the window's: this shows which typeface a
                    // name belongs to, not how large the overlay will draw it.
                    font.pointSize: Qt.application.font.pointSize
                    highlighted: fontChoice.highlightedIndex === index
                }

                // A list of every family on the machine is a long list -- two
                // hundred rows here -- and the default popup grows with its
                // contents, opens at the top of them whatever is chosen, and
                // scrolls with a thin indicator that cannot be dragged. Written
                // out: ten rows tall, wide enough for a family name, opening on
                // the one that is set, with a real scrollbar that has room of
                // its own beside the text. The delegates are recycled, which is
                // what keeps each row's font from being loaded again every time
                // it scrolls past.
                popup: Popup {
                    id: fontPopup

                    // The list is built here rather than at `onOpened`: at
                    // aboutToShow the popup is not visible yet, so the ListView's
                    // model is still null and the rows are instantiated once,
                    // after this, with the full list already in place.
                    onAboutToShow: fontChoice.ensureFamilies()

                    // Never narrower than the box it hangs from, never wider
                    // than half the window: the longest name on a machine is
                    // nobody's business but that machine's, and a menu that
                    // takes half the page to show one of them is worse than an
                    // elided row. The paddings are the box's own -- same style,
                    // same metrics -- and the bar's room is added because the
                    // rows give it their own width.
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

                    // Opened, not aboutToShow: at aboutToShow the popup is not
                    // visible yet, so the list's model -- which is bound to that
                    // visibility so two hundred rows are not instantiated while
                    // the menu is shut -- is still null and there is nothing to
                    // position. Measured: a Popup whose onAboutToShow asks for
                    // its own `visible` gets false, and its ListView's count is
                    // zero; at onOpened both are what they should be.
                    onOpened: fontList.positionViewAtIndex(fontChoice.currentIndex,
                                                           ListView.Center)

                    contentItem: ListView {
                        id: fontList

                        // A Popup is not an item, so the list inside it is what
                        // the geometry dump can see of it: this name is how the
                        // harness reads where the menu opened and how large it
                        // is (VOCEM_CONFIG_OPEN in gui/src/main.cpp).
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

        // The picture's half of what the idle grey does to a name: whoever is
        // not talking shows quieter. From 10%, the bound config.h gives it --
        // at zero the picture would not be drawn at all.
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
            // One switch, both boxes: theme_for() reads text_shadow into
            // panel_text_outline *and* toast_text_outline, so the sentence has
            // to name the message as well. It used to say "the names", which was
            // the panel's half of what the setting does -- and the half that
            // matters least, now that the message background goes down to
            // nothing and its words can end up on the bare game.
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
            description: qsTr("The colour of the message itself. Auto follows the box.")

            ColourButton {
                colour: root.config.effectiveNotificationTextColour
                changed: root.config.notificationTextColour !== "auto"
                title: qsTr("Message text colour")
                onPicked: function(chosen) { root.config.notificationTextColour = String(chosen); }
                resetAction: function() { root.config.notificationTextColour = "auto"; }
            }
        }

        // Down to nothing, like the panel's. The slider used to stop at 20%,
        // which made the message box the one surface the overlay would not let
        // you switch off; and as with the panel, the description says which of
        // the two a background of zero is, because an empty box and a broken
        // one look the same.
        SettingRow {
            label: qsTr("Opacity")
            // The same threshold and the same honesty as the panel's above.
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
