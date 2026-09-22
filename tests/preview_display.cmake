# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A map depicting a smaller display shows the overlay at the share of THAT
# screen it will really cover, and the dropdown that chooses the display exists.
#
# The overlay sizes itself from the largest connected display's mode height
# (vocem/display.h; the daemon publishes it). On a smaller display the same
# pixels cover a larger share -- by exactly largest/this -- and the window's
# maps used to normalise that away silently: every depicted screen showed the
# same proportions, which is only true of one of them.
#
# Fabricated displays, never the machine's: a two-display DRM tree under the
# VOCEM_DRM_ROOT override (the same parameterised-root idea display.h's own
# test uses), a 3840x2160 output and a 1920x1080 one. The real window runs
# offscreen twice over section 0 -- once automatic, once with the smaller
# display chosen for the panel map through the persisted key -- and the panel's
# share of the depicted screen's height must grow by largest/this = 2160/1080
# = 2.0 between the runs. Height rather than width, because the share of the
# height is independent of the stage's aspect and the automatic run's aspect
# comes from the platform plugin's idea of a screen.
#
# Against the window before the dropdown existed (the packaged 0.1.0-59
# exemplar) both halves fail: the key is ignored, so the ratio is 1.0, and no
# panelDisplayPicker appears in the dump.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/preview-display")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")

# The fabricated tree: connectors are card<N>-<name> with status, enabled and
# modes, exactly the files both readers open. The largest is 2160 lines; the
# depicted one 1080.
foreach(connector "card0-DP-1;3840x2160" "card0-HDMI-A-1;1920x1080")
    list(GET connector 0 name)
    list(GET connector 1 mode)
    file(MAKE_DIRECTORY "${scratch}/drm/${name}")
    file(WRITE "${scratch}/drm/${name}/status" "connected\n")
    file(WRITE "${scratch}/drm/${name}/enabled" "enabled\n")
    file(WRITE "${scratch}/drm/${name}/modes" "${mode}\n")
endforeach()
# An entry that is not a connector, which both readers must skip.
file(MAKE_DIRECTORY "${scratch}/drm/renderD128")

# One offscreen run of the real window over section 0 (the Panel page), with
# the given config, dumping geometry to the given file.
function(run_window config_text dump)
    file(WRITE "${scratch}/config/vocem/config.ini" "${config_text}")
include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${scratch}/config"
            "XDG_CACHE_HOME=set:${scratch}/cache"
            "VOCEM_DRM_ROOT=set:${scratch}/drm"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_SECTIONS=set:0"
            "VOCEM_CONFIG_STEP_MS=set:0"
            "VOCEM_CONFIG_GEOMETRY=set:${dump}")
    set(status "${status}" PARENT_SCOPE)
    set(errors "${errors}" PARENT_SCOPE)
endfunction()

# One item's height out of a dump, in thousandths so the arithmetic below can
# stay integer. The name must end the item string, so children do not match.
function(item_height dump_text name out)
    if(NOT dump_text MATCHES "\"item\": \"[^\"]*${name}\"[^\n]*")
        message(FATAL_ERROR "no ${name} in the dump")
    endif()
    set(line "${CMAKE_MATCH_0}")
    if(NOT line MATCHES "\"visible\": true")
        message(FATAL_ERROR "${name} is not visible: ${line}")
    endif()
    if(NOT line MATCHES "\"h\": ([0-9]+)(\\.([0-9]+))?")
        message(FATAL_ERROR "${name} carries no plain height: ${line}")
    endif()
    set(whole "${CMAKE_MATCH_1}")
    set(fraction "${CMAKE_MATCH_3}000")
    string(SUBSTRING "${fraction}" 0 3 fraction)
    math(EXPR millis "${whole} * 1000 + ${fraction}")
    set(${out} "${millis}" PARENT_SCOPE)
endfunction()

