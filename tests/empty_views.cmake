# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# An empty view says so with a placeholder message, on every page that has one.
#
# The KDE guidelines ask for an icon, a short title and an explanation where a
# view has nothing in it -- not a line of grey text where the content would have
# been. The Debug section was rebuilt on that in 0.1.0-64 and has two of them
# (the Daemon Log tab has none);
# the Applications page, next door, still answered "nothing has run yet" with a
# card holding a single row, and answered a filter that matches nothing with
# another one. Two pages, two shapes, for the same situation.
#
# Measured on the real window with nothing on disk at all: no records, no
# journals, an empty configuration. In that state the Applications page has no
# applications and the Debug section has neither a live instance nor a session,
# so every empty view in the window is showing at once, and each must be a
# PlaceholderMessage (objectName "placeholder").
#
# Against the packaged 0.1.0-64 window the Debug section's placeholders are
# there and the Applications page's is not, so this fails on section 5.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/empty-views")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
file(WRITE "${scratch}/config/vocem/config.ini" "")

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:5,8"
        "VOCEM_CONFIG_STEP_MS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

vocem_window_ran("${status}" "${errors}")
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

# The dump is one line per item, in section order, with a "section" line ahead
# of each page. Split it there and ask each page the question separately.
string(REGEX REPLACE "\n\\{\"section\"" ";{\"section\"" pages "${dump}")

# Section 5 is Applications and section 8 is Debug; the sidebar's order is what
# the dump numbers by, and gui/qml/Main.qml is where that order lives.
macro(needs_placeholder section why)
    set(found FALSE)
    foreach(page IN LISTS pages)
        if(page MATCHES "\"section\": ${section},")
            if(page MATCHES "\"item\": \"[^\"]*placeholder[^\"]*\"[^\n]*\"visible\": true")
                set(found TRUE)
            endif()
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR
            "section ${section}: ${why}, and nothing in the window says so as a "
            "placeholder message -- an empty view answered with a card holding one "
            "line of grey text is what the guidelines have a pattern for")
    endif()
endmacro()

needs_placeholder(5 "the Applications page has nothing in its list")
needs_placeholder(8 "the Debug section has no live instance to show")

message(STATUS "every empty view in the window is a placeholder message")
