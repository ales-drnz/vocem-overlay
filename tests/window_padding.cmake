# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# One padding, in every card, on every page of the settings window.
#
# The defect this holds is small and was found by eye: a loose Label dropped
# into a Card sits flush against the card's edge while the SettingRow beside it
# is inset by Theme.cardPadding. It shipped once in the Debug section, was
# noticed and fixed there, and was still on the Applications page in 0.1.0-64 --
# the paragraph over "Running without the overlay", 17 units of unaccounted
# height at the default window size and 34 at the minimum, where it wraps.
#
# Measured rather than looked at. Every card carries objectName "card" and every
# padded block inside one carries "cardContent", so the geometry dump already
# says where they all landed; scripts/check-padding.py reads it and holds two
# claims:
#
#   * every block is inset from its card by the amount the rest of the window
#     agrees on, left and right;
#   * a card is exactly as tall as the blocks in it -- which is what catches an
#     item that is in a card without taking the card's padding, since it adds
#     height nothing accounts for.
#
# Both window sizes, because a padding that only holds at one width is not a
# padding, and the Applications defect is twice as large at the minimum.
#
# Three runs of the real window, offscreen, over every section. Two fixtures,
# because the Debug section opens on whichever of its tabs has something to say:
# with a crash waiting it opens on the sessions list (whose rows are cards of
# their own), and with nothing wrong it opens on the health card.
#
# Against the packaged 0.1.0-64 window nothing in the scene is named and the
# check reports that it measured no cards at all; against this tree with the
# names but without the fixes it reports the Applications paragraph and the
# Appearance preset block by name, position and height.

# Run through ctest, or with -DCMAKE_CURRENT_BINARY_DIR=<build>/tests: in script
# mode that variable is the working directory, and a run started from the
# repository root left its scratch directories in the repository (0.1.7's
# packaging pass left two, untracked, beside CMakeLists.txt).
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

find_program(PYTHON3 NAMES python3)
if(NOT PYTHON3)
    message(STATUS "skip no python3 to read the dump with")
    return()
endif()

# A process that is certainly alive: pid 1, under the name the kernel gives it,
# which is how the records and the journals spell names.
if(NOT EXISTS "/proc/1/comm")
    message(STATUS "skip pid 1 has no readable comm here")
    return()
endif()
file(READ "/proc/1/comm" init_name)
string(STRIP "${init_name}" init_name)
if(init_name STREQUAL "")
    message(STATUS "skip pid 1 has no readable comm here")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/window-padding")
file(REMOVE_RECURSE "${scratch}")

# What each fixture holds, and why the page needs it:
#
#   a record for a game whose process is alive, hidden  -> the Applications
#     page's "Running without the overlay" card, which is where the paragraph
#     was;
#   a journal for that same live process                -> the live card on
#     Applications and the "Drawing now" card on Debug;
#   a journal whose process is gone                     -> a session row in the
#     Debug list, which is a card built by hand rather than from Card.qml;
#   one that ended cleanly                              -> the other kind of
#     row in the same list.
function(seed root with_crash)
    file(MAKE_DIRECTORY "${root}/config/vocem")
    file(MAKE_DIRECTORY "${root}/cache/vocem/apps")
    file(MAKE_DIRECTORY "${root}/cache/vocem/journal")
    file(WRITE "${root}/cache/vocem/apps/${init_name}"
         "name = ${init_name}\nexecutable = /usr/bin/${init_name}\napi = opengl\n"
         "game = true\nwhy = desktop:${init_name}.desktop\nseen = 1750000000\n")
    file(WRITE "${root}/config/vocem/config.ini" "hidden_apps = ${init_name}\n")
    file(WRITE "${root}/cache/vocem/journal/1.running"
         "process = ${init_name}\npid = 1\napi = opengl\nstarted = 2026-01-01 00:00:00\n--\n"
         "00:00:01 OpenGL backend ready\n")
    file(WRITE "${root}/cache/vocem/journal/1.stat" "frames = 28223\ndrawn = 28223\n")
    file(WRITE "${root}/cache/vocem/journal/4194306.done"
         "process = b-game\npid = 4194306\napi = opengl\nstarted = 2026-01-01 00:00:00\n--\n"
         "00:00:09 clean exit\n")
    if(with_crash)
        # A pid past anything this kernel will hand out, so the scanner cannot
        # mistake it for a live process.
        file(WRITE "${root}/cache/vocem/journal/4194305.running"
             "process = a-game\npid = 4194305\napi = vulkan\nstarted = 2026-01-01 00:00:00\n--\n"
             "00:00:01 Vulkan backend ready\n")
    endif()
endfunction()

seed("${scratch}/sessions" TRUE)
seed("${scratch}/now" FALSE)

set(dumps "")

# A macro rather than a function, so that a window which cannot run here skips
# the whole test instead of only this walk.
macro(walk fixture size)
    set(dump "${scratch}/${fixture}-${size}.json")
include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${scratch}/${fixture}/config"
            "XDG_CACHE_HOME=set:${scratch}/${fixture}/cache"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_SECTIONS=set:9"
            "VOCEM_CONFIG_SIZE=set:${size}"
            "VOCEM_CONFIG_GEOMETRY=set:${dump}")
    vocem_window_ran("${status}" "${errors}")
    if(NOT EXISTS "${dump}")
        message(FATAL_ERROR "the window wrote no geometry dump at ${size}")
    endif()
    # Every section, About included. VOCEM_CONFIG_SECTIONS can only ask for
    # FEWER than the walk's own default, so asking for all of them is a value
    # the window discards -- which means the coverage here rests on that
    # default and nothing was checking it. This is the check.
    file(READ "${dump}" walked)
    if(NOT walked MATCHES "\"section\": 9,")
        message(FATAL_ERROR
            "the walk stopped before the last section at ${size} -- the padding "
            "below would be measured on a window nobody looked at all of")
    endif()
    list(APPEND dumps "${dump}")
endmacro()

walk(sessions 1160x720)
walk(sessions 920x580)
walk(now 1160x720)

execute_process(
    COMMAND "${PYTHON3}" "${SOURCE_DIR}/scripts/check-padding.py" ${dumps}
    RESULT_VARIABLE padding_status
    OUTPUT_VARIABLE padding_report
    ERROR_VARIABLE padding_errors)
message(STATUS "${padding_report}${padding_errors}")

if(NOT padding_status EQUAL 0)
    message(FATAL_ERROR
        "the window's cards are not padded alike -- something sits in a card "
        "without taking the card's padding, which is the seam the owner spotted "
        "by eye in the Debug section and which this measures instead")
endif()

message(STATUS "one padding, in every card, on every page, at both window sizes")