# ---- the automatic run: the map as it has always been.
run_window("" "${scratch}/geometry-auto.json")
vocem_window_ran("${status}" "${errors}")
file(READ "${scratch}/geometry-auto.json" auto_dump)
item_height("${auto_dump}" "positionStage" stage_auto)
item_height("${auto_dump}" "positionStage/panel" panel_auto)

# ---- the run depicting the smaller display, chosen through the persisted key.
run_window("preview_display_panel = HDMI-A-1\n" "${scratch}/geometry-selected.json")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the window failed with the display chosen: ${status} ${errors}")
endif()
file(READ "${scratch}/geometry-selected.json" selected_dump)
item_height("${selected_dump}" "positionStage" stage_selected)
item_height("${selected_dump}" "positionStage/panel" panel_selected)

# ---- the dropdown exists on the page, with two displays to choose between.
if(NOT selected_dump MATCHES "\"item\": \"[^\"]*panelDisplayPicker\"[^\n]*\"visible\": true")
    message(FATAL_ERROR
        "no visible panelDisplayPicker in the dump -- with two connected "
        "displays the Panel page offers no way to choose which one the map "
        "depicts")
endif()

# ---- the honesty claim: the panel's share of the depicted screen's height
# grew by largest/this = 2160/1080 = 2.0. Cross-multiplied so the arithmetic
# stays integer: share_sel / share_auto = (panel_sel * stage_auto) /
# (stage_sel * panel_auto), asserted within 5% of 2.0.
if(panel_auto EQUAL 0 OR stage_auto EQUAL 0 OR stage_selected EQUAL 0)
    message(FATAL_ERROR "a measured height is zero: panel ${panel_auto}, "
                        "stage ${stage_auto}/${stage_selected}")
endif()
math(EXPR lhs "${panel_selected} * ${stage_auto}")
math(EXPR rhs "${stage_selected} * ${panel_auto}")
math(EXPR ratio_millis "(${lhs} * 1000) / ${rhs}")
# 1.95 <= lhs/rhs <= 2.05, cross-multiplied: 195*rhs <= 100*lhs <= 205*rhs.
math(EXPR low "${rhs} * 195")
math(EXPR high "${rhs} * 205")
math(EXPR lhs_scaled "${lhs} * 100")
if(lhs_scaled LESS low OR lhs_scaled GREATER high)
    message(FATAL_ERROR
        "the panel's share of the depicted 1080-line display grew by "
        "${ratio_millis}/1000 instead of the expected 2.000 (largest 2160 / "
        "depicted 1080) -- the map still normalises the overlay's size away "
        "(panel h ${panel_auto} -> ${panel_selected} millipixels, stage h "
        "${stage_auto} -> ${stage_selected})")
endif()

# And the picker is on the page with ONE display too -- the owner's ask. It
# was hidden below two connectors, which took away the caption saying which
# display the map stands for and at what resolution on the machine where that
# is the only thing the row has to say.
set(single "${CMAKE_CURRENT_BINARY_DIR}/preview-display-one")
file(REMOVE_RECURSE "${single}")
file(MAKE_DIRECTORY "${single}/config/vocem")
file(MAKE_DIRECTORY "${single}/cache/vocem")
file(MAKE_DIRECTORY "${single}/drm/card0-DP-1")
file(WRITE "${single}/drm/card0-DP-1/status" "connected\n")
file(WRITE "${single}/drm/card0-DP-1/enabled" "enabled\n")
file(WRITE "${single}/drm/card0-DP-1/modes" "3840x2160\n")
file(WRITE "${single}/config/vocem/config.ini" "")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE single_status
    OUTPUT_VARIABLE single_output
    ERROR_VARIABLE single_errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${single}/config"
        "XDG_CACHE_HOME=set:${single}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_DRM_ROOT=set:${single}/drm"
        "VOCEM_CONFIG_SECTIONS=set:0"
        "VOCEM_CONFIG_STEP_MS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${single}/geometry.json")

if(NOT single_status EQUAL 0)
    message(FATAL_ERROR "the window could not run with one display: ${single_errors}")
