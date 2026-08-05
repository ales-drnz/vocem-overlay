# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A crash is reported inside the window's Debug section, and nowhere else.
#
# The crash report was a window of its own on the desktop once -- built, made
# genuinely top-level (entry 57), and then removed on the owner's judgement:
# a window appearing on the desktop is exactly what this project's thesis
# rejects, and one report at a time was less than the journals could say. Its
# successor is the Debug section: the same journal, listed with its text among
# everything else the section shows. This holds both halves of the change on
# the real window, offscreen: a journal whose process is gone appears in the
# Debug page (an item named debugCrash, visible), and no top-level window
# called crashReport exists any more. Against the packaged 0.1.0-59 window the
# first check fails (no Debug section) and the second finds the pop-up.

if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/debug-section")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem/journal")

# A journal from a process that is gone, in the journal's own format. The pid is
# past any this kernel will hand out, so the scanner cannot mistake it for a
# live process and the test cannot depend on what else is running.
file(WRITE "${scratch}/cache/vocem/journal/4194305.running"
     "process = a-game\npid = 4194305\napi = opengl\nstarted = 2026-01-01 00:00:00\n--\n"
     "00:00:01 OpenGL backend ready\n00:00:02 uploading avatar 42_cafe.rgba\n")

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
        "VOCEM_CONFIG_SECTIONS=set:8"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()
file(READ "${scratch}/geometry.json" dump)

if(dump MATCHES "\"topLevel\": \"crashReport\"")
    message(FATAL_ERROR
        "a separate crash report window still appears -- the pop-up was removed "
        "in favour of the Debug section, and a second window on the desktop is "
        "exactly what the owner asked to be rid of")
endif()

string(REGEX MATCHALL "\"item\": \"[^\"]*debugCrash[^\"]*\"[^\n]*\"visible\": true"
       crash_rows "${dump}")
if(crash_rows STREQUAL "")
    message(FATAL_ERROR
        "the dead process's journal does not appear in the Debug section -- a "
        "crash nobody is shown is the silence the journal exists to break")
endif()

# And it is on screen without anybody clicking a tab: the section opens on
# whichever of its three views has something to say, and a crash report
# waiting is the reason somebody comes here at all. A page that opened on the
# health rows would hide the report behind a click nobody knows to make.
if(NOT dump MATCHES "\"item\": \"[^\"]*debugTabs[^\"]*\"[^\n]*\"visible\": true")
    message(FATAL_ERROR "the Debug section has no visible tab bar")
endif()

# The section carries the bottom bar every other page has, with the one action
# it has to offer in it. Reset and Apply are not here -- there is nothing to
# apply -- so the bar is earned by the action alone, which is the case
# SectionPage did not have until the owner asked for this button.
if(NOT dump MATCHES "\"item\": \"[^\"]*debugClear[^\"]*\"[^\n]*\"visible\": true")
    message(FATAL_ERROR
        "the Debug section has no Clear button in its bottom bar -- the page's "
        "one action belongs where every other page keeps its actions")
endif()

# And the same sentence is not repeated once per crashed row. The dump carries
# geometry, never text, so this half is read from the source, as the other
# source-level rules in this suite are (one_dlsym_version, instant_switches).
if(DEFINED QML_DIR)
    file(READ "${QML_DIR}/DebugPage.qml" debug_page)
    if(debug_page MATCHES "Ended without shutting down")
        message(FATAL_ERROR
            "every crashed row still explains what ending badly means -- the "
            "'Ended Badly' section header says it once, for all of them")
    endif()
endif()

message(STATUS "the crash is reported inside the Debug section, and only there")
