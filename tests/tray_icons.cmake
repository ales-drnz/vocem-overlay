# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The tray's five state icons: installed, one circle each at one radius,
# token-true, nothing behind them, and asked for by the name they carry.
#
# The tray icon is the user's own voice state -- a hollow ring outside a
# channel, a grey disc in one, green while speaking, red with a microphone
# while muted, red with headphones while deafened. A state icon missing from
# the install, or one that stops carrying its token colour, degrades to a
# generic icon with nothing logged. "All the same size" is the owner's
# requirement: five discs that drift apart make the tray twitch.
#
# Measured on what `cmake --install` puts down in a scratch DESTDIR, drawn by
# Qt's SVG renderer (tray_icon_pictures.cpp): a file the parser refuses, a
# radius that drifted, a backdrop or a wrong colour shows in the pixels.
# Expects BUILD_DIR and PROBE.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests")
endif()
if(NOT EXISTS "${PROBE}")
    message(STATUS "skip the picture probe was not built (Qt6 Svg missing)")
    return()
endif()

# Grey is the overlay's own muted text token, green the speaking ring's, red
# the muted badge's. A pipe separator, because a nested list flattens.
set(pairs
    "io.github.ales_drnz.vocem_overlay-offline.svg|#9aa0ab"
    "io.github.ales_drnz.vocem_overlay-idle.svg|#9aa0ab"
    "io.github.ales_drnz.vocem_overlay-speaking.svg|#23a55a"
    "io.github.ales_drnz.vocem_overlay-muted.svg|#f23f43"
    "io.github.ales_drnz.vocem_overlay-deafened.svg|#f23f43")

set(destdir "${CMAKE_CURRENT_BINARY_DIR}/installed-tray-icons")
file(REMOVE_RECURSE "${destdir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix /usr
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    ENVIRONMENT_MODIFICATION "DESTDIR=set:${destdir}")
if(NOT status EQUAL 0)
    message("${output}${errors}")
    message(FATAL_ERROR "cmake --install into a scratch DESTDIR failed")
endif()
set(installed "${destdir}/usr/share/icons/hicolor/scalable/apps")

set(probe_args "")
foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 token)
    if(NOT EXISTS "${installed}/${name}")
        message(FATAL_ERROR "${name} is not installed -- the packaged tray falls back to "
                            "the generic icon")
    endif()
    list(APPEND probe_args "${installed}/${name}=${token}")
endforeach()
execute_process(
    COMMAND "${PROBE}" ${probe_args}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
message("${output}")
file(REMOVE_RECURSE "${destdir}")
if(NOT status EQUAL 0)
    message("${errors}")
    message(FATAL_ERROR "the installed tray icons are not five circles of one radius in "
                        "their states' colours")
endif()

# What has no picture to measure: the names the window asks for. The tray is
# a status-notifier icon outside this process, so this half stays a reading of
# the QML. A state whose icon name is misspelled draws the theme's fallback
# with nothing logged; comments are stripped so a commented-out line cannot
# satisfy it (entry 77).
file(READ "${SOURCE_DIR}/gui/qml/Tray.qml" tray)
string(REGEX REPLACE "//[^\n]*" "" tray "${tray}")
foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    string(REPLACE ".svg" "" icon_name "${name}")
    if(NOT tray MATCHES "\"${icon_name}\"")
        message(FATAL_ERROR "Tray.qml never asks for ${icon_name} -- an "
                            "installed icon no state can reach")
    endif()
endforeach()

# The picture of the tray (TaskbarPreview.qml) derives its icon from a private
# copy of the same state-to-name table: the two mappings are compared pair by
# pair, not by the presence of names, so two swapped states fail.
file(READ "${SOURCE_DIR}/gui/qml/TaskbarPreview.qml" preview)
string(REGEX REPLACE "//[^\n]*" "" preview "${preview}")
foreach(source_name IN ITEMS tray preview)
    string(REGEX MATCHALL "case ConfigBridge\\.[A-Za-z]+: return \"[^\"]+\""
           hits "${${source_name}}")
    set(${source_name}_map "")
    foreach(hit IN LISTS hits)
        string(REGEX REPLACE "case ConfigBridge\\.([A-Za-z]+): return \"([^\"]+)\"" "\\1=\\2"
               entry "${hit}")
        list(APPEND ${source_name}_map "${entry}")
    endforeach()
    list(SORT ${source_name}_map)
endforeach()
list(LENGTH tray_map tray_pairs)
if(tray_pairs LESS 4)
    message(FATAL_ERROR "only ${tray_pairs} state-to-icon pairs read from Tray.qml -- "
                        "the parse has stopped matching the source")
endif()
if(NOT tray_map STREQUAL preview_map)
    message(FATAL_ERROR "Tray.qml and TaskbarPreview.qml disagree about which state wears "
                        "which icon:\n  tray:    ${tray_map}\n  preview: ${preview_map}")
endif()

message(STATUS "five tray icons installed, one radius, token-true, asked for, "
               "and the preview's table agrees pair by pair")
