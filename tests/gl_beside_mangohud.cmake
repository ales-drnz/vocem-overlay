# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Two OpenGL interposers in one process, and which of them draws.
#
# DESIGN carried "Steam's gameoverlayrenderer.so stacks with ours in both
# orders -- measured, same 1004 foreign pixels either way" as a settled fact.
# It was half a fact: gl_beside_steam and gl_under_steam count what *this*
# overlay drew, so they pass unchanged whether the other one drew or not, and
# "the same either way" reads equally as "the other one drew nothing in the
# probe". Nobody had asked the other question.
#
# Asked here, against MangoHud, which is the case that is live on this machine:
# MANGOHUD=1 sits in environment.d and the `mangohud` wrapper appends
# libMangoHud_shim.so *after* the session's preload. That shim exports the same
# ten names this one does, and an unversioned dlsym of its own.
#
# Measured with vocem_gl_draw_local -- the miniature game whose GL lives
# behind dlopen(RTLD_LOCAL), which is the door the shim exists for -- two passes
# each, MangoHud 0.8.4:
#
#   LD_PRELOAD                  foreign pixels     our own log lines
#   Vocem alone                 566 / 566          2
#   MangoHud alone              5126 / 5084        --
#   Vocem, then MangoHud        5785 / 5706        2
#   MangoHud, then Vocem        5063 / 5176        0 / 0
#
# **First in the preload, both draw.** Until the shim learned to chain, that
# row was our count to the pixel (1058 / 1058 on the probe of the time): the
# shim answered the game's dlsym from libc directly -- a versioned lookup,
# which never finds MangoHud's unversioned dlsym -- and forwarded every frame
# to the system's function, so MangoHud loaded, hooked, and never saw one.
# Now the game's question goes to the next dlsym in the chain and a frame goes
# to MangoHud when MangoHud is what answered (entry 115; the mechanism, each
# door and each shape of interposer are tests/shim_chain.cpp, which needs no
# display). The row is asserted as ours plus most of MangoHud's.
#
# **Second, we still never see a frame**, and nothing behind MangoHud can
# change that: its dlsym hands the game its own hook, which calls the system's
# function directly. vocem-run puts the shim first for that reason.
#
# **The pixel count is the wrong witness for that order and that is the trap
# this file exists to avoid.** With MangoHud first the probe still finds
# thousands of foreign pixels -- they are MangoHud's. A test built on that
# number would report our overlay drawing when it had not drawn at all, which
# is entry 77's shape. The witness is our own log instead: VOCEM_DEBUG=1 and
# the two lines only this overlay writes.
#
# Asserted in the direction it was measured: a shim that learns to draw behind
# MangoHud FAILS the fourth check, and then the DESIGN entry is rewritten
# rather than the assertion quietly flipped -- as happened to the third.
#
# Skipped where MangoHud is not installed.

if(NOT EXISTS "${MANGOHUD_SHIM}")
    message(STATUS "skip MangoHud's OpenGL shim is not installed here")
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
run_probe("${MANGOHUD_SHIM}" mango_pixels mango_ours)
run_probe("${SHIM}:${MANGOHUD_SHIM}" first_pixels first_ours)
run_probe("${MANGOHUD_SHIM}:${SHIM}" second_pixels second_ours)

message("     Vocem alone            ${alone_pixels} px, ${alone_ours} of our log lines")
message("     MangoHud alone         ${mango_pixels} px")
message("     Vocem, then MangoHud   ${first_pixels} px, ${first_ours} of our log lines")
message("     MangoHud, then Vocem   ${second_pixels} px, ${second_ours} of our log lines")

set(failures 0)

# The control: alone, this overlay draws and says so. Without this the two
# comparisons below could both be satisfied by an overlay that never works.
if(alone_ours GREATER 0 AND NOT alone_pixels STREQUAL "none" AND alone_pixels GREATER 0)
    message("ok   alone, the overlay draws and says so")
else()
    message("FAIL alone, the overlay drew nothing: the rest of this measures nothing")
    math(EXPR failures "${failures} + 1")
endif()

# MangoHud alone has to draw a great deal more than we do, or "the count did not
# change" below is not evidence of anything.
if(NOT mango_pixels STREQUAL "none" AND mango_pixels GREATER alone_pixels)
    message("ok   MangoHud alone draws, and more of the frame than we do")
else()
    message("FAIL MangoHud alone drew nothing here: it cannot be told apart from being cut out")
    math(EXPR failures "${failures} + 1")
endif()

# First in the preload: we draw, and so does MangoHud -- the frame carries our
# pixels and most of what MangoHud draws alone (its own count moves by a few
# percent between passes, so half of it is the bar, far above ours alone).
set(both_floor "")
if(alone_pixels MATCHES "^[0-9]+$" AND mango_pixels MATCHES "^[0-9]+$")
    math(EXPR both_floor "${alone_pixels} + ${mango_pixels} / 2")
endif()
if(first_ours GREATER 0 AND NOT both_floor STREQUAL "" AND first_pixels GREATER both_floor)
    message("ok   first in the preload we draw, and MangoHud behind us draws as well")
else()
    message("FAIL first in the preload: ${first_pixels} px against ${alone_pixels} ours alone "
            "and ${mango_pixels} MangoHud's alone, ${first_ours} log lines -- the chain is cut")
    math(EXPR failures "${failures} + 1")
endif()

# Second in the preload: we do not draw at all. Asserted as measured, not as
# wished for -- see the header.
if(second_ours EQUAL 0)
    message("ok   second in the preload we never reach a frame, which is the measured state")
else()
    message("FAIL second in the preload we now draw (${second_ours} log lines). That is an "
            "improvement, and DESIGN's entry has to be rewritten rather than this flipped.")
    math(EXPR failures "${failures} + 1")
endif()

if(failures)
    message(FATAL_ERROR "the two interposers no longer behave as measured")
endif()
message("ok   first in LD_PRELOAD both draw; behind MangoHud we do not")
