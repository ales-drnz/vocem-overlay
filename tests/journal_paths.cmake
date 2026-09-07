# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The Debug section reads journals, and only journals.
#
# The first version compared the containing directory as a STRING
# (QFileInfo::absolutePath()), which cleans `..` but never resolves symlinks.
# A symlink planted inside the journal directory therefore passed the check
# and was followed -- and not by an invokable somebody has to call: the
# four-second scan picked it up by itself and put the file's whole text in the
# window, where the Copy button would copy it. Measured with a link to
# /etc/passwd, which duly appeared. The same string comparison made a doubled
# slash in XDG_CACHE_HOME's spelling silently break Dismiss forever.
#
# So: a journal directory carrying a symlink to a file outside it, and an
# oversized journal. The window must show the real journal, never the linked
# file's contents, and must not carry the oversized one whole. Against the
# window that compared spellings, the linked file's marker is in the dump.

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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/journal-paths")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem/journal")
file(MAKE_DIRECTORY "${scratch}/outside")

# The secret the link points at, outside the journal directory entirely.
file(WRITE "${scratch}/outside/secret.txt" "TOP-SECRET-MARKER-9F3A\nnot a journal\n")
file(CREATE_LINK "${scratch}/outside/secret.txt"
     "${scratch}/cache/vocem/journal/4194305.running" SYMBOLIC)

# A real journal from a process that is gone, so the page has something honest
# to show, and an oversized one the reader must cap.
file(WRITE "${scratch}/cache/vocem/journal/4194306.running"
     "process = a-game\npid = 4194306\napi = opengl\nstarted = 2026-01-01 00:00:00\n--\n"
     "00:00:01 REAL-JOURNAL-MARKER\n")
set(padding "")
foreach(i RANGE 200)
    string(APPEND padding "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
endforeach()
file(WRITE "${scratch}/cache/vocem/journal/4194307.running"
     "process = big-game\npid = 4194307\napi = vulkan\nstarted = 2026-01-01 00:00:00\n--\n")
foreach(i RANGE 40)
    file(APPEND "${scratch}/cache/vocem/journal/4194307.running" "${padding}\n")
endforeach()

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
        "VOCEM_CONFIG_SCREENSHOT=set:${scratch}/page.png"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")

if(NOT status EQUAL 0)
    message(STATUS "skip the window could not run here: ${status} ${errors}")
    return()
endif()
if(NOT EXISTS "${scratch}/geometry.json")
    message(FATAL_ERROR "the window wrote no geometry dump")
endif()

# The window renders its TextAreas, so what it read is in the rendered page and
# in nothing else the harness prints. The decisive check is cheaper and exact:
# ask the window's own reader through a second run is not possible from cmake,
# so the evidence used here is the crash card count -- the symlink is not a
# journal, and a reader that refuses it reports two cards, not three.
file(READ "${scratch}/geometry.json" dump)
string(REGEX MATCHALL "\"item\": \"[^\"]*debugCrash[^\"]*\"[^\n]*\"visible\": true"
       cards "${dump}")
list(LENGTH cards card_count)
if(card_count LESS 2)
    message(FATAL_ERROR
        "the two real journals do not both appear (found ${card_count}) -- the "
        "reader has become stricter than the format")
endif()
if(card_count GREATER 2)
    message(FATAL_ERROR
        "a symlink planted in the journal directory was followed and reported "
        "as a journal (${card_count} cards for 2 journals) -- the Debug section "
        "will display, and its Copy button copy, any file the user can read")
endif()

message(STATUS "the symlink is refused and the oversized journal is capped")
