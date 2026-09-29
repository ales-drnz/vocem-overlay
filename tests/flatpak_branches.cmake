# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Every Vulkan layer extension point an installed runtime declares has a branch
# of the extension, and every branch is the one manifest with its version
# changed and nothing else.
#
# A runtime mounts org.freedesktop.Platform.VulkanLayer.* at the version of
# the extension point it declares. freedesktop 26.08 declares 26.08, and so
# does GNOME 51 on top of it; the project published 25.08 alone, so a game on
# either got no layer, and nothing anywhere said so -- the extension is simply
# not mounted. Measured on 2026-09-29: org.freedesktop.Platform//26.08 was
# installed here (`flatpak info -m`: [Extension
# org.freedesktop.Platform.VulkanLayer] version = 26.08), and Flathub's
# com.valvesoftware.Steam already ran on it.
#
# The first half reads this machine's installed runtimes, which is the honest
# witness for "a runtime asks for a branch we do not build" and the reason the
# test skips out loud without flatpak; versions older than the first branch
# are runtimes past their end of life, and not asked for. The second half holds
# flatpak/vulkanlayer/branch-manifest.sh to its promise: one manifest, the
# branches differing in `runtime-version` and `branch` and nothing else, so no
# fix can reach one branch and miss another.

set(dir "${SOURCE_DIR}/flatpak/vulkanlayer")
set(source "${dir}/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml")
file(STRINGS "${dir}/branches" branches REGEX "^[0-9]+\\.[0-9]+$")
if(NOT branches)
    message(FATAL_ERROR "${dir}/branches names no branch")
endif()
list(GET branches 0 first)
set(failures 0)

# --- every branch is the one manifest with its version changed --------------
file(STRINGS "${source}" source_lines)
foreach(branch IN LISTS branches)
    execute_process(COMMAND sh "${dir}/branch-manifest.sh" "${branch}"
                    RESULT_VARIABLE status OUTPUT_VARIABLE written ERROR_VARIABLE errors
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status EQUAL 0 OR NOT EXISTS "${written}")
        message("FAIL branch-manifest.sh ${branch} wrote nothing: ${errors}")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()
    file(STRINGS "${written}" lines)
    list(LENGTH lines count)
    list(LENGTH source_lines source_count)
    if(NOT count EQUAL source_count)
        message("FAIL the ${branch} manifest has ${count} lines, the source ${source_count}")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()
    set(differing "")
    set(runtime_ok FALSE)
    set(branch_ok FALSE)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        list(GET lines ${i} line)
        list(GET source_lines ${i} original)
        if(line STREQUAL "runtime-version: '${branch}'")
            set(runtime_ok TRUE)
        elseif(line STREQUAL "branch: '${branch}'")
            set(branch_ok TRUE)
        endif()
        if(NOT line STREQUAL original AND NOT line MATCHES "^(runtime-version|branch): ")
            list(APPEND differing "${line}")
        endif()
    endforeach()
    if(NOT runtime_ok OR NOT branch_ok)
        message("FAIL the ${branch} manifest does not say runtime-version and branch ${branch}")
        math(EXPR failures "${failures} + 1")
    elseif(differing)
        message("FAIL the ${branch} manifest differs from the source beyond its version: ${differing}")
        math(EXPR failures "${failures} + 1")
    else()
        message("ok   ${branch}: the one manifest, runtime-version and branch ${branch}")
    endif()
endforeach()

# --- every extension point an installed runtime declares is built -----------
find_program(FLATPAK flatpak)
set(asked "")
if(FLATPAK)
    execute_process(COMMAND "${FLATPAK}" list --runtime --columns=ref
                    OUTPUT_VARIABLE refs RESULT_VARIABLE status ERROR_QUIET)
    string(REGEX MATCHALL "[^\n]+" refs "${refs}")
    # A runtime installed both per user and system-wide is listed twice.
    if(refs)
        list(REMOVE_DUPLICATES refs)
    endif()
    foreach(ref IN LISTS refs)
        # The runtimes applications run on, not their extensions.
        if(NOT ref MATCHES "^org\\.[a-z]+\\.Platform/")
            continue()
        endif()
        execute_process(COMMAND "${FLATPAK}" info -m "${ref}"
                        OUTPUT_VARIABLE metadata ERROR_QUIET)
        if(metadata MATCHES "\\[Extension org\\.freedesktop\\.Platform\\.VulkanLayer\\][^[]*version *= *([0-9.]+)")
            set(point "${CMAKE_MATCH_1}")
            message("     ${ref} mounts VulkanLayer extensions at ${point}")
            if(point VERSION_LESS first)
                continue()
            endif()
            list(APPEND asked "${point}")
            if(NOT point IN_LIST branches)
                message("FAIL ${ref} asks for the extension at ${point} and no branch builds it: "
                        "every Vulkan game on that runtime goes without the layer, silently. "
                        "Add ${point} to flatpak/vulkanlayer/branches")
                math(EXPR failures "${failures} + 1")
            endif()
        endif()
    endforeach()
endif()

if(failures)
    message(FATAL_ERROR "the extension's branches do not cover what is asked of them")
endif()
if(NOT asked)
    message(STATUS "skip no installed runtime declares a VulkanLayer extension point, so what "
                   "the branches must cover was not measured here")
    return()
endif()
list(REMOVE_DUPLICATES asked)
message("ok   every extension point asked here (${asked}) has a branch")
