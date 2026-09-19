# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The map of the display follows the machine while the window is open.
#
# It did not. `displays()`, `overlay_display_height()` and `screen_resolution()`
# in gui/src/environment.h were function-local statics computed on first use, so
# the map's shape, the map's caption, the "Map shows" dropdown and the
# overlay-height figure all came from the machine as it was when the window
# process started -- and this is a tray application that `start_at_login` starts
# hidden at login, so "when the process started" means "at login". The daemon,
# meanwhile, re-reads /sys/class/drm every sixty seconds and republishes, and
# running games follow it: plug a monitor in and within a minute the overlay
# resizes for it while the picture of the overlay goes on drawing the display
# that was there this morning. Entry 63's shape, in the one instrument the owner
# uses to place the overlay.
#
# ---- why preview_display.cmake could not have caught this
#
# It sets VOCEM_DRM_ROOT before launching the window, so every display it can
# ever describe is one the window already knew about at birth. A fixture that
# predates the process can only measure what the program knew when it started.
# That sentence is the finding, and it is why this test plants the second
# connector 1.6 seconds in, while the window is up -- the same thing
# live_while_open.cmake does with a journal, and for the same reason.
#
# ---- the witness, and why it is a shape
#
# 1280x1024 (5:4, aspect 1.250) to start with, and a 3840x2160 connector added
# while the window runs. Both maps take their shape from the display the overlay
# is sized for, which is the tallest, so the new connector must turn a 5:4 map
# into a 16:9 one. A shape rather than a caption because the geometry dump holds
# rectangles and numbers and never text; and 5:4 rather than the obvious
# 1920x1080, because 1920x1080 and 3840x2160 are the same shape and a map that
# never noticed would have measured identical to one that did.
#
# The maps live on the first two sections and the window's slow sweep is four
# seconds, so a walk that visits each page once and never comes back would only
# ever have a map laid out off-screen to measure. The walk here is explicit --
# "0,1,2,3,0,1" -- and comes back to both maps at about 5.5 and 6.6 seconds,
# after the sweep, so every rectangle this test reads is one the window was
# actually drawing. Only visible ones are read, for that reason.
#
# The control run, both connectors present before the window starts, is what
# makes this a defect of the remembering rather than of the reader.
#
# Measured against the packaged 0.1.4-1 window: 1.250 on every section with the
# connector added at 1.6 s, and 1.778 with both present at startup. Against the
# fix: 1.250 before the sweep and 1.778 after it, on both maps.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/display-change-while-open")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")

# The tree the window is born into: one 5:4 display.
file(MAKE_DIRECTORY "${scratch}/drm/card0-DP-1")
file(WRITE "${scratch}/drm/card0-DP-1/status" "connected\n")
file(WRITE "${scratch}/drm/card0-DP-1/enabled" "enabled\n")
file(WRITE "${scratch}/drm/card0-DP-1/modes" "1280x1024\n")
file(WRITE "${scratch}/config/vocem/config.ini" "")

