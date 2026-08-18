# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The witness for "which interposer drew": one run of the miniature game under
# a given LD_PRELOAD, returning the foreign-pixel count and how many of the two
# log lines only this overlay writes came out of it. The pixel count alone is
# the WRONG witness beside another interposer -- its pixels count the same --
# which is the whole lesson of gl_beside_mangohud.cmake; both that test and
# gl_beside_steam.cmake include this file so the instrument cannot drift
# between them. Expects PROBE, SHIM and LIBRARY in the including scope.

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
