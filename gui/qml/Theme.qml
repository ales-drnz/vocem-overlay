// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The few values the window's own containers need. Everything a Qt Quick control
// draws by itself is left alone -- the point of using the org.kde.desktop style is
// that buttons, sliders and switches look like the rest of the session, and
// restyling them would undo that. What is defined here is only what the style has
// no opinion about: the spacing rhythm, and the colour of the cards and the
// sidebar, both derived from the system palette rather than fixed, so the window
// follows a light or dark theme without a second set of values.
//
// The spacing scale is the one the KDE HIG describes: a small step for things that
// belong together, a large step between groups.

pragma Singleton

import QtQuick
import QtQuick.Controls

QtObject {
    id: theme

    readonly property Control probe: Control { visible: false }
    readonly property var palette: probe.palette

    // The spacing scale, and the only one. Every margin in the window is one of
    // these; a number written inline anywhere else is a bug waiting to be noticed
    // as "the padding is off".
    //
    // Derived from the font, not fixed: these used to be 6/12/18/24 with a
    // comment declaring them "Kirigami's rhythm", but Kirigami defines gridUnit
    // as the *height of the font metrics* and derives the steps from it -- a
    // constant only follows the desktop while the desktop keeps the font the
    // constant was tuned on. A Plasma user makes the interface larger by making
    // the font larger, and fixed pixels gave them bigger text inside unchanged
    // spaces. The ruler below measures the default application font, so at Noto
    // Sans 10 the grid is 18 and every step lands exactly on the old values;
    // at any other font size the whole rhythm follows.
    readonly property TextMetrics gridRuler: TextMetrics { text: "M" }
    readonly property int gridUnit: Math.ceil(gridRuler.height)
    // A third, two thirds, one grid, and a third beyond it: the tuned ratios of
    // this window, kept -- Kirigami's own quarters would have shrunk every gap
    // by a step nobody asked for.
    readonly property int smallSpacing: Math.max(2, Math.round(gridUnit / 3))
    readonly property int mediumSpacing: smallSpacing * 2
    readonly property int largeSpacing: smallSpacing * 3
    readonly property int pageMargin: smallSpacing * 4
    // Breeze's own corner: Frame_FrameRadius in breezemetrics.h is 5, and a
    // window that wants to sit in a Plasma session wears the session's corners.
    // The overlay's boxes keep their own radius -- they are read over a game,
    // not over this desktop.
    readonly property int cornerRadius: 5

    // Inside a card. Two and a half steps: more than the rows' gap, so the card
    // reads as a container rather than as a stack of lines, and less than a
    // group gap. Used by the group heading too, so a title and the labels under
    // it share one left edge.
    readonly property int cardPadding: Math.round(smallSpacing * 2.5)

    // The width of the control block on the right of a settings row. One number
    // for all of them, so a slider is the same length on every page and a row does
    // not change shape when the value beside it gains a digit.
    readonly property int controlWidth: 300

    // Whether this is a dark theme, from the platform rather than inferred.
    //
    // Two more obvious sources were tried on a dark Plasma session and both lie.
    // SystemPalette reports the application's QPalette, which for a QGuiApplication
    // is not Breeze at all. A plain Label's `color` reports black -- measured --
    // even while that same label paints itself white, because the org.kde.desktop
    // style draws from the desktop's colour scheme without ever assigning the
    // property. Believing either one produced a window of near-white cards with
    // white text on them.
    //
    // styleHints.colorScheme comes from the platform theme and answered Dark
    // correctly on that session. Unknown falls back to the palette, which is at
    // least right on the plain styles where the palette is what draws everything.
    readonly property bool dark:
        Application.styleHints.colorScheme === Qt.ColorScheme.Dark ||
        (Application.styleHints.colorScheme === Qt.ColorScheme.Unknown &&
         palette.window.hslLightness < 0.5)

    // Only ever used on top of the veils below, so it follows the same source.
    readonly property color textColour: dark ? "#ffffff" : "#000000"

    // Every surface is a veil over whatever the window already is, rather than an
    // opaque colour of its own. That way the window never has to know its own
    // background -- which, per the note above, it cannot reliably ask for -- and a
    // card is correct on any theme by construction: slightly lighter than its
    // surroundings in the dark, near-white on the light, which is the shape both
    // the KDE and GNOME guidelines settle on for grouped settings.
    readonly property color cardColour: dark ? Qt.rgba(1, 1, 1, 0.07)
                                             : Qt.rgba(1, 1, 1, 0.75)
    readonly property color cardBorder: dark ? Qt.rgba(1, 1, 1, 0.10)
                                             : Qt.rgba(0, 0, 0, 0.10)
    readonly property color separator: dark ? Qt.rgba(1, 1, 1, 0.08)
                                            : Qt.rgba(0, 0, 0, 0.08)
    readonly property color sidebarColour: dark ? Qt.rgba(0, 0, 0, 0.16)
                                                : Qt.rgba(0, 0, 0, 0.05)
    // The status bar goes the other way from the sidebar, so the two never merge
    // into one band across the top-left corner.
    readonly property color headerColour: dark ? Qt.rgba(1, 1, 1, 0.04)
                                               : Qt.rgba(1, 1, 1, 0.45)

    // The state of the daemon, as three colours -- Breeze's Positive, Neutral
    // and Negative, which are the same values in BreezeDark.colors and
    // BreezeLight.colors, so one set serves both themes. They were Discord's
    // greens and reds before: this window is a Plasma application and states
    // are said in the desktop's own semantics; the overlay keeps Discord's
    // palette because it stands in for the Discord client, and this window does
    // not. Never the colour alone: the dot beside the status changes shape with
    // the state as well (see Main.qml), which is the HIG's requirement.
    readonly property color online: "#27ae60"
    readonly property color busy: "#f67400"
    readonly property color offline: "#da4453"

    // Qt takes a whole number of pixels for a font size and a fractional number of
    // points, and the previews need a fractional one: the overlay's text is 16
    // units, Inter has to be asked for about 13.2 of Qt's to come out that tall,
    // and 13 draws every box that ends where its text ends a little narrow. The
    // conversion is measured rather than taken from the screen's DPI, which is not
    // something to trust on a fractionally scaled Wayland session -- the same
    // string at 100 points and at 100 pixels, and the ratio of the two widths is
    // the factor.
    readonly property TextMetrics pointRuler: TextMetrics {
        font.pointSize: 100
        font.hintingPreference: Font.PreferNoHinting
        text: "Hamburgefonstiv"
    }
    readonly property TextMetrics pixelRuler: TextMetrics {
        font.pixelSize: 100
        font.hintingPreference: Font.PreferNoHinting
        text: "Hamburgefonstiv"
    }
    readonly property real pointsPerPixel:
        pointRuler.width > 0 ? pixelRuler.width / pointRuler.width : 0.75

    // The previews read the overlay's palette out of a map, which is what makes them
    // impossible to drift from the drawing -- and which costs a typed property's one
    // guarantee: a name spelled wrong is `undefined` rather than an error, and an
    // undefined colour paints as transparent. A missing ring or a missing name reads
    // as a design decision rather than as a mistake.
    //
    // So each preview says which tokens it reads, and is told at once if one of them
    // is not there. Not fatal, because a preview with one colour missing is still
    // worth showing next to the controls that change it; loud, because nothing else
    // in the window would say so.
    function requireTokens(tokens, owner, names) {
        for (const name of names) {
            if (tokens[name] === undefined) {
                console.error(owner + ": no overlay theme token named '" + name +
                              "'. Check ConfigBridge::overlayTheme() in config_bridge.cpp " +
                              "against include/vocem/theme.h.");
            }
        }
    }

    // The stand-in for a game behind the previews.
    readonly property Gradient gameBackdrop: Gradient {
        GradientStop { position: 0.0; color: "#41506b" }
        GradientStop { position: 0.55; color: "#25304a" }
        GradientStop { position: 1.0; color: "#12161f" }
    }
}
