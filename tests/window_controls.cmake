# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The window's controls as they behave: one offscreen run of the window over
# every section with VOCEM_CONFIG_DRIVE=1, which clicks every Switch and
# CheckBox and moves every Slider and SpinBox to both ends (drive_controls in
# gui/src/main.cpp) and dumps, per control, which setting it changed, where a
# slider's ends land after the bridge's clamp, and whether config.ini changed
# on the click. Included by slider_bounds.cmake and instant_switches.cmake,
# which call vocem_window_controls(<scratch directory>) and read the dump's
# "control" lines from `controls` (a CMake list, one JSON object each). A
# macro, as window_status.cmake's is: its return() leaves the calling script.
#
# Scratch XDG directories and VOCEM_CONFIG_NO_DAEMON: nothing reaches the
# owner's settings or the session's services.

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

macro(vocem_window_controls scratch)
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

    file(REMOVE_RECURSE "${scratch}")
    file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache" "${scratch}/data"
         "${scratch}/run")
    file(WRITE "${scratch}/config/vocem/config.ini" "")

    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "LD_PRELOAD=unset:"
            "LC_ALL=set:C.UTF-8"
            "XDG_CONFIG_HOME=set:${scratch}/config"
            "XDG_CACHE_HOME=set:${scratch}/cache"
            "XDG_DATA_HOME=set:${scratch}/data"
            "XDG_RUNTIME_DIR=set:${scratch}/run"
            "VOCEM_DRM_ROOT=set:${scratch}/data"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_NO_DAEMON=set:1"
            "VOCEM_CONFIG_STEP_MS=set:0"
            "VOCEM_CONFIG_DRIVE=set:1"
            "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")
    vocem_window_ran("${status}" "${errors}")
    if(NOT EXISTS "${scratch}/geometry.json")
        message(FATAL_ERROR "the window wrote no geometry dump")
    endif()
    file(STRINGS "${scratch}/geometry.json" controls REGEX "^\\{\"control\": ")
    file(REMOVE_RECURSE "${scratch}/run")
endmacro()
