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
// Always shown, one display or several: the header used to promise "hidden
// outright on a single-display machine" while the body kept it visible, and
// the body is right (its own comment below says why the old reasoning was
// overturned) -- a file whose header contradicts its body is read by its
// header. The empty selection means automatic: the largest display, which is
// the one the overlay is sized for and the one the maps have always shown.
//
// The list comes from the bridge's one display enumeration (environment.h),
// which is re-read on the window's four-second sweep, so the rebuild below fires
// for a display plugged in, unplugged or switched to another mode while the
// window is open -- as well as for the one that was asleep when it opened.

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
        // The bridge announces the enumeration on a signal of its own, and only
        // when it moved; this used to listen on stateChanged, twice a second.
        function onDisplaysChanged() {
            // Only when the enumeration itself changed. A ComboBox puts
            // currentIndex back to 0 when its model is replaced (entry 104), so
            // reassigning on every tick would drag the selection to "Automatic"
            // under the hand that set it.
            const fresh = root.optionsFrom(root.config.displays);
            if (!root.sameOptions(fresh, picker.options)) {
                picker.options = fresh;
                picker.currentIndex = root.indexFor(root.selection);
            }
        }
    }
}
