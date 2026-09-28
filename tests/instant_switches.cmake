# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A switch means the click did something; everything else is a checkbox.
#
# The KDE guidelines, *Getting input*: "Use a Switch for 'instant apply'
# controls that take effect immediately; otherwise, use a CheckBox." Three
# settings in this window are written on the click (ConfigBridge::persistNow):
# the overlay, the voice panel and the messages. The last two are the
# header's switches; the first is the tray's checkable entry, which is not an
# item of the window and is not reached here. Ten booleans on pages under an
# Apply bar were switches through 0.1.0-64.
#
# Measured rather than read: every Switch and CheckBox in the running window
# is clicked (window_controls.cmake) and config.ini is compared before and
# after the click.
#   * every Switch wrote config.ini on the click;
#   * every CheckBox did not;
#   * the switches are the voice panel's and the messages', and each changes
#     that one setting.
# At least seven checkboxes must be clicked: a walk that finds none must fail.
#
# Expects CONFIG_BINARY and CMAKE_CURRENT_BINARY_DIR (a test directory).

include("${CMAKE_CURRENT_LIST_DIR}/window_controls.cmake")
vocem_window_controls("${CMAKE_CURRENT_BINARY_DIR}/instant-switches")

set(problems "")
set(switched "")
set(checkboxes 0)
foreach(line IN LISTS controls)
    string(JSON type GET "${line}" control)
    if(NOT type STREQUAL "Switch" AND NOT type STREQUAL "CheckBox")
        continue()
    endif()
    string(JSON label GET "${line}" label)
    string(JSON drives GET "${line}" drives)
    string(JSON writes GET "${line}" writesAtOnce)
    string(JSON section GET "${line}" section)
    if(type STREQUAL "Switch")
        list(APPEND switched "${drives}")
        if(NOT writes)
            list(APPEND problems "the Switch '${label}' (section ${section}, ${drives}) did not "
                                 "write config.ini on the click")
        endif()
    else()
        math(EXPR checkboxes "${checkboxes} + 1")
        if(writes)
            list(APPEND problems "a CheckBox (section ${section}, ${drives}) wrote config.ini on "
                                 "the click: a setting written at once is a Switch")
        endif()
    endif()
endforeach()
list(SORT switched)
string(REPLACE ";" "," switched "${switched}")
if(NOT switched STREQUAL "notificationsEnabled,panelEnabled")
    list(APPEND problems "the switches change '${switched}', expected the voice panel's and "
                         "the messages' (notificationsEnabled,panelEnabled)")
endif()
if(checkboxes LESS 7)
    list(APPEND problems "only ${checkboxes} checkboxes were clicked, of at least 7 -- a walk "
                         "that stopped reaching the pages is not agreement")
endif()
if(problems)
    string(REPLACE ";" "\n  " problems "${problems}")
    message(FATAL_ERROR "a boolean promises an immediacy it does not have, or hides one:\n  "
                        "${problems}")
endif()
message(STATUS "ok instant_switches: 2 switches write on the click, ${checkboxes} checkboxes wait "
               "for Apply")
