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
if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
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

message(STATUS
    "depicting the smaller display grows the panel's share by "
    "${ratio_millis}/1000 (expected 2.000), and the dropdown is on the page "
    "with two displays and with one")
