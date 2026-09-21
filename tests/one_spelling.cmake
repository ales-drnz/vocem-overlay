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

# The same floor, for the same reason, as one_dlsym_version.cmake and for the
# rule one_placement_inverse.cmake writes down: without it a moved directory
# turns this into a test that approves nothing.
list(LENGTH sources source_count)
if(source_count LESS 40)
    message(FATAL_ERROR
        "one_spelling: only ${source_count} source files found under ${SOURCE_DIR} -- "
        "the walk has lost the code, which is not agreement")
endif()
message(STATUS "     ${source_count} source files examined")

# **What this does NOT hold, said rather than implied.** The patterns below are
# name-shaped, not fact-shaped: a second `mkdir -p` loop is caught only if its
# variable is called `partial`, and a second curl sink only if the function is
# called `*append_to_string`. So this pins the three copies that were removed
# and would not notice a fourth written in other words. Making it fact-shaped
# means recognising the SHAPE of each duplicated thing, which is a different
# test; until somebody writes it, this is a guard against the copies coming
# back, not a proof that none exist.

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

# The fourth fact, and the one probe directory of its own.
#
# The three rules above walk the SHIPPED code; this one walks tests/, because
# the copy that grew back was in there. `tests/probe_name.h` is entry 168's one
# spelling of "what is this probe called": readlink /proc/self/exe, take the
# basename untruncated, fall back to the probe's old literal. Entry 168 swept
# nine OpenGL probes and left `vk_present_draw.cpp` -- the probe entry 129 is
# about, which is where the rule came from -- with a hand-rolled copy three
# lines long, in the same release that added the header for it.
#
# The rule is narrow on purpose. Three probes readlink /proc/self/exe for the
# PATH, to re-exec themselves under bwrap or to fork (`private_shm.h`,
# `gl_noop_quiet.cpp`, `emoji_bank_arrives.cpp`, `gl_emoji_colour.cpp`), and
# that is a different fact with a different answer. What only own_name() may do
# is take the BASENAME of that buffer.
file(GLOB test_sources "${SOURCE_DIR}/tests/*.cpp" "${SOURCE_DIR}/tests/*.h")
list(LENGTH test_sources test_count)
if(test_count LESS 40)
    message(FATAL_ERROR
        "one_spelling: only ${test_count} test sources found under ${SOURCE_DIR}/tests -- "
        "the walk has lost the probes, which is not agreement")
endif()
message(STATUS "     ${test_count} test sources examined for the probe's own name")

foreach(source IN LISTS test_sources)
    if(source MATCHES "tests/probe_name\\.h$")
        continue()
    endif()
    file(READ "${source}" whole)
    if(NOT whole MATCHES "/proc/self/exe")
        continue()
    endif()
    file(STRINGS "${source}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "^[ \t]*(//|\\*|/\\*).*" "" code "${line}")
        if(code MATCHES "(strrchr|find_last_of)[ \t]*\\([ \t]*(::)?self")
            list(APPEND offenders
                "${source}:${number}: a second basename of /proc/self/exe (probe_name.h's own_name() is the one)")
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

message("ok   the clock, the mkdir -p, the curl sink and the probe's own name each "
        "live in one file")
