# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The facts that live in exactly one file, held there.
#
# Entry 33's lesson was that fixing an instance instead of sweeping for the
# pattern is how the same line survives its own post-mortem -- and the sweep
# for this project's other one-place facts found the monotonic clock spelled
# five times, mkdir -p four times (once with different permission bits), and a
# curl callback twice. Each is one header now; this test is what keeps a sixth
# copy from growing back quietly. Like one_dlsym_version.cmake, the rule is
# about code and not prose.

file(GLOB_RECURSE sources
    "${SOURCE_DIR}/gl/src/*.cpp" "${SOURCE_DIR}/gl/src/*.h"
    "${SOURCE_DIR}/layer/src/*.cpp" "${SOURCE_DIR}/layer/src/*.h"
    "${SOURCE_DIR}/daemon/src/*.cpp" "${SOURCE_DIR}/daemon/src/*.h"
    "${SOURCE_DIR}/cli/src/*.cpp"
    "${SOURCE_DIR}/common/src/*.cpp"
    "${SOURCE_DIR}/gui/src/*.cpp" "${SOURCE_DIR}/gui/src/*.h"
    "${SOURCE_DIR}/include/vocem/*.h")

set(offenders "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "^[ \t]*(//|\\*|/\\*).*" "" code "${line}")
        # The one clock: vocem/clock.h.
        if(NOT source MATCHES "include/vocem/clock\\.h$")
            if(code MATCHES "clock_gettime[ \t]*\\([ \t]*CLOCK_MONOTONIC")
                list(APPEND offenders "${source}:${number}: a second monotonic clock (vocem/clock.h is the one)")
            endif()
        endif()
        # The one mkdir -p: vocem/paths.h. A single mkdir of a known directory
        # is fine; a loop feeding mkdir component by component is the copy.
        if(NOT source MATCHES "include/vocem/paths\\.h$")
            if(code MATCHES "::mkdir[ \t]*\\([ \t]*partial")
                list(APPEND offenders "${source}:${number}: a second mkdir -p loop (vocem/paths.h is the one)")
            endif()
        endif()
        # The one curl sink: daemon/src/curl_sink.h.
        if(NOT source MATCHES "curl_sink\\.h$")
            if(code MATCHES "size_t[ \t]+[a-z_]*append_to_string")
                list(APPEND offenders "${source}:${number}: a second curl write callback (daemon/src/curl_sink.h is the one)")
            endif()
        endif()
    endforeach()
endforeach()

if(offenders)
    message("A fact that lives in one file grew a second copy:")
    foreach(offender IN LISTS offenders)
        message("  ${offender}")
    endforeach()
    message(FATAL_ERROR "one spelling per fact -- entry 33 is what a second one costs")
endif()

message("ok   the clock, the mkdir -p and the curl sink each live in one file")
