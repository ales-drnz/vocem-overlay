# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A switch means the click did something; everything else is a checkbox.
#
# The KDE guidelines put it plainly in *Getting input*: "Use a Switch for
# 'instant apply' controls that take effect immediately; otherwise, use a
# CheckBox." This window has exactly three settings that take effect
# immediately -- the overlay, the voice panel and the messages, written straight
# to the file by ConfigBridge::persistNow() and reaching a running game a couple
# of seconds later with no Apply in between. Two of them are the switches in the
# header; the third is the tray's checkable menu entry.
#
# Every other boolean in the window sits on a page under an Apply bar and does
# nothing at all until that button is pressed. Ten of them were switches through
# 0.1.0-64, promising an immediacy the code does not have -- and the Applications
# page said so out loud, in a subtitle claiming the row "takes effect in a
# running game within a couple of seconds".
#
# A source check rather than a measurement of the built window, because Qt gives
# a Switch and a CheckBox the same accessible role and the same reported
# geometry family: what distinguishes them here is which type the QML names.
# Like one_spelling.cmake, the rule is about code.
#
# The list of persistNow() settings is checked too, so that making some future
# setting instant and leaving this rule behind is a failure rather than a
# silence.

file(GLOB qml "${SOURCE_DIR}/gui/qml/*.qml")

set(offenders "")
foreach(source IN LISTS qml)
    get_filename_component(name "${source}" NAME)
    if(name STREQUAL "Main.qml")
        continue()
    endif()
    file(STRINGS "${source}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "^[ \t]*//.*" "" code "${line}")
        if(code MATCHES "^[ \t]*Switch[ \t]*\\{")
            list(APPEND offenders
                 "${name}:${number}: a Switch on a page under an Apply bar")
        endif()
    endforeach()
endforeach()

# The header's two, and only those two.
file(STRINGS "${SOURCE_DIR}/gui/qml/Main.qml" main_lines REGEX "^[ \t]*Switch[ \t]*\\{")
list(LENGTH main_lines header_switches)
if(NOT header_switches EQUAL 2)
    list(APPEND offenders
         "Main.qml: ${header_switches} switches in the header, expected the two that are "
         "written immediately (the voice panel and the messages)")
endif()

# What actually writes without waiting for Apply. Three calls, and the switches
# above stand for exactly those.
file(STRINGS "${SOURCE_DIR}/gui/src/config_bridge.cpp" instant REGEX "^[ \t]*persistNow\\(\\);")
list(LENGTH instant instant_count)
if(NOT instant_count EQUAL 3)
    list(APPEND offenders
         "config_bridge.cpp: ${instant_count} settings write immediately, not 3 -- the "
         "rule about which controls may be switches was written against three")
endif()

if(offenders)
    message("A boolean promises an immediacy the code does not have:")
    foreach(offender IN LISTS offenders)
        message("  ${offender}")
    endforeach()
    message(FATAL_ERROR
        "a Switch is for a setting that takes effect on the click; a setting that "
        "waits for Apply is a CheckBox (KDE HIG, Getting input)")
endif()

message("ok   the only switches are the ones whose click reaches a running game")
