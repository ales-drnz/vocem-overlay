# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# "Drawing now" follows the world while the window is open.
#
# The live list was computed in the bridge's constructor and never again, so a
# window left open before a game started went on saying "the overlay is not
# drawing anywhere" while the overlay was drawing. Reported by the owner with
# Minecraft up, its journal reading 28223 frames drawn, and the Debug page
# empty -- the page built to break the overlay's silence producing a silence
# of its own.
#
# Nothing that reads state ONCE can be held by a dump taken at the end, so the
# journal here is created AFTER the window is up: the harness walks its
# sections on a timer, the sentinel journal lands two seconds in, and the
# Debug page's dump comes later. Against a bridge that refreshes the live list
# only in its constructor, the row never appears.

if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

# A process that is certainly alive for the length of the run, wearing the
# name the journal records: this test's own cmake process.
execute_process(COMMAND sh -c "echo $PPID" OUTPUT_VARIABLE live_pid
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT EXISTS "/proc/${live_pid}/comm")
    message(STATUS "skip cannot see my own process in /proc")
    return()
endif()
file(READ "/proc/${live_pid}/comm" live_name)
string(STRIP "${live_name}" live_name)

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/live-while-open")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem/journal")

set(journal "${scratch}/cache/vocem/journal/${live_pid}.running")
file(WRITE "${scratch}/late.sh"
"#!/bin/sh
sleep 2
printf 'process = ${live_name}\\npid = ${live_pid}\\napi = opengl\\nstarted = now\\n--\\n' > '${journal}'
printf 'frames = 28223\\ndrawn = 28223\\n' > '${scratch}/cache/vocem/journal/${live_pid}.stat'
")

execute_process(
    COMMAND sh -c "sh '${scratch}/late.sh' & exec '${CONFIG_BINARY}'"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:8"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
if(NOT EXISTS "${journal}")
    message(FATAL_ERROR "the fixture never wrote its journal -- the run was too short")
endif()
file(READ "${scratch}/geometry.json" dump)

# The Debug page's empty-state label is present exactly while nothing is
# drawing, so its visibility at the last section is the measurement: an item
# named debugLiveEmpty still visible means the page never noticed.
string(REGEX MATCHALL "\"item\": \"[^\"]*debugLiveRow[^\"]*\"[^\n]*\"visible\": true"
       rows "${dump}")
if(rows STREQUAL "")
    message(FATAL_ERROR
        "a journal that appeared while the window was open never reached the "
        "Debug page -- the live list is frozen at the moment the window opened, "
        "which is the silence this page exists to break")
endif()

message(STATUS "the live list follows the world while the window is open")
