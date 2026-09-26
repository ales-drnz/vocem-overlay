# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The font picker goes on following the setting after its list has been opened.
#
# The box's currentIndex is a binding on the setting, and ensureFamilies() --
# run when the list opens and when the box takes focus -- assigned it by hand
# to survive the ComboBox's own reset when the model grows. A plain assignment
# in QML removes the binding it lands on, so from the first opening on the box
# no longer followed the setting: the family written in config.ini by hand, by
# a second window or by a script was picked up by the bridge (reloadIfMoved)
# and shown by displayText, which reads the setting directly, while the row
# the list marks and the face the box is drawn in (font.family, from
# currentIndex) stayed on the old family. setFontFamily's refusal path puts the
# box back the same way -- by re-running that binding -- so it stopped working
# too.
#
# Measured on the real window, offscreen, through the geometry dump: the box
# publishes fontIndex (its currentIndex) and fontSetIndex (where the setting's
# family sits in the same list). The walk opens the list once on the
# Appearance section and then stays on the Spacing section, where the box is
# not visible and nothing opens it again, while a script writes another family
# into config.ini; the bridge reloads it on its four-second sweep. Against the
# window as it stood the last dump has fontIndex 0 and fontSetIndex > 0.
#
# The family is one this machine has (fc-list, with the filter the window's
# own list uses), so the test means the same thing on any machine that has a
# TrueType family at all. /dev/shm is private (bwrap), so the window does not
# read the owner's live segment.

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
find_program(BWRAP_BINARY bwrap)
if(NOT BWRAP_BINARY)
    message(STATUS "skip bwrap is missing, so /dev/shm cannot be made private")
    return()
endif()
find_program(FC_LIST fc-list)
if(NOT FC_LIST)
    message(STATUS "skip fc-list is missing, so no family can be chosen")
    return()
endif()
execute_process(COMMAND "${FC_LIST}" ":fontformat=TrueType:outline=true" family
                OUTPUT_VARIABLE families_text OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" families "${families_text}")
list(SORT families)
set(family "")
foreach(candidate IN LISTS families)
    # fc-list prints every name a face answers to, comma-separated; the first
    # is the one the window's list carries.
    string(REGEX REPLACE ",.*" "" candidate "${candidate}")
    string(STRIP "${candidate}" candidate)
    if(NOT candidate STREQUAL "")
        set(family "${candidate}")
        break()
    endif()
endforeach()
if(family STREQUAL "")
    message(STATUS "skip this machine has no TrueType family to pick")
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/font-picker-follows")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache/vocem")
file(WRITE "${scratch}/config/vocem/config.ini" "font_family = \n")

# Two seconds in -- after the first step has opened the list -- the family
# changes on disk. A new mtime is what the bridge's sweep looks for.
file(WRITE "${scratch}/late.sh"
"#!/bin/sh
sleep 2
printf 'font_family = %s\\n' '${family}' > '${scratch}/config/vocem/config.ini.new'
mv '${scratch}/config/vocem/config.ini.new' '${scratch}/config/vocem/config.ini'
")

execute_process(
    COMMAND "${BWRAP_BINARY}" --dev-bind / / --tmpfs /dev/shm --die-with-parent
            sh -c "sh '${scratch}/late.sh' & exec '${CONFIG_BINARY}'"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:"
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "LC_ALL=set:C.UTF-8"
        "VOCEM_CONFIG_OPEN=set:fontFamilyChoice"
        "VOCEM_CONFIG_SECTIONS=set:2,3,3,3,3,3,3,3,3"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

vocem_window_ran("${status}" "${errors}")
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(STRINGS "${scratch}/geometry.json" rows REGEX "fontFamilyChoice\"")
list(LENGTH rows row_count)
if(row_count LESS 2)
    message(FATAL_ERROR "the dump has ${row_count} rows for the font box; expected one a step")
endif()

# The first step: the list has been opened, and the setting is the built-in font.
list(GET rows 0 first)
list(GET rows -1 last)
message("first step: ${first}")
message("last step:  ${last}")
foreach(row IN ITEMS first last)
    if(NOT "${${row}}" MATCHES "\"fontIndex\": ([0-9-]+)")
        message(FATAL_ERROR "the font box publishes no fontIndex")
    endif()
    set(${row}_index "${CMAKE_MATCH_1}")
    if(NOT "${${row}}" MATCHES "\"fontSetIndex\": ([0-9-]+)")
        message(FATAL_ERROR "the font box publishes no fontSetIndex")
    endif()
    set(${row}_set "${CMAKE_MATCH_1}")
endforeach()
if(NOT first_set EQUAL 0 OR NOT first_index EQUAL 0)
    message(FATAL_ERROR
        "with the built-in font set the box should start at the top: fontIndex ${first_index}, "
        "fontSetIndex ${first_set}")
endif()
if(last_set LESS 1)
    message(FATAL_ERROR
        "'${family}' never reached the window, or is not in its list (fontSetIndex "
        "${last_set}) -- nothing was measured")
endif()
if(NOT last_index EQUAL last_set)
    message(FATAL_ERROR
        "config.ini now says '${family}', which is row ${last_set} of the list, and the box is "
        "still on row ${last_index}: its text names the new family while its list and its "
        "face stay on the old one -- the binding on currentIndex was removed when the list was "
        "first opened")
endif()
message(STATUS "the font box follows a family written outside the window after its list was opened "
               "(row ${last_index} of the list)")
