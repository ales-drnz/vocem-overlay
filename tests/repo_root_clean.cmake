# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Nothing sits in the repository root that is neither tracked nor ignored.
#
# `git status` does not report a directory that contains no files, so a stray
# one is invisible to the check everybody makes. Six of them were carried for
# three days: `0192/`, `inst/`, `LD_PRELOAD=/`, `VOCEM_DISABLE=1/`,
# `SteamLinuxRuntime_4/` and `SteamLinuxRuntime_sniper/`, each holding an empty
# `cache/` and `config/`, all created within the same minute on 2026-09-15 --
# this project's own environment variables and Steam runtime path fragments, so
# an ad-hoc command word-split a line into directory names. Entry 137 caught
# the version of this that has files in it ("two were sitting beside
# CMakeLists.txt, untracked") because that one `git status` CAN see.
#
# The rule is the one the .gitignore already implies: everything in the root is
# either part of the project or explicitly ignored. Anything else is somebody's
# accident, and the ones that leave no files behind are the ones nobody finds.
#
# Skips where git cannot answer -- a tarball build, a checkout with no .git --
# rather than asserting about a question it cannot ask.

if(NOT EXISTS "${SOURCE_DIR}/.git")
    message(STATUS "skip no .git here, so nothing can say what is tracked or ignored")
    return()
endif()
find_program(GIT git)
if(NOT GIT)
    message(STATUS "skip git is not installed")
    return()
endif()

file(GLOB entries RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
list(LENGTH entries entry_count)
if(entry_count LESS 10)
    message(FATAL_ERROR
        "only ${entry_count} entries in ${SOURCE_DIR}: the glob is what is broken, not the tree")
endif()

set(strays "")
foreach(entry IN LISTS entries)
    if(entry STREQUAL ".git")
        continue()
    endif()
    # Ignored on purpose?
    execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" check-ignore -q "${entry}"
                    RESULT_VARIABLE ignored OUTPUT_QUIET ERROR_QUIET)
    if(ignored EQUAL 0)
        continue()
    endif()
    # Tracked, or holding something tracked?
    execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" ls-files --error-unmatch -- "${entry}"
                    RESULT_VARIABLE tracked OUTPUT_QUIET ERROR_QUIET)
    if(tracked EQUAL 0)
        continue()
    endif()
    # Untracked but carrying files: `git status` already shows it, and this
    # test is about the case it cannot. Named here anyway, because a file this
    # test names is one somebody looked at.
    file(GLOB_RECURSE inside "${SOURCE_DIR}/${entry}/*")
    list(LENGTH inside inside_count)
    if(IS_DIRECTORY "${SOURCE_DIR}/${entry}")
        list(APPEND strays "${entry}/ (${inside_count} file(s) inside)")
    else()
        list(APPEND strays "${entry}")
    endif()
endforeach()

if(NOT strays STREQUAL "")
    string(REPLACE ";" "\n  " listed "${strays}")
    message(FATAL_ERROR
        "the repository root carries entries that are neither tracked nor ignored:\n  ${listed}\n"
        "An empty directory is invisible to `git status`, which is how six of them went "
        "unnoticed for three days. Either it belongs to the project and should be committed, or "
        "it is build output and belongs in .gitignore, or somebody's command made it by "
        "accident and it should go.")
endif()

message("ok   every entry in the repository root is tracked or ignored, ${entry_count} of them")
