# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A slider's bounds are the settings file's bounds, and they are spelled twice:
# config.h clamps what is read, the page says what can be dragged. Nothing held
# the two together, and one pair had drifted: "Show for" offered 1-20 over a
# clamp of 1-30, so a legitimate 30 in the file showed as a slider pinned at 20
# that rewrote the setting the moment it was touched -- the window silently
# narrowing what the file is allowed to say. This walks every SliderRow/SpinRow
# bound to a config value and holds its from/to to the clamp of the snake_case
# key with the same name; a percentage control (value * 100) is held to the
# clamp times one hundred. Values are compared in integer thousandths, because
# CMake's EQUAL is not a float comparison.
#
# The count is asserted too: a walk that pairs nothing must fail, not pass on
# an empty set (entry 77's shape -- a test that can pass for a reason other
# than the one it names).

if(NOT SOURCE_DIR)
    get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# "0.5" -> 500, "120" -> 120000, "3.0" -> 3000. Integer thousandths, so that
# math(EXPR) can scale and compare exactly.
function(to_thousandths value out)
    if(value MATCHES "^([0-9]+)\\.([0-9]+)$")
        set(whole "${CMAKE_MATCH_1}")
        set(frac "${CMAKE_MATCH_2}000")
        string(SUBSTRING "${frac}" 0 3 frac)
        math(EXPR result "${whole} * 1000 + ${frac}")
    elseif(value MATCHES "^[0-9]+$")
        math(EXPR result "${value} * 1000")
    else()
        message(FATAL_ERROR "slider_bounds: cannot read '${value}' as a number")
    endif()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The clamps, from the one place the file's bounds are stated. Whole-file
# regex, not file(STRINGS): the source is full of semicolons, which CMake
# treats as list separators and mangles a per-line walk with.
file(READ "${SOURCE_DIR}/include/vocem/config.h" config_text)
string(REGEX MATCHALL "[a-z_]+ = clamp\\(to_number\\(value\\), [0-9.]+f?, [0-9.]+f?\\)"
       clamp_hits "${config_text}")
set(clamp_count 0)
foreach(hit IN LISTS clamp_hits)
    if(hit MATCHES "([a-z_]+) = clamp\\(to_number\\(value\\), ([0-9.]+)f?, ([0-9.]+)f?\\)")
        set(key "${CMAKE_MATCH_1}")
        to_thousandths("${CMAKE_MATCH_2}" lo)
        to_thousandths("${CMAKE_MATCH_3}" hi)
        set(clamp_lo_${key} "${lo}")
        set(clamp_hi_${key} "${hi}")
        math(EXPR clamp_count "${clamp_count} + 1")
    endif()
endforeach()
if(clamp_count LESS 15)
    message(FATAL_ERROR "slider_bounds: only ${clamp_count} clamps read from config.h -- "
                        "the parse has stopped matching the source, which is not agreement")
endif()

# The rows. A block's from/to and the config binding that names the pair are
# taken from one stretch of text that cannot cross a closing brace, so a row
# without a config binding (an animation, a preview control) never borrows the
# next block's. Whole-file matching for the same semicolon reason as above --
# the from/to line itself contains one.
file(GLOB qml_files "${SOURCE_DIR}/gui/qml/*.qml")
set(paired 0)
set(mismatches "")
foreach(qml IN LISTS qml_files)
    file(READ "${qml}" qml_text)
    # Semicolons are CMake's list separator, and the from/to line contains one:
    # a match kept as a list element would be split apart at exactly the
    # character the pattern pivots on. Neutralised before matching.
    string(REPLACE ";" "," qml_text "${qml_text}")
    string(REGEX MATCHALL "from: [0-9.]+, to: [0-9.]+[^}]*" blocks "${qml_text}")
    foreach(block IN LISTS blocks)
        string(REGEX MATCH "from: ([0-9.]+), to: ([0-9.]+)" _ "${block}")
        to_thousandths("${CMAKE_MATCH_1}" qml_lo)
        to_thousandths("${CMAKE_MATCH_2}" qml_hi)
        set(scale 1)
        set(camel "")
        if(block MATCHES "value: Math\\.round\\(root\\.config\\.([a-zA-Z]+) \\* 100\\)")
            set(camel "${CMAKE_MATCH_1}")
            set(scale 100)
        elseif(block MATCHES "value: root\\.config\\.([a-zA-Z]+)")
            set(camel "${CMAKE_MATCH_1}")
        endif()
        if(camel STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE "([A-Z])" "_\\1" snake "${camel}")
        string(TOLOWER "${snake}" snake)
        if(NOT DEFINED clamp_lo_${snake})
            # A row bound to something config.h does not clamp is a different
            # kind of control (or a rename): say so rather than skip quietly.
            message(FATAL_ERROR "slider_bounds: ${qml} binds '${camel}' "
                                "(read as '${snake}') and config.h has no clamp for it")
        endif()
        math(EXPR expected_lo "${clamp_lo_${snake}} * ${scale}")
        math(EXPR expected_hi "${clamp_hi_${snake}} * ${scale}")
        if(NOT qml_lo EQUAL expected_lo OR NOT qml_hi EQUAL expected_hi)
            get_filename_component(page "${qml}" NAME)
            list(APPEND mismatches
                 "${page}: '${camel}' offers ${qml_lo}..${qml_hi} (thousandths) "
                 "over a clamp of ${expected_lo}..${expected_hi}")
        endif()
        math(EXPR paired "${paired} + 1")
    endforeach()
endforeach()

if(paired LESS 12)
    message(FATAL_ERROR "slider_bounds: only ${paired} rows paired with a clamp -- "
                        "the walk has stopped seeing the pages, which is not agreement")
endif()
if(mismatches)
    string(REPLACE ";" "\n  " mismatches "${mismatches}")
    message(FATAL_ERROR "a slider disagrees with the file it edits:\n  ${mismatches}")
endif()
message(STATUS "ok slider_bounds: ${paired} rows agree with config.h's clamps")
