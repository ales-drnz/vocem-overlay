# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Beside Steam's overlay, with the witness that can tell whose pixels they are.
#
# The old gl_beside_steam / gl_under_steam ran the probe unchanged with Steam's
# gameoverlayrenderer.so in LD_PRELOAD and asserted the probe's own
# foreign-pixel count -- which counts what ANY interposer drew, so both tests
# passed whether Steam's library was loaded, missing, or drawing nothing.
# gl_beside_mangohud.cmake names that trap in its header ("they pass unchanged
# whether the other one drew or not"); these two tests were the ones it was
# describing, and they were never converted. They also inherited a find_file
# cache: with Steam uninstalled after configure, the stale path made the loader
# warn and continue, and the tests went on passing on Steam-free pixels.
#
# The witness here is the same as the MangoHud test's, from the shared
# gl_probe_witness.cmake: our own log lines say whether WE drew. What DESIGN's
# open-risks entry records as measured is exactly what is asserted -- this
# overlay draws in BOTH orders with Steam, so Steam forwards down the chain --
# and nothing more: whether Steam's own overlay drew is not measurable from
# here (its pixels and ours share one count, and it draws nothing in a probe
# with no Steam client running). That half stays unmeasured and said.
#
# Skipped -- as a ctest Skip, not a pass -- when Steam is not installed or the
# path the configure step found has gone stale.

if(NOT EXISTS "${STEAM_OVERLAY}")
    message(STATUS "skip Steam's gameoverlayrenderer.so is not at the configured path")
    return()
endif()
if(NOT EXISTS "${PROBE}" OR NOT EXISTS "${SHIM}" OR NOT EXISTS "${LIBRARY}")
    message(STATUS "skip the GL probe was not built")
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/gl_probe_witness.cmake")

run_probe("${SHIM}" alone_pixels alone_ours)
if(alone_pixels STREQUAL "skip")
    message(STATUS "skip the probe found no display")
    return()
endif()
# The order Steam actually produces: the session's preload first (ours, from
# environment.d), Steam's appended after it.
run_probe("${SHIM}:${STEAM_OVERLAY}" beside_pixels beside_ours)
# And the harder way round, where Steam's hook owns the symbol and ours only
# sees the frame if Steam forwards down the chain.
run_probe("${STEAM_OVERLAY}:${SHIM}" under_pixels under_ours)

message("     Vocem alone          ${alone_pixels} px, ${alone_ours} of our log lines")
message("     Vocem, then Steam    ${beside_pixels} px, ${beside_ours} of our log lines")
message("     Steam, then Vocem    ${under_pixels} px, ${under_ours} of our log lines")

set(failures 0)

# The control: alone, this overlay draws and says so. Without it the two
# checks below could both be satisfied by an overlay that never works.
if(alone_ours GREATER 0 AND NOT alone_pixels STREQUAL "none" AND alone_pixels GREATER 0)
    message("ok   alone, the overlay draws and says so")
else()
    message("FAIL alone, the overlay drew nothing: the rest of this measures nothing")
    math(EXPR failures "${failures} + 1")
endif()

if(beside_ours GREATER 0)
    message("ok   with Steam appended after us, we still draw")
else()
    message("FAIL with Steam appended after us, our overlay went silent")
    math(EXPR failures "${failures} + 1")
endif()

if(under_ours GREATER 0)
    message("ok   with Steam ahead of us, its hook forwards and we still draw")
else()
    message("FAIL with Steam ahead of us, our overlay went silent -- the chain broke")
    math(EXPR failures "${failures} + 1")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} check(s) failed")
endif()
