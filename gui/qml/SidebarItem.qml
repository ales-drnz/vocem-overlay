// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One entry in the navigation sidebar.
//
// A plain ItemDelegate with an icon, a label and a highlight: the style lays the
// row out and draws the selection, as in every other Qt application. Replacing
// its background or contentItem would take padding and centring away from it.

import QtQuick
import QtQuick.Controls

ItemDelegate {
    id: root

    // A freedesktop icon name from the session's theme; an unknown one leaves
    // the label on its own.
    property alias iconName: root.icon.name

    property bool current: false

    highlighted: current
    icon.width: 22
    icon.height: 22

    // Callers take icon names from one family (`preferences`, or the flat
    // `dialog`/`applications` ones of the same density): Breeze's monochrome
    // `actions` icons exist only at 16 and look misaligned beside them, and
    // org.kde.desktop ignores icon.color, so the artwork cannot be recoloured.
}
