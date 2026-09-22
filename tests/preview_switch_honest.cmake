# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A switch that takes the overlay off the screen has to show in the picture of
# the screen.
#
# The header carries three switches, and the two that decide what is drawn reach
# a running game in about two seconds (they are `persistNow()` settings). The
# message half was honest from the start: `notificationsEnabled` is read in
# seven places, two of them opacity dimmings in the map of the display and in
# the live preview column. `panelEnabled` was read in exactly one place -- the
# Switch that sets it -- so with the voice panel switched OFF the Panel page's
# map, the Appearance and Spacing preview columns and the live preview all went
# on drawing a panel at full strength, identical to the enabled state, while the
# game drew nothing. Its own sibling in the same file had the treatment, which
# is what made the gap visible at all.
#
# What is deliberately NOT dimmed: the five preset chips on the Appearance page.
# They are how somebody chooses what the panel will look like -- including
# before switching it on -- and a chip is a swatch rather than a picture of the
# display. The maps and the live preview claim to show what is on screen; a
# chip claims to show what a preset is.
#
# The instrument is the geometry dump's `opacity`, which is on every named item
# (entry 143 had to publish `pictureOpacity` by hand for the same reason: a
# strength is invisible to a rectangle).

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a test "
        "directory: run this through ctest, so the scratch files land in the build tree")
endif()
if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/preview-switch-honest")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")

# One run at a given config.ini, dumping sections 0 and 1 -- the two pages that
# carry a map of the display.
function(run_window ini out_dump)
    file(WRITE "${scratch}/config/vocem/config.ini" "${ini}")
    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${scratch}/config"
            "XDG_CACHE_HOME=set:${scratch}/cache"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_SECTIONS=set:0,1"
            "VOCEM_CONFIG_STEP_MS=set:0"
            "VOCEM_CONFIG_GEOMETRY=set:${scratch}/${out_dump}.json")
    vocem_window_ran("${status}" "${errors}")
    if(NOT EXISTS "${scratch}/${out_dump}.json")
        message(FATAL_ERROR "the window wrote no geometry dump for ${out_dump}")
    endif()
endfunction()

# The strongest a named box was drawn at anywhere in the dump, in thousandths.
# The strongest, because the same box is dumped on every step and only the page
# on screen is the claim; an off-screen copy may be dimmed by its page.
function(strength dump name out)
    set(best -1)
    string(REGEX MATCHALL "\"item\": \"[^\"]*/${name}\"[^\n]*" lines "${dump}")
    foreach(line IN LISTS lines)
        if(line MATCHES "\"opacity\": ([0-9.]+)")
            set(value "${CMAKE_MATCH_1}")
            # CMake compares integers; thousandths keep three decimals of it.
            string(REGEX REPLACE "^([0-9]+)$" "\\1.0" value "${value}")
            string(REGEX MATCH "^([0-9]+)\\.([0-9]*)" ignored "${value}")
            set(whole "${CMAKE_MATCH_1}")
            set(fraction "${CMAKE_MATCH_2}000")
            string(SUBSTRING "${fraction}" 0 3 fraction)
            math(EXPR thousandths "${whole} * 1000 + ${fraction}")
            if(thousandths GREATER best)
                set(best "${thousandths}")
            endif()
        endif()
    endforeach()
    set(${out} "${best}" PARENT_SCOPE)
endfunction()

run_window("" on)
file(READ "${scratch}/on.json" on_dump)
strength("${on_dump}" "panel" panel_on)
strength("${on_dump}" "message" message_on)

run_window("panel_enabled = false\nnotifications_enabled = false\n" off)
file(READ "${scratch}/off.json" off_dump)
strength("${off_dump}" "panel" panel_off)
strength("${off_dump}" "message" message_off)

message("     panel   ${panel_on} -> ${panel_off} thousandths of strength")
message("     message ${message_on} -> ${message_off}")

# The floor first: a dump with no box in it reports -1 for both, and two equal
# absences would otherwise read as agreement (entry 105's rule).
if(panel_on LESS 0 OR message_on LESS 0 OR panel_off LESS 0 OR message_off LESS 0)
    message(FATAL_ERROR
        "a box was not found in one of the dumps (panel ${panel_on}/${panel_off}, message "
        "${message_on}/${message_off}): this measured nothing")
endif()
if(NOT panel_on EQUAL 1000 OR NOT message_on EQUAL 1000)
    message(FATAL_ERROR
        "with both switches on, the boxes are drawn at ${panel_on} and ${message_on} rather "
        "than full strength: whatever is dimming them is not the switch")
endif()

set(failures 0)
if(panel_off LESS panel_on)
    message("ok   the voice panel switched off is drawn faint in the map, not as if it were on")
else()
    message("FAIL the Panel map draws the panel at ${panel_off} with the switch OFF, the same "
            "as with it on. The game draws nothing in that state, and this picture claims to "
            "be a picture of the screen.")
    math(EXPR failures "${failures} + 1")
endif()
if(message_off LESS message_on)
    message("ok   and the message box with its own switch, as it already did")
else()
    message("FAIL the message box is drawn at ${message_off} with its switch OFF")
    math(EXPR failures "${failures} + 1")
endif()

if(failures)
    message(FATAL_ERROR "a switch that empties the overlay is invisible in the window's own "
                        "picture of it")
endif()
message("ok   both master switches reach the picture of the display")