endif()
file(READ "${single}/geometry.json" single_dump)
string(REGEX MATCHALL "\"item\": \"[^\"]*panelDisplayPicker[^\"]*\"[^\n]*\"visible\": true"
       single_picker "${single_dump}")
if(single_picker STREQUAL "")
    message(FATAL_ERROR
        "no visible panelDisplayPicker with a single display -- the row also "
        "says which display the map stands for and at what resolution, which "
        "is exactly what a one-display machine needs it to say")
endif()

# ---- the map with nothing chosen has the shape of the display it stands for.
#
# It used to take its aspect from `Screen`: the screen this window happens to
# be on, which is a different display from the one the caption names on any
# machine with two -- and which is not a display shape at all under the
# offscreen plugin, where every map in every run of this suite came out square
# while its caption said 3840 x 2160. The single-display run above has one
# 3840x2160 connector, so the stage is 16:9 or the map is not a map of it.
if(NOT single_dump MATCHES "\"item\": \"[^\"]*positionStage\", \"x\": [0-9.e+-]+, \"y\": [0-9.e+-]+, \"w\": ([0-9.]+), \"h\": ([0-9.]+)")
    message(FATAL_ERROR "no positionStage rectangle in the single-display dump")
endif()
# Both out of the match before anything else matches over them -- a second
# regex call replaces CMAKE_MATCH_*, and the height came out empty.
set(stage_w "${CMAKE_MATCH_1}")
set(stage_h "${CMAKE_MATCH_2}")
string(REGEX REPLACE "\\..*" "" stage_w_whole "${stage_w}")
string(REGEX REPLACE "\\..*" "" stage_h_whole "${stage_h}")
if(stage_h_whole LESS 1)
    message(FATAL_ERROR "the map has no height: ${CMAKE_MATCH_2}")
endif()
math(EXPR stage_aspect_millis "${stage_w_whole} * 1000 / ${stage_h_whole}")
if(stage_aspect_millis LESS 1750 OR stage_aspect_millis GREATER 1806)
    message(FATAL_ERROR
        "with one 3840x2160 display the map is ${stage_w_whole}x${stage_h_whole} "
        "(aspect ${stage_aspect_millis}/1000), not the display's 1778/1000 -- the "
        "map with nothing chosen is not shaped like the display it stands for")
endif()

# ---- a horizontal panel is capped by the display, not by 520 units.
#
# panel.cpp holds a column to 520 units so one pathological display name
# cannot cross the screen, and says in as many words that this is "not its cap"
# for a row: what stops a row is the display, which is what its own budget
# counted against. The preview applied the 520 to both, so a horizontal panel
# was drawn ending at the third person while the game drew all four. Measured
# at the settings below on 1920x1080: the overlay 780 units wide, the preview
# 520 -- 27% of the display shown against 40% drawn.
#
# Both sides, in the overlay's own unit. The overlay's half is the same probe
# scripts/compare-preview.py drives (it sits beside this script in the test
# tree); without it there is nothing to compare against and the check says so
# rather than asserting a number nobody re-measures.
set(geometry_probe "${CMAKE_CURRENT_BINARY_DIR}/vocem_panel_geometry")
if(NOT EXISTS "${geometry_probe}")
    message(STATUS "skip the panel geometry probe was not built")
    return()
endif()

set(wide "${CMAKE_CURRENT_BINARY_DIR}/preview-display-wide")
file(REMOVE_RECURSE "${wide}")
file(MAKE_DIRECTORY "${wide}/config/vocem")
file(MAKE_DIRECTORY "${wide}/cache/vocem")
file(MAKE_DIRECTORY "${wide}/drm/card0-DP-1")
file(WRITE "${wide}/drm/card0-DP-1/status" "connected\n")
file(WRITE "${wide}/drm/card0-DP-1/enabled" "enabled\n")
file(WRITE "${wide}/drm/card0-DP-1/modes" "1920x1080\n")
# Sideways, with the pictures and the gaps wide enough that four people ask for
# more than 520 units. Every one of these is a setting the window offers.
set(wide_settings
    "panel_layout = horizontal"
    "avatar_size = 2.0"
    "avatar_gap = 48"
    "row_spacing = 48"
    "box_padding_x = 40")
