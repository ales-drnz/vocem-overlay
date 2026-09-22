# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The window's preview quiets the picture of whoever is not talking, as the game
# does (avatar_idle_opacity, panel.cpp's picture_alpha at rest).
#
# An opacity is invisible to a rectangle, so the preview's picture carries it as
# a property the geometry dump prints, and this holds it to the file: with the
# setting at 30%, the Appearance page's previews must draw a picture at 0.3 --
# the quiet participant -- and one at 1, the speaker. Against the window shipped
# in 0.1.9, which had no such setting and drew every picture lit, there is no
# picture strength in the dump at all and this fails.

# Run through ctest, or with -DCMAKE_CURRENT_BINARY_DIR=<build>/tests: in script
# mode that variable is the working directory, and a run started from the
# repository root leaves its scratch directories in the repository.
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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/preview-idle-picture")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
file(WRITE "${scratch}/config/vocem/config.ini" "[appearance]\navatar_idle_opacity = 0.3\n")

# Sections 0 to 2: Appearance is the third, and its live preview and preset
# chips are the pictures read here.
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
        "VOCEM_CONFIG_NO_DAEMON=set:1"
        "VOCEM_CONFIG_SECTIONS=set:2,"
        "VOCEM_CONFIG_STEP_MS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

vocem_window_ran("${status}" "${errors}")
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

string(REGEX MATCHALL "\"item\": \"[^\"]*avatar[^\"]*\"[^\n]*\"pictureOpacity\": [0-9.e-]+"
       pictures "${dump}")
list(LENGTH pictures total)
if(total EQUAL 0)
    message(FATAL_ERROR
        "no preview picture says how strongly it is drawn: the preview has no idea "
        "of avatar_idle_opacity, and draws every face lit where the game quiets them")
endif()

set(quiet 0)
set(lit 0)
set(other "")
foreach(picture IN LISTS pictures)
    if(picture MATCHES "\"pictureOpacity\": ([0-9.e-]+)$")
        set(value "${CMAKE_MATCH_1}")
        if(value STREQUAL "0.3")
            math(EXPR quiet "${quiet} + 1")
        elseif(value STREQUAL "1")
            math(EXPR lit "${lit} + 1")
        else()
            list(APPEND other "${value}")
        endif()
    endif()
endforeach()
message(STATUS "${total} preview pictures: ${quiet} quiet at 0.3, ${lit} lit")
if(NOT other STREQUAL "")
    message(FATAL_ERROR "a preview picture at a strength that is neither: ${other}")
endif()
if(quiet EQUAL 0)
    message(FATAL_ERROR "no preview picture is quieted to the file's 30%")
endif()
if(lit EQUAL 0)
    message(FATAL_ERROR "no preview picture is lit: the speaker's must be")
endif()

message(STATUS "the preview quiets whoever is not talking, as the game will")
