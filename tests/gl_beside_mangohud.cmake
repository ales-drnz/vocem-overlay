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
# ten names this one does, and its basename fails is_system_gl() exactly as
# ANGLE's libEGL.so does.
#
# The answer is that they exclude each other and whoever is first in LD_PRELOAD
# wins. Measured with vocem_gl_draw_local -- the miniature game whose GL lives
# behind dlopen(RTLD_LOCAL), which is the door the shim exists for -- two passes
# each:
#
#   LD_PRELOAD                  foreign pixels     our own log lines
#   nothing                     0                  --
#   Vocem alone                 1058 / 1058        2
#   MangoHud alone              5324 / 5232        --
#   Vocem, then MangoHud        1058 / 1058        2
#   MangoHud, then Vocem        5350 / 5298        0 / 0
#
# **The pixel count is the wrong witness for the second order and that is the
# trap this file exists to avoid.** With MangoHud first the probe still finds
# thousands of foreign pixels and still passes its own check -- they are
# MangoHud's. A test built on that number would report our overlay drawing when
# it had not drawn at all, which is entry 77's shape. The witness here is our
# own log instead: VOCEM_DEBUG=1 and the two lines only this overlay writes.
#
# Asserted in the direction it was measured, like shim_private_dispatch: a shim
# that learns to chain properly FAILS the second case, and then the DESIGN entry
# gets rewritten rather than the assertion quietly flipped.
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

# One run of the probe under a given preload. Returns the foreign-pixel count
# and how many lines only this overlay writes came out of it.
function(run_probe preload out_pixels out_ours)
    execute_process(
        COMMAND "${PROBE}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "LD_PRELOAD=set:${preload}"
            "VOCEM_SHIM_PRELOADED=set:1"
            "VOCEM_GL_LIBRARY=set:${LIBRARY}"
            "VOCEM_DEBUG=set:1"
            "MANGOHUD=set:1")
    if(status EQUAL 77)
        # No display inside this run: the probe says so itself, and a skip is
        # the honest answer rather than a comparison of two zeroes.
        set(${out_pixels} "skip" PARENT_SCOPE)
        set(${out_ours} 0 PARENT_SCOPE)
        return()
    endif()
    set(both "${output}${errors}")
    if(both MATCHES "foreign pixels:[ \t]*([0-9]+)")
        set(${out_pixels} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_pixels} "none" PARENT_SCOPE)
    endif()
    # The two lines nothing else in the process writes.
    string(REGEX MATCHALL "\\[vocem/gl\\] (OpenGL backend ready|drawing in)" ours "${both}")
    list(LENGTH ours count)
    set(${out_ours} "${count}" PARENT_SCOPE)
endfunction()

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

# First in the preload: we draw, and the count is ours alone -- MangoHud
# contributed nothing to the frame.
if(first_ours GREATER 0 AND first_pixels EQUAL alone_pixels)
    message("ok   first in the preload we draw, and the frame carries our pixels and no others")
else()
    message("FAIL first in the preload: ${first_pixels} px against ${alone_pixels} alone, "
            "${first_ours} log lines")
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
message("ok   whoever is first in LD_PRELOAD draws, and the other does not")
