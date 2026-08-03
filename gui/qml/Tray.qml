// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The tray icon: the way back to a window that was put away, and the place the
// application's one real Quit lives.
//
// The application and the overlay rise and fall together now, by the owner's
// decision: opened means the overlay is up, and Quit stops the daemon -- taking
// the overlay out of every running game within a second -- before ending this
// process. Closing the window with keep-running on merely hides it behind this
// icon, and that is the one case in which the overlay stays up with no window
// on screen; the icon is what makes that state reachable rather than stranded.
//
// Qt.labs.platform rather than a QSystemTrayIcon: it goes through the platform
// theme, which on Plasma means a real StatusNotifierItem over D-Bus rather than
// an X11 tray window that Wayland has no equivalent for.
//
// Its `available` property decides the close behaviour. Where there is no tray --
// a bare compositor with no status area -- closing the window has to quit, or the
// application would vanish with no way to reach it and no way to stop it.

import QtQuick
import Qt.labs.platform as Platform
import Vocem

Platform.SystemTrayIcon {
    id: root

    required property var config
    // Called when the user asks for the window back.
    signal openRequested()

    visible: available

    // The icon is the user's own voice state, and nothing else: one circle at
    // one diameter, with no box behind it and no portrait of the application
    // (the owner's ask, looking at it in the panel -- at 22 px a picture of
    // the product says less than a coloured disc). Green while speaking, red
    // with a microphone while muted, red with headphones while deafened, grey
    // while in a channel saying nothing, and a hollow ring when not in a
    // channel: the fill carries "in a call", so the state is never colour
    // alone -- the rule the window's status mark and the overlay's own ring
    // both live by.
    //
    // Five installed theme icons and a name swap, which is the cheapest update
    // SNI has (a string over D-Bus; measured on a private bus: ten swaps a
    // second arrive one-to-one with no coalescing, and this changes state a
    // few times per sentence at most -- the bridge emits only on change).
    // Deliberately not an animation: every icon change makes the tray host
    // refetch and repaint, and a decoration that costs IPC forever is against
    // what a tray is for.
    //
    // Unless the user asked for the application's own picture instead, on the
    // System tray page. The SAVED answer, never the edited one: every other
    // setting is previewed inside the window and written on Apply, and this is
    // the only one whose subject is a thing on the desktop -- following the edit
    // would alter the panel before Apply and leave it altered if the window were
    // closed without applying. The page has a preview of its own for that.
    icon.name: {
        if (!root.config.appliedTrayVoiceIcon) {
            return "io.github.ales_drnz.vocem_overlay";
        }
        switch (root.config.selfVoice) {
        case ConfigBridge.Speaking: return "io.github.ales_drnz.vocem_overlay-speaking";
        case ConfigBridge.Muted: return "io.github.ales_drnz.vocem_overlay-muted";
        case ConfigBridge.Deafened: return "io.github.ales_drnz.vocem_overlay-deafened";
        case ConfigBridge.InChannelIdle: return "io.github.ales_drnz.vocem_overlay-idle";
        default: return "io.github.ales_drnz.vocem_overlay-offline";
        }
    }
    tooltip: config.statusText

    onActivated: function(reason) {
        // A plain click is "show me the thing"; the context menu covers the rest.
        if (reason === Platform.SystemTrayIcon.Trigger ||
            reason === Platform.SystemTrayIcon.DoubleClick) {
            root.openRequested();
        }
    }

    menu: Platform.Menu {
        Platform.MenuItem {
            text: qsTr("Open Vocem Overlay")
            onTriggered: root.openRequested()
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("Overlay Enabled")
            checkable: true
            checked: root.config.enabled
            onTriggered: root.config.enabled = checked
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            // Quit, meaning quit: the daemon is stopped first, so the overlay
            // leaves the games too, and comes back the next time the
            // application is opened. Not plain Qt.quit(), which would end the
            // window and leave a headless overlay running with nothing on
            // screen to reach it by.
            text: qsTr("Quit")
            onTriggered: root.config.quitOverlay()
        }
    }
}
