# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The message preview shows the message.
#
# This test began as the other way round: the daemon withheld a message's text
# unless a switch was on, the preview promised one anyway, and the owner read
# the difference between the promise and the game as the toast failing to draw.
# The switch is gone -- the text is always drawn now, and what keeps it out of
# a game's reach is the transport rather than a setting (vocem/note.h) -- so
# what has to be held here is the plain claim: with nothing configured at all,
# the preview draws a body, because the toast in the game will.
#
# Against the window shipped in 0.1.0-61, whose preview followed a setting that
# defaulted to off, the body is invisible with an empty settings file and this
# fails.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/toast-preview")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
# Nothing configured: the defaults are what a fresh install draws.
file(WRITE "${scratch}/config/vocem/config.ini" "")

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:1"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

vocem_window_ran("${status}" "${errors}")
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

string(REGEX MATCHALL "\"item\": \"[^\"]*messageBody[^\"]*\"[^\n]*\"visible\": true"
       bodies "${dump}")
if(bodies STREQUAL "")
    message(FATAL_ERROR
        "the preview draws no message body with the defaults -- the toast in "
        "the game always carries one, and a preview that shows less is the "
        "promise that sent the owner hunting a drawing defect")
endif()

message(STATUS "the preview draws the message, as the game will")
