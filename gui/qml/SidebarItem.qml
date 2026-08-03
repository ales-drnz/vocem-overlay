// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One entry in the navigation sidebar.
//
// This is a plain ItemDelegate with an icon, a label and a highlight, and that is
// the whole point of it. An earlier version replaced both the background and the
// contentItem to draw an inset rounded "pill", which is what current Kirigami
// applications show -- but Kirigami redefines its delegates wholesale, and doing
// half of that here meant the style no longer had any say in the padding, the
// spacing between icon and label, or the vertical centring. Those became three
// hardcoded numbers that did not agree with each other, and icons whose theme
// artwork has different internal margins sat visibly off from their text.
//
// So the style lays the row out and draws the selection. What it renders is what
// every other Qt application on the desktop renders, which is the only definition
// of "native" that holds up.

import QtQuick
import QtQuick.Controls

ItemDelegate {
    id: root

    // A freedesktop icon name, resolved by the style through the session's icon
    // theme. An empty or unknown name simply leaves the label on its own.
    property alias iconName: root.icon.name

    property bool current: false

    highlighted: current
    icon.width: 22
    icon.height: 22

    // Note for whoever picks the icon names: take them all from one family.
    // The first set mixed Breeze's coloured `preferences` artwork, drawn at 32px,
    // with monochrome `actions` icons that only exist at 16 -- two different
    // palettes and two different densities in one column, which is what made the
    // list look misaligned even though every row is laid out identically. These
    // are all `preferences` or the flat `dialog`/`applications` families that
    // share their density. Recolouring the theme's artwork to force agreement is not an option:
    // icon.color was tried, and org.kde.desktop ignores it.
}