# The taller one, written 1.6 s in: after section 0's grab at about 1.1 s and
# before the window's first four-second sweep. The three files are written into
# a directory that is renamed into place, so the reader can never see a
# connector with a status and no modes.
file(WRITE "${scratch}/plug.sh"
"#!/bin/sh
sleep 1.6
mkdir -p '${scratch}/pending/card0-HDMI-A-1'
printf 'connected\\n' > '${scratch}/pending/card0-HDMI-A-1/status'
printf 'enabled\\n'   > '${scratch}/pending/card0-HDMI-A-1/enabled'
printf '3840x2160\\n' > '${scratch}/pending/card0-HDMI-A-1/modes'
mv '${scratch}/pending/card0-HDMI-A-1' '${scratch}/drm/card0-HDMI-A-1'
")

# An explicit walk that comes back to both maps: the second visit to section 0
# is at about 5.5 s and to section 1 at about 6.6 s, comfortably past the
# four-second sweep. The walk's timers do not depend on how fast this machine
# is -- the clock is stopped while each step is set, grabbed and dumped (entry
# 105) -- so those two figures are 1.1 s per step and nothing else.
include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

execute_process(
    COMMAND sh -c "sh '${scratch}/plug.sh' & exec '${CONFIG_BINARY}'"
    RESULT_VARIABLE status
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "VOCEM_DRM_ROOT=set:${scratch}/drm"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:0,1,2,3,0,1"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")
vocem_window_ran("${status}" "${errors}")
if(NOT EXISTS "${scratch}/drm/card0-HDMI-A-1/modes")
    message(FATAL_ERROR "the fixture never plugged its display in -- the run was too short")
endif()

# Every rectangle a named map reported while it was on screen, in the order the
# dump was written, as an aspect in thousandths. Both maps: the panel's and the
# message's, because one page following the machine while the other does not is
# the same defect half fixed.
#
# Visible only. The same item is dumped on every step, laid out off-screen,
# and a map nobody was looking at is not what this is a claim about.
function(map_aspects dump name out)
    set(found "")
    string(REGEX MATCHALL
           "\"item\": \"[^\"]*${name}\", \"x\": [0-9.e+-]+, \"y\": [0-9.e+-]+, \"w\": [0-9.]+, \"h\": [0-9.]+, \"visible\": true"
           lines "${dump}")
    foreach(line IN LISTS lines)
        string(REGEX MATCH "\"w\": ([0-9.]+), \"h\": ([0-9.]+)" ignored "${line}")
        set(width "${CMAKE_MATCH_1}")
        set(height "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "\\..*" "" width "${width}")
        string(REGEX REPLACE "\\..*" "" height "${height}")
        if(height GREATER 0 AND width GREATER 0)
            math(EXPR aspect "${width} * 1000 / ${height}")
            list(APPEND found "${aspect}")
        endif()
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

file(READ "${scratch}/geometry.json" dump)
map_aspects("${dump}" "positionStage" panel_aspects)
map_aspects("${dump}" "cornerStage" message_aspects)

# It must never pass for the reason preview_display.cmake refuses to: a dump
# with no map in it reports nothing, and two empty sets compare equal. Two
# readings each, because the whole measurement is a before and an after: one
# reading means the walk never came back to the page and the last figure is
# also the first.
list(LENGTH panel_aspects panel_seen)
list(LENGTH message_aspects message_seen)
if(panel_seen LESS 2 OR message_seen LESS 2)
    message(FATAL_ERROR
        "the walk did not show each map twice (panel ${panel_seen}, message "
        "${message_seen} visible readings) -- there is no before and after here, "
        "and an absent measurement is not agreement")
endif()

# The run has to have exercised the defect. If the very first reading is already
# the taller display's shape, the connector arrived before the window read the
# tree and this run proves nothing about remembering -- so it says so rather
# than passing.
list(GET panel_aspects 0 first_panel)
if(first_panel GREATER 1500)
    message(FATAL_ERROR
        "the first map was already ${first_panel}/1000 -- the second connector "
        "landed before the window's first read, so this run never held anything "
        "to the 5:4 tree it was born into")
endif()

# And the last reading, after the sweep, is the display that was plugged in.
function(ends_on_the_new_display which aspects)
    list(GET aspects -1 last)
    if(last LESS 1750 OR last GREATER 1806)
        string(REPLACE ";" " " walk "${aspects}")
        message(FATAL_ERROR
            "the ${which} map ends the run at ${last}/1000 and the display "
            "plugged in while the window was open is 3840x2160, 1778/1000. The "
            "map is still drawing the 1280x1024 display that was there when the "
            "window opened, which is what the overlay stopped being sized for "
            "within a minute of the cable going in. Aspects across the run: "
            "${walk}")
    endif()
endfunction()
ends_on_the_new_display("panel" "${panel_aspects}")
ends_on_the_new_display("message" "${message_aspects}")

# ---- the control: both displays there before the window starts.
#
# What makes this a defect of the remembering and not of the reader -- and the
# one run in which the map is measured while its page is actually shown.
set(control "${CMAKE_CURRENT_BINARY_DIR}/display-change-control")
file(REMOVE_RECURSE "${control}")
file(MAKE_DIRECTORY "${control}/config/vocem")
file(MAKE_DIRECTORY "${control}/cache/vocem")
foreach(connector "card0-DP-1;1280x1024" "card0-HDMI-A-1;3840x2160")
    list(GET connector 0 name)
    list(GET connector 1 mode)
    file(MAKE_DIRECTORY "${control}/drm/${name}")
    file(WRITE "${control}/drm/${name}/status" "connected\n")
    file(WRITE "${control}/drm/${name}/enabled" "enabled\n")
    file(WRITE "${control}/drm/${name}/modes" "${mode}\n")
endforeach()
file(WRITE "${control}/config/vocem/config.ini" "")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE control_status
    ERROR_VARIABLE control_errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${control}/config"
        "XDG_CACHE_HOME=set:${control}/cache"
        "VOCEM_DRM_ROOT=set:${control}/drm"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${control}/geometry.json")
if(NOT control_status EQUAL 0)
    message(FATAL_ERROR "the window failed on the control run: ${control_status} ${control_errors}")
endif()
file(READ "${control}/geometry.json" control_dump)
if(NOT control_dump MATCHES "\"item\": \"[^\"]*positionStage\", \"x\": [0-9.e+-]+, \"y\": [0-9.e+-]+, \"w\": ([0-9.]+), \"h\": ([0-9.]+)[^\n]*\"visible\": true")
    message(FATAL_ERROR "no visible panel map in the control dump")
endif()
set(control_w "${CMAKE_MATCH_1}")
set(control_h "${CMAKE_MATCH_2}")
string(REGEX REPLACE "\\..*" "" control_w "${control_w}")
string(REGEX REPLACE "\\..*" "" control_h "${control_h}")
if(control_h LESS 1)
    message(FATAL_ERROR "the control map has no height")
endif()
math(EXPR control_aspect "${control_w} * 1000 / ${control_h}")
if(control_aspect LESS 1750 OR control_aspect GREATER 1806)
    message(FATAL_ERROR
        "with both displays present before it started, the visible map is "
        "${control_aspect}/1000 and not the taller display's 1778/1000 -- the "
        "reader is wrong, and the run above was measuring something else")
endif()

# ---- and a display chosen by hand is not dragged off by the sweep.
#
# The price of an enumeration that can change while the window is open. A
# ComboBox puts currentIndex back to 0 when its model is replaced (entry 104),
# so the picker rebuilds its options only when the enumeration actually differs
# -- values and labels, not their count, since a display switched to another
# mode changes only the resolution written in its label. Get that comparison
# wrong in the direction of "always different" and the map would snap back to
# Automatic under the hand that set it, twice a second. Both connectors are
# present from the start here, so nothing about the machine changes: the map
# must still be depicting the 5:4 display it was told to, four seconds and eight
# ticks later.
file(WRITE "${control}/config/vocem/config.ini" "preview_display_panel = DP-1\n")
execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE chosen_status
    ERROR_VARIABLE chosen_errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${control}/config"
        "XDG_CACHE_HOME=set:${control}/cache"
        "VOCEM_DRM_ROOT=set:${control}/drm"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:0,1,2,3,0"
        "VOCEM_CONFIG_GEOMETRY=set:${control}/chosen.json")
if(NOT chosen_status EQUAL 0)
    message(FATAL_ERROR "the window failed with a display chosen: ${chosen_status} ${chosen_errors}")
endif()
file(READ "${control}/chosen.json" chosen_dump)
map_aspects("${chosen_dump}" "positionStage" chosen_aspects)
list(LENGTH chosen_aspects chosen_seen)
if(chosen_seen LESS 2)
    message(FATAL_ERROR
        "the walk did not show the panel map twice with a display chosen "
        "(${chosen_seen} visible readings)")
endif()
list(GET chosen_aspects -1 chosen_last)
if(chosen_last LESS 1200 OR chosen_last GREATER 1300)
    string(REPLACE ";" " " chosen_walk "${chosen_aspects}")
    message(FATAL_ERROR
        "with DP-1 chosen and nothing about the machine changing, the map ends "
        "the run at ${chosen_last}/1000 instead of the 1280x1024 display's "
        "1250/1000 -- the sweep took the selection away from whoever set it "
        "(aspects: ${chosen_walk})")
endif()

string(REPLACE ";" " " panel_walk "${panel_aspects}")
string(REPLACE ";" " " message_walk "${message_aspects}")
message(STATUS
    "a display plugged in while the window was open reaches both maps: panel "
    "${panel_walk}, message ${message_walk} (thousandths, 1250 = the 5:4 it "
    "opened on, 1778 = the 3840x2160 that arrived); the control with both "
    "present at startup draws ${control_aspect}/1000, and a chosen display "
    "still reads ${chosen_last}/1000 after the sweep")


# --- the same monitor arriving, with a display pinned ------------------------
#
# A second run, because the two claims need opposite fixtures: the run above
# asks what the map does when nothing is chosen (it must follow the machine),
# and this one asks what the DROPDOWN does when something is (it must not move).
# Pinning a display in the first run would have made its own assertion false --
# the map then depicts the pinned display, correctly -- which is exactly why
# every fixture in this suite left the dropdown at its default and nothing ever
# exercised the guard.
set(pinned "${CMAKE_CURRENT_BINARY_DIR}/display-change-pinned")
file(REMOVE_RECURSE "${pinned}")
file(MAKE_DIRECTORY "${pinned}/config/vocem")
file(MAKE_DIRECTORY "${pinned}/cache/vocem")
file(MAKE_DIRECTORY "${pinned}/drm/card0-DP-1")
file(WRITE "${pinned}/drm/card0-DP-1/status" "connected\n")
file(WRITE "${pinned}/drm/card0-DP-1/enabled" "enabled\n")
file(WRITE "${pinned}/drm/card0-DP-1/modes" "1280x1024\n")
file(WRITE "${pinned}/config/vocem/config.ini" "preview_display_panel = DP-1\n")
file(WRITE "${pinned}/plug.sh"
"#!/bin/sh
sleep 1.6
mkdir -p '${pinned}/pending/card0-HDMI-A-1'
printf 'connected\\n' > '${pinned}/pending/card0-HDMI-A-1/status'
printf 'enabled\\n'   > '${pinned}/pending/card0-HDMI-A-1/enabled'
printf '3840x2160\\n' > '${pinned}/pending/card0-HDMI-A-1/modes'
mv '${pinned}/pending/card0-HDMI-A-1' '${pinned}/drm/card0-HDMI-A-1'
")

execute_process(
    COMMAND sh -c "sh '${pinned}/plug.sh' & exec '${CONFIG_BINARY}'"
    RESULT_VARIABLE pinned_status
    ERROR_VARIABLE pinned_errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${pinned}/config"
        "XDG_CACHE_HOME=set:${pinned}/cache"
        "VOCEM_DRM_ROOT=set:${pinned}/drm"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:0,1,2,3,0,1"
        "VOCEM_CONFIG_GEOMETRY=set:${pinned}/geometry.json")
vocem_window_ran("${pinned_status}" "${pinned_errors}")
if(NOT EXISTS "${pinned}/drm/card0-HDMI-A-1/modes")
    message(FATAL_ERROR "the pinned fixture never plugged its display in -- the run was too short")
endif()
file(READ "${pinned}/geometry.json" pinned_dump)


# What the "Map shows" dropdown was pointing at, every time it was on screen.
# A pin is an index, and an index is invisible to a rectangle: DisplayPicker
# publishes pickerIndex and pickerCount into the dump for exactly this check.
function(picker_states dump out_indexes out_counts)
    set(indexes "")
    set(counts "")
    string(REGEX MATCHALL
           "\"item\": \"[^\"]*panelDisplayPicker/displayPicker\"[^\n]*\"visible\": true[^\n]*"
           lines "${dump}")
    foreach(line IN LISTS lines)
        if(line MATCHES "\"pickerIndex\": ([0-9]+)[^\n]*\"pickerCount\": ([0-9]+)")
            list(APPEND indexes "${CMAKE_MATCH_1}")
            list(APPEND counts "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    set(${out_indexes} "${indexes}" PARENT_SCOPE)
    set(${out_counts} "${counts}" PARENT_SCOPE)
endfunction()

picker_states("${pinned_dump}" picker_indexes picker_counts)
list(LENGTH picker_indexes picker_seen)
if(picker_seen LESS 2)
    message(FATAL_ERROR
        "the dropdown was read ${picker_seen} time(s), so there is no before and after: either "
        "the walk did not come back to the page or DisplayPicker stopped publishing "
        "pickerIndex, and an absent measurement is not agreement")
endif()
list(GET picker_indexes 0 picker_first)
list(GET picker_counts 0 count_first)
list(GET picker_indexes -1 picker_last)
list(GET picker_counts -1 count_last)
message("     Map shows: index ${picker_first} of ${count_first} rows before, "
        "${picker_last} of ${count_last} after")
if(NOT count_last GREATER count_first)
    message(FATAL_ERROR
        "the dropdown offered ${count_first} rows before and ${count_last} after: the "
        "enumeration never reached it, so what follows would measure nothing")
endif()
if(NOT picker_first EQUAL 1 OR NOT picker_last EQUAL 1)
    message(FATAL_ERROR
        "the pinned display was lost when the enumeration changed: the dropdown pointed at row "
        "${picker_first} before the monitor arrived and row ${picker_last} after, where "
        "config.ini says DP-1 throughout. A ComboBox resets currentIndex to 0 when its model "
        "is replaced (entry 104), so a live binding on config.displays drags the pin to "
        "\"Automatic\" while the map underneath goes on drawing the pinned display -- and "
        "re-picking the row it is showing then writes \"\" and loses the pin for good.")
endif()
message("ok   a monitor arriving does not drag the pinned display back to Automatic")
