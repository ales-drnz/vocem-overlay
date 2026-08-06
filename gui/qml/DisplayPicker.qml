// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which display the map below depicts.
//
// The overlay sizes itself from the largest connected display, and a map is a
// map of one screen: with several plugged in, "the panel covers a twentieth of
// the display" is a different claim on each of them. This row lets each map say
// which display it is talking about; OverlayStage then draws the boxes at the
// share of that screen they will really cover, and says so when it differs.
//
// Hidden outright on a single-display machine -- a dropdown with one honest
// answer is furniture -- and the empty selection means automatic: the largest
// display, which is the one the overlay is sized for and the one the maps have
// always shown.
//
// The list comes from the bridge's one display enumeration (environment.h),
// which is cached after the first non-empty read; the rebuild below only fires
// for the display that was asleep when the window opened.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vocem

RowLayout {
    id: root

    required property var config
    // The persisted connector name this row edits ("" = automatic/largest).
    required property string selection
    // What the ComboBox announces to assistive technology; the pages name the
    // map the row belongs to.
    required property string accessibleLabel

    signal picked(string name)

    // The chosen display's {name, width, height}, or null while automatic --
    // and null again if the chosen connector is gone, so an unplugged display
    // degrades to the map's normal picture rather than to a stale shape.
    readonly property var shownDisplay: {
        if (selection === "")
            return null;
        const found = config.displays;
        for (let i = 0; i < found.length; ++i)
            if (found[i].name === selection)
                return found[i];
        return null;
    }

    // Always shown, the owner's ask, including on a machine with one display.
    // It was hidden below two on the reasoning that a control with one real
    // choice is furniture -- and that reasoning ignores what the row says
    // *besides* offering a choice: which display the picture stands for, at
    // what resolution, and the fact that the overlay sizes itself for the
    // largest one. On a single-display machine that is a caption confirming
    // the map is about the one screen there is, and it is the first place
    // somebody plugging a second monitor in will look.
    spacing: Theme.smallSpacing

    function optionsFrom(found) {
        const list = [{ label: qsTr("Automatic (largest)"), value: "" }];
        for (let i = 0; i < found.length; ++i)
            list.push({ label: qsTr("%1 (%2 × %3)").arg(found[i].name)
                                  .arg(found[i].width).arg(found[i].height),
                        value: found[i].name });
        return list;
    }

    function indexFor(name) {
        for (let i = 0; i < picker.options.length; ++i)
            if (picker.options[i].value === name)
                return i;
        return 0;
    }

    onSelectionChanged: picker.currentIndex = root.indexFor(root.selection)

    Label {
        text: qsTr("Map shows")
        opacity: 0.7
    }

    ComboBox {
        id: picker

        // Held in a plain property and only replaced when the enumeration
        // itself changed: a model bound straight to config.displays would be
        // reassigned on every state tick.
        property var options: root.optionsFrom(root.config.displays)

        textRole: "label"
        valueRole: "value"
        model: options
        Accessible.name: root.accessibleLabel

        Component.onCompleted: currentIndex = root.indexFor(root.selection)
        onActivated: root.picked(currentValue)
    }

    Connections {
        target: root.config
        function onStateChanged() {
            if (root.config.displays.length + 1 !== picker.options.length) {
                picker.options = root.optionsFrom(root.config.displays);
                picker.currentIndex = root.indexFor(root.selection);
            }
        }
    }
}
