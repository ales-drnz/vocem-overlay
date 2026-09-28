// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which height sizes the atlas -- the one decision both injected paths make.
//
// Entry 39 established that a window is where the overlay is drawn, not how
// large it should be: the atlas follows the display's published mode height,
// and zero falls back to the drawable. Both paths spelt that as a ternary,
// and the ternary has a hole measured this session: a drawable TALLER than
// every display (supersampling/DSR -- a 4320-pixel drawable on a 2160
// display) got the display's sizing and landed on screen at 0.5x after the
// compositor's downscale, the illegibly-small direction DESIGN refuses.
// sizing_height() closes it: a drawable taller than the display can only be
// headed for a downscale, so it wins. Against the ternary the supersampled
// case here answers 2160 instead of 4320 and this fails.

#include <stdio.h>

#include "vocem/fonts.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

// fonts.h includes imgui.h for the Fonts struct; this test only needs the
// header-inline sizing_height(), so ImGui is include-only here, never linked.

int main() {
    // No published display height: the old fallback, the drawable.
    check(vocem::sizing_height(0, 1080) == 1080,
          "zero display height falls back to the drawable");

    // Entry 39's case, unchanged: a window shorter than the display is sized
    // by the display, so resizing the window does not rubber-band the panel.
    check(vocem::sizing_height(2160, 1080) == 2160,
          "a windowed drawable is sized by the display (entry 39)");

    // The rescue this test exists for: a supersampled drawable taller than
    // every display is headed for the compositor's downscale, and only sizing
    // from the drawable cancels it. The ternary answers 2160 here.
    check(vocem::sizing_height(2160, 4320) == 4320,
          "a drawable taller than the display is sized by the drawable");

    // Fullscreen at the display's own size: both agree, nothing to decide.
    check(vocem::sizing_height(1080, 1080) == 1080,
          "fullscreen at the display's size answers that size");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
