# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The overlay inside gamescope, which the field said we could not survive.
#
# The claim that came in from outside was that gamescope strips LD_PRELOAD, and
# it is repeated with a real citation behind it: gamescope does remove Steam's
# `gameoverlayrenderer.so` on purpose -- its maintainer measured that leaving it
# in "ends up breaking all input" -- and Steam's own overlay is still invisible
# for Proton games under gamescope. Generalised to "gamescope strips the
# preload", that would take the whole OpenGL half away from anybody playing on a
# Deck or in a gamescope session, and it was believed here for a day.
#
# Measured, it is false: gamescope hands the child LD_PRELOAD verbatim, the shim
# loads, and the overlay draws the same 1058 pixels it draws outside. What
# gamescope removes is one named library, not the variable.
#
# So this pins the measurement rather than the belief, because both directions
# would be news: gamescope learning to strip the variable takes the OpenGL half
# out of every gamescope session silently (entry 38's shape -- loads, hooks,
# never draws), and gamescope's own Vulkan WSI layer sits exactly where our
# present hook does, so this is the neighbourhood where a gamescope update can
# reach us.
#
# The witness is our own log, not the pixel count, for the reason
# gl_beside_mangohud.cmake exists: gamescope composites, MangoHud may be in the
# session, and a frame with foreign pixels in it says nothing about WHOSE they
# are. The count is compared against the same probe run plainly in the same
# pass, so the comparison is between two numbers taken minutes apart on one
# machine rather than against a constant in a file.
#
# Skipped where gamescope is not installed. Headless on purpose: `--backend
# headless` composites without a DRM output, so nothing appears on the owner's
# screen -- he may be in a game while this runs. That also means this measures
# the injection and the draw, NOT gamescope's scanout; the probe reads the
# frame out of its own window before gamescope would ever composite it, which is
# the honest scope of the claim.
#
# Expects PROBE, SHIM, LIBRARY and GAMESCOPE.

if(NOT EXISTS "${PROBE}" OR NOT EXISTS "${SHIM}" OR NOT EXISTS "${LIBRARY}")
    message(STATUS "skip the GL probe was not built")
    return()
endif()
if(NOT GAMESCOPE OR NOT EXISTS "${GAMESCOPE}")
    message(STATUS "skip gamescope is not installed here")
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/gl_probe_witness.cmake")

# The same run as run_probe, one nesting deeper. Kept beside the shared helper
# rather than inside it: the helper's whole point is that gl_beside_steam and
# gl_beside_mangohud cannot drift apart, and this needs a different COMMAND, not
# a different instrument.
function(run_probe_in_gamescope preload out_pixels out_ours)
    execute_process(
        COMMAND "${GAMESCOPE}" --backend headless -- "${PROBE}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 180
        ENVIRONMENT_MODIFICATION
            "LD_PRELOAD=set:${preload}"
            "VOCEM_SHIM_PRELOADED=set:1"
            "VOCEM_GL_LIBRARY=set:${LIBRARY}"
            "VOCEM_DEBUG=set:1"
            "MANGOHUD=set:1")
    set(both "${output}${errors}")
    if(both MATCHES "skip no DISPLAY" OR both MATCHES "skip the display did not open")
        set(${out_pixels} "skip" PARENT_SCOPE)
        set(${out_ours} 0 PARENT_SCOPE)
        return()
    endif()
    if(both MATCHES "foreign pixels:[ \t]*([0-9]+)")
        set(${out_pixels} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_pixels} "none" PARENT_SCOPE)
    endif()
    string(REGEX MATCHALL "\\[vocem/gl\\] (OpenGL backend ready|drawing in)" ours "${both}")
    list(LENGTH ours count)
    set(${out_ours} "${count}" PARENT_SCOPE)
endfunction()

run_probe("${SHIM}" plain_pixels plain_ours)
if(plain_pixels STREQUAL "skip")
    message(STATUS "skip the probe found no display")
    return()
endif()
run_probe_in_gamescope("${SHIM}" nested_pixels nested_ours)
if(nested_pixels STREQUAL "skip")
    message(STATUS "skip gamescope did not give the probe a display")
    return()
endif()

message("     plain              ${plain_pixels} px, ${plain_ours} of our log lines")
message("     inside gamescope   ${nested_pixels} px, ${nested_ours} of our log lines")

set(failures 0)

# The control: plainly, this overlay draws and says so. Without it the two below
# could both be satisfied by an overlay that never works anywhere.
if(plain_ours GREATER 0 AND NOT plain_pixels STREQUAL "none" AND plain_pixels GREATER 0)
    message("ok   plainly, the overlay draws and says so")
else()
    message("FAIL plainly the overlay drew nothing: the rest of this measures nothing")
    math(EXPR failures "${failures} + 1")
endif()

# The claim. Our log lines can only come from our shim having been loaded, so
# their presence IS the answer to "did the preload survive".
if(nested_ours GREATER 0)
    message("ok   gamescope passes LD_PRELOAD through: the shim loads and the library draws")
else()
    message("FAIL nothing of ours ran inside gamescope. If gamescope now strips the variable "
            "rather than the one library it used to, the OpenGL half is gone from every "
            "gamescope session and DESIGN's entry has to be rewritten rather than this "
            "assertion relaxed.")
    math(EXPR failures "${failures} + 1")
endif()

# And it draws the same frame, not a degraded one.
if(NOT nested_pixels STREQUAL "none" AND nested_pixels EQUAL plain_pixels)
    message("ok   and the frame it draws inside gamescope is the frame it draws outside")
else()
    message("FAIL inside gamescope the count is ${nested_pixels} against ${plain_pixels} plainly")
    math(EXPR failures "${failures} + 1")
endif()

if(failures)
    message(FATAL_ERROR "the overlay no longer behaves inside gamescope as measured")
endif()
message("ok   the OpenGL half survives gamescope")
