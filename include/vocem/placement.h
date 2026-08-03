// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Where a box sits on the display: one spelling, for the drawing, the previews and
// the tests.
//
// **A position fraction is a place between the margins, not a coordinate.** Zero
// puts the box against the near edge -- at the distance-from-edge setting -- one
// puts it against the far edge, and **0.5 centres it**. That is what an anchor
// point means to somebody clicking one, and it is not what the arithmetic used to
// do: the fraction was multiplied by the display's size and used as the box's
// *top-left corner*, then clamped into the margins. So the middle-of-a-side
// anchors put the panel's top edge at half the display and the whole box hung
// below the middle -- reported by the owner as the snap "counting from the top".
// The corners were right, which is why it took a while to be seen: three of the
// four extremes agree under both readings, and 0.5 is the only fraction where the
// two differ.
//
// The same function serves the message box, which now has a distance-from-edge of
// its own and anchor marks that move with it exactly as the panel's do. Two boxes,
// two settings, one arithmetic -- because the previews have to reproduce this to
// the pixel and the last time a distance lived in two places the slider moved one
// box and not the other (entry 17).

#ifndef VOCEM_PLACEMENT_H
#define VOCEM_PLACEMENT_H

namespace vocem {

// The near-edge coordinate of a box of `box` units placed at `fraction` along an
// axis of `extent` units, keeping `inset` units clear of both ends.
//
// When the box cannot fit between the margins the margins are what give: the box
// is centred in whatever room the display has, which keeps it on screen and keeps
// the answer one a preview can reproduce. A box larger than the display itself
// starts at zero -- there is nothing better to do, and clipping from the far edge
// beats clipping from the near one, where the names are.
inline float place_within(float fraction, float box, float extent, float inset) {
    const float travel = extent - box - inset * 2.0f;
    if (travel > 0.0f) {
        return inset + fraction * travel;
    }
    const float room = extent - box;
    return room > 0.0f ? room * 0.5f : 0.0f;
}

// The fraction that `place_within` turns back into `position` -- its exact
// inverse, and it has to be, or a drag fights the placement that draws it. The
// window used to divide the box's coordinate by the display's size, which is the
// inverse of the arithmetic this file replaced: while a drag was live the two
// disagreed by a whole inset plus a fraction of the box, and the panel jumped
// under the pointer.
inline float fraction_within(float position, float box, float extent, float inset) {
    const float travel = extent - box - inset * 2.0f;
    if (travel <= 0.0f) {
        return 0.0f;
    }
    const float f = (position - inset) / travel;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// The six places the panel can be sent to, in the order the window walks them:
// the four corners and the middle of either side. The fractions are exact -- 0,
// 0.5, 1 -- so "top left" is the same pair of numbers every time and the
// comparison harness stays exact.
struct AnchorPoint {
    float x;
    float y;
};

inline constexpr AnchorPoint kPanelAnchors[] = {
    {0.0f, 0.0f}, {1.0f, 0.0f},  // top corners
    {0.0f, 0.5f}, {1.0f, 0.5f},  // middles of the sides
    {0.0f, 1.0f}, {1.0f, 1.0f},  // bottom corners
};
inline constexpr int kPanelAnchorCount = sizeof(kPanelAnchors) / sizeof(kPanelAnchors[0]);

// The message box is chosen by corner rather than by fraction -- a toast arrives,
// is read and leaves, and it has never needed the middle of a side. These are the
// same four fractions its `notification_corner` means, so its anchor marks can be
// placed by the arithmetic above rather than by a second copy of it.
inline constexpr AnchorPoint kNotificationAnchors[] = {
    {0.0f, 0.0f},  // 0: top left
    {1.0f, 0.0f},  // 1: top right
    {0.0f, 1.0f},  // 2: bottom left
    {1.0f, 1.0f},  // 3: bottom right
};
inline constexpr int kNotificationAnchorCount =
    sizeof(kNotificationAnchors) / sizeof(kNotificationAnchors[0]);

}  // namespace vocem

#endif  // VOCEM_PLACEMENT_H
