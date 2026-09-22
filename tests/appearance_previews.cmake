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
#   1. The preset row is five real previews (objectName presetPreview), each
#      drawn from the theme its preset would produce -- so each carries a
#      distinct picture (the dump prints surfaceRgb, presetOpacity and presetBox
#      for exactly this check; two presets share a surface and an opacity and are
#      told apart by where that surface is drawn) -- and exactly one of them
#      shows the active mark (objectName presetActiveMark): the defaults are the
#      pills preset.
#
#   2. Appearance and Spacing each carry a live preview of both boxes beside
#      the controls: appearanceLive/panel, appearanceLive/message,
#      spacingLive/panel and spacingLive/message, visible on their own pages.
#
# One offscreen run of the real window over sections 2 and 3, scratch XDG dirs
# (never the real ones -- a real cache's crash journals would pop a window over
# the page). Against the window before these previews existed the preset row
# was a row of colour chips and neither page had a preview column, so every
# assertion below fails there.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/appearance-previews")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
# Defaults: the default surface, opacity and box are the pills preset's, so the
# active mark has exactly one honest place to be.
file(WRITE "${scratch}/config/vocem/config.ini" "")

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

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
        "VOCEM_CONFIG_SECTIONS=set:2,3"
        "VOCEM_CONFIG_STEP_MS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

vocem_window_ran("${status}" "${errors}")
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
if(NOT preset_count EQUAL 5)
    message(FATAL_ERROR
        "expected 5 visible preset previews on the Appearance page, found "
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
    set(preset_opacity "${CMAKE_MATCH_1}")
    if(NOT line MATCHES "\"presetBox\": ([0-9.e+-]+)")
        message(FATAL_ERROR "a preset preview carries no presetBox: ${line}")
    endif()
    list(APPEND surfaces "${rgb}/${preset_opacity}/${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES surfaces)
list(LENGTH surfaces distinct)
if(NOT distinct EQUAL 5)
    message(FATAL_ERROR
        "the five preset previews carry only ${distinct} distinct pictures "
        "(${surfaces}) -- two presets are being previewed the same way")
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

# ---- the font menu previews the fonts, and the built-in row previews the
# built-in font.
#
# Every row of that menu is drawn in the face it names, which is the whole
# reason the menu is spelled out rather than left to the style -- and the row
# that offers to go back to the carried Inter has to be drawn in Inter, not in
# the family the user is about to leave. `builtInFontFamily` exists for exactly
# that and had no test: a menu is in no screenshot and in no geometry dump,
# because nothing in a harness clicks one open.
#
# So the window is told to open it (VOCEM_CONFIG_OPEN, gui/src/main.cpp) and
# each row's Label carries a name, whose *implicitWidth* is the one number that
# says which face it was drawn in -- the row's own width is the list's. Two runs
# that differ only in the family chosen must produce the same set of row widths:
# a row is drawn in the family it names, and none of them names the chosen one.
# Against the code before builtInFontFamily (the built-in row drawn through
# overlayFont(), whose first family is the chosen one) the two sets differ by
# that row alone -- measured here, 75.48 against 115.00 for "Built-in (Inter)"
# with Adwaita Mono chosen.
#
# Adwaita Mono because it is early in the list -- the menu opens centred on what
# is set, and a family far down it would scroll the built-in row out of the
# rows the dump can see.
function(font_menu_row_widths family out)
    set(scratch "${CMAKE_CURRENT_BINARY_DIR}/appearance-font-menu")
    file(REMOVE_RECURSE "${scratch}")
    file(MAKE_DIRECTORY "${scratch}/config/vocem")
    file(MAKE_DIRECTORY "${scratch}/cache/vocem")
    file(WRITE "${scratch}/config/vocem/config.ini" "font_family = ${family}\n")
    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${scratch}/config"
            "XDG_CACHE_HOME=set:${scratch}/cache"
            "QT_QPA_PLATFORM=set:offscreen"
            "LC_ALL=set:C.UTF-8"
            "VOCEM_CONFIG_SECTIONS=set:2,"
            "VOCEM_CONFIG_STEP_MS=set:0"
            "VOCEM_CONFIG_OPEN=set:fontFamilyChoice"
            "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "the window failed with font_family=${family}: ${errors}")
    endif()
    file(READ "${scratch}/geometry.json" menu_dump)
    # Where the menu itself is, so that the rows a recycling view keeps ready
    # above the top of it are not counted as rows anybody can see.
    if(NOT menu_dump MATCHES "\"item\": \"[^\"]*fontList\", \"x\": [0-9.e+-]+, \"y\": ([0-9.]+)")
        message(FATAL_ERROR
            "no fontList in the dump for font_family=${family} -- the harness "
            "did not open the font menu, and everything below would be measuring "
            "an empty list")
    endif()
    string(REGEX REPLACE "\\..*" "" list_top "${CMAKE_MATCH_1}")

    string(REGEX MATCHALL
           "\"item\": \"[^\"]*fontRowName[^\"]*\", \"x\": [0-9.e+-]+, \"y\": [0-9.]+[^\n]*\"implicitWidth\": [0-9.]+"
           rows "${menu_dump}")
    # y first, so a natural sort puts the top row of the menu at the head. Only
    # the first row is compared between runs: it is the built-in entry (the menu
    # opens centred on what is set, and both families used here are at the very
    # top of the list, where centring clamps to the top), and it is the one row
    # whose face the defect changed.
    set(pairs "")
    foreach(row IN LISTS rows)
        if(row MATCHES "\"y\": ([0-9.]+)[^\n]*\"implicitWidth\": ([0-9.]+)")
            set(row_y "${CMAKE_MATCH_1}")
            set(row_width "${CMAKE_MATCH_2}")
            string(REGEX REPLACE "\\..*" "" row_y_whole "${row_y}")
            if(NOT row_y_whole LESS list_top)
                list(APPEND pairs "${row_y_whole} ${row_width}")
            endif()
        endif()
    endforeach()
    list(SORT pairs COMPARE NATURAL)
    set(${out} "${pairs}" PARENT_SCOPE)
endfunction()

font_menu_row_widths("" built_in_rows)
font_menu_row_widths("Adwaita Mono" chosen_rows)
list(LENGTH built_in_rows row_count)
# A menu that did not open reports no rows, and two empty lists are equal: the
# check would pass for the one reason it must never pass for.
if(row_count LESS 5)
    message(FATAL_ERROR
        "the font menu shows ${row_count} rows inside the list with the popup "
        "opened -- either the harness did not open it or the list is empty, and "
        "either way nothing below is measuring the faces it claims to")
endif()
list(GET built_in_rows 0 built_in_top)
list(GET chosen_rows 0 chosen_top)
string(REGEX REPLACE "^[0-9]+ " "" built_in_top_width "${built_in_top}")
string(REGEX REPLACE "^[0-9]+ " "" chosen_top_width "${chosen_top}")
if(NOT built_in_top_width STREQUAL chosen_top_width)
    message(FATAL_ERROR
        "the built-in row of the font menu is ${built_in_top_width} units wide "
        "with nothing chosen and ${chosen_top_width} with Adwaita Mono chosen -- "
        "the row that offers the carried Inter is drawn in the family the user "
        "is about to leave (ConfigBridge::builtInFontFamily is what stops that)")
endif()

message(STATUS
    "five preset previews with distinct pictures, one active mark, both live "
    "previews carry both boxes, and the font menu's built-in row keeps the "
    "built-in face (${built_in_top_width}) when a family is chosen")
