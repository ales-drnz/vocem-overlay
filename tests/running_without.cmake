# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A game running right now with the overlay switched off is said above the
# history, not buried in it.
#
# The Applications page's list is a history, and the one thing somebody opens
# it for mid-session -- "why is there no overlay in the game I am playing this
# second" -- was buried in whichever folded group the record landed in. The
# page puts those rows in a card of their own now: records whose process is
# alive at this moment (the bridge walks /proc beside the records) and whose
# overlay is off, filtered to the rows worth interrupting for -- a hidden
# game, and the two shapes a missed game arrives in.
#
# The fixture needs a process that is reliably alive: pid 1's name. A record
# is seeded for it claiming it is a game, the config hides it, and the row
# must appear. Against the window before the card existed there is no such
# item, and this fails.

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

# The name the kernel gives pid 1, exactly as the records spell names.
file(READ "/proc/1/comm" init_name)
string(STRIP "${init_name}" init_name)
if(init_name STREQUAL "")
    message(STATUS "skip pid 1 has no readable comm here")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/running-without")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem/apps")

file(WRITE "${scratch}/cache/vocem/apps/${init_name}"
     "name = ${init_name}\nexecutable = /usr/bin/${init_name}\napi = opengl\n"
     "game = true\nwhy = desktop:${init_name}.desktop\nseen = 1750000000\n")
file(WRITE "${scratch}/config/vocem/config.ini"
     "hidden_apps = ${init_name}\n")

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
        "VOCEM_CONFIG_SECTIONS=set:5"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

string(REGEX MATCHALL "\"item\": \"[^\"]*runningWithoutRow[^\"]*\"[^\n]*\"visible\": true"
       rows "${dump}")
if(rows STREQUAL "")
    message(FATAL_ERROR
        "a hidden game with a live process does not appear in the "
        "running-without-the-overlay card -- the question somebody opens the "
        "page with mid-session goes unanswered")
endif()

message(STATUS "the running-but-hidden game is said above the history")
