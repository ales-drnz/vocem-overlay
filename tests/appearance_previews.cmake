# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The Appearance page previews its presets, and the editing pages preview their
# edits -- measured out of the window's own geometry dump.
#
# Two claims, both about what the window actually builds rather than what its
# QML promises:
#
#   1. The preset row is four real previews (objectName presetPreview), each
#      drawn from the theme its preset would produce -- so each carries a
#      distinct surface (the dump prints surfaceRgb and presetOpacity for
#      exactly this check) -- and exactly one of them shows the active mark
#      (objectName presetActiveMark): the defaults are the transparent preset.
#
#   2. Appearance and Spacing each carry a live preview of both boxes beside
#      the controls: appearanceLive/panel, appearanceLive/message,
#      spacingLive/panel and spacingLive/message, visible on their own pages.
#
# One offscreen run of the real window over sections 0..3, scratch XDG dirs
# (never the real ones -- a real cache's crash journals would pop a window over
# the page). Against the window before these previews existed the preset row
# was a row of colour chips and neither page had a preview column, so every
# assertion below fails there.

if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/appearance-previews")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
# Defaults: the default surface and opacity are the transparent preset's, so the
# active mark has exactly one honest place to be.
file(WRITE "${scratch}/config/vocem/config.ini" "")

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
        "VOCEM_CONFIG_SECTIONS=set:3"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

# ---- the four preset previews, each with a distinct surface.
# The wrapper item's own line ends the name at presetPreview(#N), so the match
# does not count the previews' children.
string(REGEX MATCHALL "\"item\": \"[^\"]*presetPreview(#[0-9]+)?\"[^\n]*\"visible\": true[^\n]*"
       preset_lines "${dump}")
list(LENGTH preset_lines preset_count)
if(NOT preset_count EQUAL 4)
    message(FATAL_ERROR
        "expected 4 visible preset previews on the Appearance page, found "
        "${preset_count} -- the preset row does not preview its presets")
endif()

set(surfaces "")
foreach(line IN LISTS preset_lines)
    if(NOT line MATCHES "\"surfaceRgb\": ([0-9.e+-]+)")
        message(FATAL_ERROR "a preset preview carries no surfaceRgb: ${line}")
    endif()
    set(rgb "${CMAKE_MATCH_1}")
    if(NOT line MATCHES "\"presetOpacity\": ([0-9.e+-]+)")
        message(FATAL_ERROR "a preset preview carries no presetOpacity: ${line}")
    endif()
    list(APPEND surfaces "${rgb}/${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES surfaces)
list(LENGTH surfaces distinct)
if(NOT distinct EQUAL 4)
    message(FATAL_ERROR
        "the four preset previews carry only ${distinct} distinct surfaces "
        "(${surfaces}) -- two presets are being previewed with the same theme")
endif()

# ---- exactly one active mark: the defaults are one preset, not two.
string(REGEX MATCHALL "\"item\": \"[^\"]*presetActiveMark(#[0-9]+)?\"[^\n]*\"visible\": true"
       active_lines "${dump}")
list(LENGTH active_lines active_count)
if(NOT active_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly 1 visible preset active mark at the defaults, found "
        "${active_count}")
endif()

# ---- the live previews: both boxes, on both pages.
foreach(name "appearanceLive/panel" "appearanceLive/message"
             "spacingLive/panel" "spacingLive/message")
    if(NOT dump MATCHES "\"item\": \"[^\"]*${name}\"[^\n]*\"visible\": true")
        message(FATAL_ERROR
            "no visible ${name} in the dump -- the page lost its live preview")
    endif()
endforeach()

message(STATUS
    "four preset previews with distinct surfaces, one active mark, and both "
    "live previews carry both boxes")
