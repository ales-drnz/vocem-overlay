# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A slider's ends are the ends the settings file is read with.
#
# config.h clamps what is read (Config::numbers()), the page says what can be
# dragged, and the two drifted once: "Show for" offered 1-20 over a clamp of
# 1-30, so a legitimate 30 in the file showed as a slider pinned at 20 that
# rewrote the setting the moment it was touched. This moves every Slider and
# SpinBox in the running window to its `to` and its `from` (window_controls
# .cmake) and holds what the bridge then holds to the compiled clamp of the
# setting the control changed:
#   * the control changes exactly one setting, and it is a clamped number;
#   * at `from` the setting is the clamp's low end, at `to` its high end --
#     so a slider narrower than the clamp, or wider (the bridge clamps it), is
#     a failure;
#   * `from` and `to` are those ends in the control's own unit, the same or a
#     hundred times (a percentage);
#   * moving it does not write config.ini: numbers wait for Apply.
# At least 14 controls must be paired: a walk that finds none must fail rather
# than agree with nothing.
#
# Expects CONFIG_BINARY, NUMBERS_BINARY (tests/config_numbers.cpp) and
# CMAKE_CURRENT_BINARY_DIR (a test directory).

include("${CMAKE_CURRENT_LIST_DIR}/window_controls.cmake")
vocem_window_controls("${CMAKE_CURRENT_BINARY_DIR}/slider-bounds")

# In integer thousandths, so that CMake can compare exactly.
function(to_thousandths value out)
    if(value MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
        set(frac "${CMAKE_MATCH_4}000")
        string(SUBSTRING "${frac}" 0 3 frac)
        math(EXPR result "${CMAKE_MATCH_2} * 1000 + ${frac}")
        if(CMAKE_MATCH_1)
            math(EXPR result "0 - ${result}")
        endif()
    else()
        message(FATAL_ERROR "slider_bounds: cannot read '${value}' as a number")
    endif()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND "${NUMBERS_BINARY}" OUTPUT_VARIABLE numbers RESULT_VARIABLE got)
if(NOT got EQUAL 0)
    message(FATAL_ERROR "slider_bounds: ${NUMBERS_BINARY} answered ${got}")
endif()
string(REPLACE "\n" ";" numbers "${numbers}")
set(clamp_count 0)
foreach(row IN LISTS numbers)
    if(row MATCHES "^([a-z_]+) ([0-9.]+) ([0-9.]+) [0-9]+$")
        to_thousandths("${CMAKE_MATCH_2}" low)
        to_thousandths("${CMAKE_MATCH_3}" high)
        set(low_${CMAKE_MATCH_1} "${low}")
        set(high_${CMAKE_MATCH_1} "${high}")
        math(EXPR clamp_count "${clamp_count} + 1")
    endif()
endforeach()

set(paired 0)
set(problems "")
foreach(line IN LISTS controls)
    string(JSON type GET "${line}" control)
    if(NOT type STREQUAL "Slider" AND NOT type STREQUAL "SpinBox")
        continue()
    endif()
    string(JSON label GET "${line}" label)
    string(JSON drives GET "${line}" drives)
    string(JSON writes GET "${line}" writesAtOnce)
    set(name "${type} '${label}'")
    if(drives STREQUAL "" OR drives MATCHES ",")
        list(APPEND problems "${name} changes '${drives}', not exactly one setting")
        continue()
    endif()
    string(REGEX REPLACE "([A-Z])" "_\\1" key "${drives}")
    string(TOLOWER "${key}" key)
    if(NOT DEFINED low_${key})
        list(APPEND problems "${name} changes ${drives} (${key}), which config.h does not clamp")
        continue()
    endif()
    string(JSON from GET "${line}" from)
    string(JSON to GET "${line}" to)
    string(JSON at_from GET "${line}" atFrom)
    string(JSON at_to GET "${line}" atTo)
    foreach(v from to at_from at_to)
        to_thousandths("${${v}}" ${v})
    endforeach()
    if(NOT at_from EQUAL low_${key} OR NOT at_to EQUAL high_${key})
        list(APPEND problems "${name}: ${key} reaches ${at_from}..${at_to} (thousandths) at the "
                             "control's ends, over a clamp of ${low_${key}}..${high_${key}}")
    endif()
    math(EXPR from_percent "${low_${key}} * 100")
    math(EXPR to_percent "${high_${key}} * 100")
    if(NOT ((from EQUAL low_${key} AND to EQUAL high_${key}) OR
            (from EQUAL from_percent AND to EQUAL to_percent)))
        list(APPEND problems "${name} offers ${from}..${to} (thousandths), which is neither "
                             "${key}'s clamp nor that clamp as a percentage")
    endif()
    if(writes)
        list(APPEND problems "${name} wrote config.ini when it moved: a number waits for Apply")
    endif()
    math(EXPR paired "${paired} + 1")
endforeach()

if(clamp_count LESS 16)
    message(FATAL_ERROR "slider_bounds: config_numbers printed ${clamp_count} clamps")
endif()
if(paired LESS 14)
    list(APPEND problems "only ${paired} controls were driven and paired, of at least 14 -- a "
                         "walk that stopped reaching the pages is not agreement")
endif()
if(problems)
    string(REPLACE ";" "\n  " problems "${problems}")
    message(FATAL_ERROR "a control disagrees with the file it edits:\n  ${problems}")
endif()
message(STATUS "ok slider_bounds: ${paired} controls reach exactly config.h's clamps")
