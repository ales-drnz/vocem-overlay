// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which display the map below depicts.
//
// The overlay sizes itself from the largest connected display, so "a twentieth
// of the display" is a different claim on each screen. This row says which
// display a map stands for; OverlayStage draws the boxes at that screen's share.
// The empty selection means automatic: the largest display. The list is the
// bridge's display enumeration (environment.h), re-read on the window's sweep.

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

    // Always shown, even with one display: besides offering a choice the row
    // says which display the picture stands for and at what resolution.
    spacing: Theme.smallSpacing

    function optionsFrom(found) {
        const list = [{ label: qsTr("Automatic (largest)"), value: "" }];
        for (let i = 0; i < found.length; ++i)
            list.push({ label: qsTr("%1 (%2 × %3)").arg(found[i].name)
                                  .arg(found[i].width).arg(found[i].height),
                        value: found[i].name });
        return list;
    }

    // Two option lists that say the same thing. Every value and every label,
    // not the count: a display unplugged in the same sweep as another is
    // plugged in leaves the count alone, and a display switched to a different
    // mode changes only its label, which is where its resolution is written.
    function sameOptions(one, other) {
        if (one.length !== other.length)
            return false;
        for (let i = 0; i < one.length; ++i)
            if (one[i].value !== other[i].value || one[i].label !== other[i].label)
                return false;
        return true;
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
        objectName: "displayPicker"

        // Assigned in Component.onCompleted and by the handler below, never
        // bound: a binding over config.displays would replace the model on the
        // same signal before the guard ran, and a ComboBox whose model is
        // replaced goes back to row 0 (entry 171).
        property var options: []

        // For the geometry dump: the pin's whole visible state is an index.
        // display_change_while_open.cmake reads these two.
        readonly property int pickerIndex: currentIndex
        readonly property int pickerCount: options.length

        textRole: "label"
        valueRole: "value"
        model: options
        Accessible.name: root.accessibleLabel

        Component.onCompleted: {
            options = root.optionsFrom(root.config.displays);
            currentIndex = root.indexFor(root.selection);
        }
        onActivated: root.picked(currentValue)
    }

    Connections {
        target: root.config
        // The bridge signals only when the enumeration moved.
        function onDisplaysChanged() {
            // Replace the model only when the options differ, and restore the
            // index after: a replaced model puts currentIndex back to 0.
            const fresh = root.optionsFrom(root.config.displays);
            if (!root.sameOptions(fresh, picker.options)) {
                picker.options = fresh;
                picker.currentIndex = root.indexFor(root.selection);
            }
        }
    }
}
