// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Where a box sits on the display: one spelling, for the drawing, the previews and
// the tests.
//
// **A position fraction is a place between the margins, not a coordinate.** Zero
// puts the box against the near edge -- at the distance-from-edge setting -- one
// against the far edge, and **0.5 centres it**. The panel and the message box
// each have their own distance-from-edge and share this arithmetic, because the
// previews must reproduce it to the pixel.

#ifndef VOCEM_PLACEMENT_H
#define VOCEM_PLACEMENT_H

namespace vocem {

// The near-edge coordinate of a box of `box` units placed at `fraction` along an
// axis of `extent` units, keeping `inset` units clear of both ends (entry 56).
//
// When the box cannot fit between the margins the margins give: the box is
// centred in whatever room the display has. A box larger than the display starts
// at zero -- clipping from the far edge beats clipping the near one, where the
// names are.
inline float place_within(float fraction, float box, float extent, float inset) {
    const float travel = extent - box - inset * 2.0f;
    if (travel > 0.0f) {
        return inset + fraction * travel;
    }
    const float room = extent - box;
    return room > 0.0f ? room * 0.5f : 0.0f;
}

// The fraction that `place_within` turns back into `position` -- its exact
// inverse, and it has to be, or a drag fights the placement that draws it and
// the panel jumps under the pointer.
inline float fraction_within(float position, float box, float extent, float inset) {
    const float travel = extent - box - inset * 2.0f;
    if (travel <= 0.0f) {
        return 0.0f;
    }
    const float f = (position - inset) / travel;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// The anchor tables are deliberately NOT here: QML cannot read a C++ header, so
// the window's six anchor points live in ScreenMap.qml (left column then right,
// so the Tab order never depends on where the panel is) and the toast's corners
// in NotificationScreen.qml. one_placement_inverse.cmake and
// tests/panel_geometry.cpp hold the two sides together.

}  // namespace vocem

#endif  // VOCEM_PLACEMENT_H
