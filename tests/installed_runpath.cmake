# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Nothing this project installs carries a RUNPATH.
#
# qt_standard_project_setup() turns on Qt's deployment defaults, and one of
# them is an install RPATH of $ORIGIN:$ORIGIN/../lib on every executable of
# the directory it is called in. So the packaged /usr/bin/vocem-config asked
# the loader to look for its libraries in /usr/bin and /usr/lib before the
# system's search path -- measured with readelf on 0.1.10-7, the only
# shipped binary with a RUNPATH at all -- which is a Qt application-bundle
# convention and wrong for a file in a distribution's /usr/bin: namcap flags
# it, and anything that drops a library into /usr/bin is loaded first.
#
# Measured on what an install produces rather than on the build tree, whose
# RUNPATH is the build's own and is supposed to be there: `cmake --install`
# into a scratch DESTDIR, then readelf on every ELF file it put down.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
find_program(READELF readelf)
if(NOT READELF)
    message(STATUS "skip readelf is missing")
    return()
endif()

set(destdir "${CMAKE_CURRENT_BINARY_DIR}/installed-runpath")
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

file(GLOB_RECURSE installed LIST_DIRECTORIES false "${destdir}/*")
set(elves 0)
set(offenders "")
foreach(file IN LISTS installed)
    if(IS_SYMLINK "${file}")
        continue()
    endif()
    file(READ "${file}" magic LIMIT 4 HEX)
    if(NOT magic STREQUAL "7f454c46")
        continue()
    endif()
    math(EXPR elves "${elves} + 1")
    execute_process(COMMAND "${READELF}" -d "${file}" OUTPUT_VARIABLE dynamic
                    ENVIRONMENT_MODIFICATION "LC_ALL=set:C")
    string(REPLACE "${destdir}" "" shown "${file}")
    if(dynamic MATCHES "\\((RUNPATH|RPATH)\\)[^\n]*")
        list(APPEND offenders "${shown}: ${CMAKE_MATCH_0}")
    else()
        message("ok   ${shown}: no RUNPATH")
    endif()
endforeach()
if(elves EQUAL 0)
    message(FATAL_ERROR "the install put down no ELF file at all -- nothing was measured")
endif()
if(offenders)
    foreach(offender IN LISTS offenders)
        message("FAIL ${offender}")
    endforeach()
    message(FATAL_ERROR "an installed binary carries a RUNPATH")
endif()
file(REMOVE_RECURSE "${destdir}")
message(STATUS "none of the ${elves} installed ELF files carries a RUNPATH")
