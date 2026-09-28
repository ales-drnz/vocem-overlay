// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The few values the window's own containers need: the spacing rhythm and the
// colours of cards and sidebar, derived from the platform so the window follows a
// light or dark theme. Controls drawn by the org.kde.desktop style are left alone,
// so they look like the rest of the session.

pragma Singleton

import QtQuick
import QtQuick.Controls

QtObject {
    id: theme

    readonly property Control probe: Control { visible: false }
    readonly property var palette: probe.palette

    // The spacing scale, and the only one: every margin in the window is one of
    // these. Derived from the default font's height, as Kirigami's gridUnit is,
    // so a larger desktop font enlarges the spaces too (at Noto Sans 10 the grid
    // is 18 and the steps are 6/12/18/24).
    readonly property TextMetrics gridRuler: TextMetrics { text: "M" }
    readonly property int gridUnit: Math.ceil(gridRuler.height)
    // A third, two thirds, one grid, and a third beyond it: this window's own
    // ratios rather than Kirigami's quarters.
    readonly property int smallSpacing: Math.max(2, Math.round(gridUnit / 3))
    readonly property int mediumSpacing: smallSpacing * 2
    readonly property int largeSpacing: smallSpacing * 3
    readonly property int pageMargin: smallSpacing * 4
    // Breeze's own corner (Frame_FrameRadius in breezemetrics.h). The overlay's
    // boxes keep their own radius: they are read over a game, not this desktop.
    readonly property int cornerRadius: 5

    // Inside a card: more than the rows' gap, less than a group gap. The group
    // heading uses it too, so a title and its labels share one left edge.
    readonly property int cardPadding: Math.round(smallSpacing * 2.5)

    // The control block on the right of every settings row: one width, so a
    // slider is the same length on every page and a row keeps its shape.
    readonly property int controlWidth: 300

    // Whether this is a dark theme, from the platform theme's colorScheme.
    // SystemPalette is the QGuiApplication's palette, not Breeze, and a Label's
    // `color` reads black while the org.kde.desktop style paints it white, so
    // neither can be asked. Unknown falls back to the palette, which is right on
    // the plain styles where the palette draws everything.
    readonly property bool dark:
        Application.styleHints.colorScheme === Qt.ColorScheme.Dark ||
        (Application.styleHints.colorScheme === Qt.ColorScheme.Unknown &&
         palette.window.hslLightness < 0.5)

    // Only ever used on top of the veils below, so it follows the same source.
    readonly property color textColour: dark ? "#ffffff" : "#000000"

    // Every surface is a veil over the window rather than an opaque colour, so
    // the window never needs its own background (which it cannot reliably ask
    // for) and a card is right on any theme: slightly lighter in the dark,
    // near-white on the light.
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

    // The daemon's state: Breeze's Positive, Neutral and Negative, the same in
    // BreezeDark and BreezeLight. The overlay keeps Discord's palette because it
    // stands in for Discord; this window is a Plasma application. Never colour
    // alone: the status dot also changes shape (see Main.qml).
    readonly property color online: "#27ae60"
    readonly property color busy: "#f67400"
    readonly property color offline: "#da4453"

    // Qt takes whole pixels or fractional points for a font size, and the
    // previews need a fraction (the overlay's 16-unit text is about 13.2 of Qt's).
    // The factor is the width ratio of one string at 100 points and 100 pixels,
    // not the screen's DPI, which a fractionally scaled Wayland session misreports.
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

    // The previews read the overlay's palette out of a map, where a misspelt name
    // is `undefined` and paints as transparent. Each preview names the tokens it
    // reads and a missing one is logged: not fatal, since the preview is still
    // worth showing, but loud, since nothing else would say so.
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