string(REPLACE ";" "\n" wide_config "${wide_settings}")
file(WRITE "${wide}/config/vocem/config.ini" "${wide_config}\n")

execute_process(
    COMMAND "${geometry_probe}" width=1920 height=1080 users=4
            panel_layout=horizontal avatar_size=2.0 avatar_gap=48 row_spacing=48
            box_padding_x=40
    RESULT_VARIABLE probe_status
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_errors
    TIMEOUT 60)
if(NOT probe_status EQUAL 0)
    message(FATAL_ERROR "the panel geometry probe failed: ${probe_status} ${probe_errors}")
endif()
if(NOT probe_output MATCHES "\"panel\": \\{\"x\": [0-9.e+-]+, \"y\": [0-9.e+-]+, \"w\": ([0-9]+)")
    message(FATAL_ERROR "the probe printed no panel width: ${probe_output}")
endif()
set(overlay_width "${CMAKE_MATCH_1}")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE wide_status
    ERROR_VARIABLE wide_errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${wide}/config"
        "XDG_CACHE_HOME=set:${wide}/cache"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_DRM_ROOT=set:${wide}/drm"
        "VOCEM_CONFIG_SECTIONS=set:0"
        "VOCEM_CONFIG_STEP_MS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${wide}/geometry.json")
if(NOT wide_status EQUAL 0)
    message(FATAL_ERROR "the window failed on the horizontal panel: ${wide_status} ${wide_errors}")
endif()
file(READ "${wide}/geometry.json" wide_dump)

# The preview's box in overlay units: its rectangle divided by the factor the
# view drew it at, both of which the dump prints for this item.
if(NOT wide_dump MATCHES "\"item\": \"[^\"]*positionStage/panel\", \"x\": [0-9.e+-]+, \"y\": [0-9.e+-]+, \"w\": ([0-9.]+)[^\n]*\"factor\": ([0-9.]+)")
    message(FATAL_ERROR "no panel box with a factor in the horizontal dump")
endif()
set(preview_pixels "${CMAKE_MATCH_1}")
set(preview_factor "${CMAKE_MATCH_2}")
# CMake's arithmetic is integer, so both are taken to thousandths first and the
# division of one by the other is back in whole overlay units.
function(to_millis number out)
    if(NOT number MATCHES "^([0-9]+)(\\.([0-9]+))?$")
        message(FATAL_ERROR "not a plain number: ${number}")
    endif()
    set(fraction "${CMAKE_MATCH_3}000")
    string(SUBSTRING "${fraction}" 0 3 fraction)
    math(EXPR millis "${CMAKE_MATCH_1} * 1000 + ${fraction}")
    set(${out} "${millis}" PARENT_SCOPE)
endfunction()
to_millis("${preview_pixels}" pixels_millis)
to_millis("${preview_factor}" factor_millis)
if(factor_millis LESS 1)
    message(FATAL_ERROR "the preview drew the panel at a scale of ${preview_factor}")
endif()
math(EXPR preview_units "${pixels_millis} / ${factor_millis}")
if(preview_units LESS 700 OR preview_units GREATER 860)
    message(FATAL_ERROR
        "the horizontal panel preview is ${preview_units} overlay units wide "
        "where the overlay draws ${overlay_width} -- a row is capped by the "
        "display it is counted against, not by the column's 520 units "
        "(panel.cpp says so where it sets the constraint)")
endif()

message(STATUS
    "depicting the smaller display grows the panel's share by "
    "${ratio_millis}/1000 (expected 2.000), the dropdown is on the page with "
    "two displays and with one, the automatic map is the display's own shape "
    "(${stage_aspect_millis}/1000), and a horizontal panel previews "
    "${preview_units} units against the overlay's ${overlay_width}")
