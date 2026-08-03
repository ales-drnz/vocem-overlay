// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which swapchains get their colours re-encoded, held to the claim.
//
// The dangerous direction is a wrong formula, not a missing one: an SDR
// swapchain routed through the PQ encode would be as broken as HDR10 without
// it, and HLG -- whose encode this project has not implemented -- must stay
// untouched rather than get BT.2084's curve on the grounds that both are HDR.
// The shader itself can only be judged on an HDR display; what a test can
// hold is that the decision sends exactly the two implemented spaces to the
// converter and everything else, known or unknown, to the stock pipeline.

#include <stdio.h>
#include <stdlib.h>

#include "hdr_pipeline.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_HDR10_ST2084_EXT) == 2,
          "HDR10 PQ gets the PQ encode");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == 1,
          "scRGB gets the linear encode");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == 0,
          "plain sRGB stays untouched");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_PASS_THROUGH_EXT) == 0,
          "pass-through stays untouched (the compositor defines it, not us)");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_BT709_NONLINEAR_EXT) == 0,
          "BT.709 stays untouched");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_HDR10_HLG_EXT) == 0,
          "HLG stays untouched: its encode is not implemented, and a wrong curve is worse");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT) == 0,
          "Display-P3 stays untouched");

    // The SDR white override, read once: set before the first ask.
    setenv("VOCEM_HDR_NITS", "500", 1);
    check(vocem::hdr_sdr_nits() == 500.0f, "VOCEM_HDR_NITS overrides the 203-nit default");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
