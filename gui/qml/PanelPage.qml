// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The voice panel, and where it sits on the display.
//
// One section of the window. Each is its own file: they have nothing to say to
// each other, and one file of eight hundred lines said all of it at once.

import QtQuick
import QtQuick.Layouts
import Vocem

SectionPage {
    id: root

    // panelPreviewDisplay included: it is persisted like any other setting and
    // it was the one this page edited that Reset could not restore and
    // atDefaults could not see -- its default* invokable sat unused, which was
    // the evidence the convention was meant to cover it.
    settings: ["scale", "screenMargin", "positionX", "positionY", "panelPreviewDisplay"]
    title: qsTr("Panel")
    subtitle: qsTr("Drag the panel to position it, or click an anchor point. A drag released near an anchor snaps to it. A running game follows when you apply.")

    ColumnLayout {
        anchors.fill: parent
        // The dropdown belongs to the map under it -- it says which display the
        // picture is of -- so they sit a small step apart, which is what the KDE
        // guidelines give related items. The card below is a different group and
        // takes a large step (the card's own margin on top of this one), the
        // same step the pages made of cards keep between theirs. Both gaps used
        // to be the middle step, which said the picker and the card were equally
        // far from the picture. The two together still come to what they came to
        // before, so the map is exactly as tall as it was and the geometry
        // comparison measures the same picture.
        spacing: Theme.smallSpacing

        // Which display the map below depicts. Only offered with several
        // connected; the overlay is sized for the largest, and the map says so
        // when a smaller one is chosen.
        DisplayPicker {
            id: panelDisplayPicker
            objectName: "panelDisplayPicker"

            config: root.config
            selection: root.config.panelPreviewDisplay
            accessibleLabel: qsTr("Display the panel map depicts")
            onPicked: function(name) { root.config.panelPreviewDisplay = name; }
        }

        ScreenMap {
            config: root.config
            shownDisplay: panelDisplayPicker.shownDisplay
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // The two settings the map answers for: how large the panel is
        // against the display, and how close to its edge it may go.
        // Both were elsewhere, and neither could be judged there.
        Card {
            Layout.fillWidth: true
            Layout.topMargin: Theme.mediumSpacing

            SettingRow {
                label: qsTr("Panel size")
                description: qsTr("Text, avatars and spacing together.")
                first: true

                SliderRow {
                    accessibleName: qsTr("Panel size")
                    from: 0.5; to: 3.0; stepSize: 0.05
                    suffix: "×"
                    value: root.config.scale
                    defaultValue: root.config.defaultScale
                    onMoved: function(chosen) { root.config.scale = chosen; }
                }
            }

            SettingRow {
                label: qsTr("Distance from the edge")
                description: qsTr("How close to the edge of the display the panel may sit.")

                SliderRow {
                    accessibleName: qsTr("Distance from the edge")
                    from: 0; to: 120; stepSize: 1
                    decimals: 0
                    value: root.config.screenMargin
                    defaultValue: root.config.defaultScreenMargin
                    onMoved: function(chosen) { root.config.screenMargin = chosen; }
                }
            }
        }
    }
}
