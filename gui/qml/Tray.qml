// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The tray icon: the way back to a window that was put away, and the place the
// application's one real Quit lives.
//
// The application and the overlay rise and fall together: Quit stops the daemon
// before ending this process. Closing the window with keep-running on hides it
// behind this icon, the one case in which the overlay stays up with no window.
// Qt.labs.platform, because on Plasma it is a real StatusNotifierItem over D-Bus.
// Where no tray is `available`, closing the window has to quit, or nothing would
// be left to reach or stop the application by.

import QtQuick
import Qt.labs.platform as Platform
import Vocem

Platform.SystemTrayIcon {
    id: root

    required property var config
    // Called when the user asks for the window back.
    signal openRequested()

    visible: available

    // The icon is the user's own voice state: one circle, green while speaking,
    // red with a microphone while muted, red with headphones while deafened,
    // grey while in a channel saying nothing, a hollow ring when not in one --
    // the fill carries "in a call", so the state is never colour alone.
    //
    // Five installed theme icons and a name swap, the cheapest update SNI has (a
    // string over D-Bus; the bridge emits only on change). Never an animation:
    // every change makes the tray host refetch and repaint.
    //
    // Unless the System tray page asks for the application's own picture. The
    // SAVED answer, never the edited one: following the edit would alter the
    // panel before Apply and leave it altered if the window closed unapplied.
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
        // A plain click opens the window; the context menu covers the rest.
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
            // leaves the games too. Not plain Qt.quit(), which would leave a
            // headless overlay running with nothing to reach it by.
            text: qsTr("Quit")
            onTriggered: root.config.quitOverlay()
        }
    }
}
