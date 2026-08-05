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

    // And the other half of the question, which is not about HDR at all: a
    // format that carries the sRGB encoding itself. The hardware applies
    // linear->sRGB to whatever the shader writes, so ImGui's already-encoded
    // colours are encoded twice -- measured, the panel's 79,84,92 stored as
    // 151,155,162. Mode 3 hands the hardware what it expects.
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_FORMAT_B8G8R8A8_SRGB) == 3,
          "an sRGB format is handed over linear, whatever its colour space says");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_FORMAT_R8G8B8A8_SRGB) == 3,
          "and the other byte order of the same thing");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_FORMAT_B8G8R8A8_UNORM) == 0,
          "a UNORM format is left alone: nothing encodes it on the way in");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                              VK_FORMAT_A2B10G10R10_UNORM_PACK32) == 0,
          "nor is the ten-bit one a swapchain reaches for next");

    // The order between the two questions, which is a decision and not an
    // accident: a colour space this project implements always arrives in a
    // format that carries no encoding of its own, so if both could ever speak
    // the space is the one that describes what the display will do.
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_FORMAT_R8G8B8A8_SRGB) == 2,
          "the colour space wins over the format where both have an opinion");
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, VK_FORMAT_R8G8B8A8_SRGB) ==
              1,
          "and on scRGB as well");

    // The default argument keeps every caller that only knows a colour space
    // answering as it did: nothing is quietly rerouted by a call that was not
    // updated.
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == 0,
          "a caller that names no format gets the answer it always got");

    check(vocem::format_is_srgb(VK_FORMAT_A8B8G8R8_SRGB_PACK32),
          "the packed sRGB format counts as one");
    check(!vocem::format_is_srgb(VK_FORMAT_R16G16B16A16_SFLOAT),
          "and a float format does not");

    // The SDR white override, read once: set before the first ask.
    setenv("VOCEM_HDR_NITS", "500", 1);
    check(vocem::hdr_sdr_nits() == 500.0f, "VOCEM_HDR_NITS overrides the 203-nit default");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
