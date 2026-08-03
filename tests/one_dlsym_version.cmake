# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# There is one place that says which symbol version `dlsym` has, and it is
# `gl/src/real_dlsym.h`.
#
# This test exists because the first time that version was wrong it was fixed in one
# file and not the other. The shim was corrected -- loudly, with a test -- and
# `libvocem_gl.so` kept its own copy of the same line, so 32-bit games went on
# getting an overlay that loaded, reported itself as drawing, and drew nothing,
# because every GL symbol it looked up came back null. Two copies of a constant is
# one copy too many when getting it wrong is invisible.
#
# The rule is about code and not about prose: a line that calls `dlvsym` may not
# carry a version string of its own. Comments are free to name versions, and the
# header is free to define them.

file(GLOB_RECURSE sources
    "${SOURCE_DIR}/gl/src/*.cpp" "${SOURCE_DIR}/gl/src/*.h"
    "${SOURCE_DIR}/layer/src/*.cpp" "${SOURCE_DIR}/layer/src/*.h"
    "${SOURCE_DIR}/common/src/*.cpp"
    "${SOURCE_DIR}/include/vocem/*.h"
    "${SOURCE_DIR}/tests/*.cpp")

set(offenders "")
foreach(source IN LISTS sources)
    if(source MATCHES "real_dlsym\\.h$")
        continue()
    endif()
    file(STRINGS "${source}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "^[ \t]*(//|\\*|/\\*).*" "" code "${line}")
        if(code MATCHES "dlvsym" AND code MATCHES "\"GLIBC_")
            list(APPEND offenders "${source}:${number}: ${line}")
        endif()
    endforeach()
endforeach()

if(offenders)
    message("A dlvsym call names a symbol version itself instead of using")
    message("VOCEM_DLSYM_VERSIONS from gl/src/real_dlsym.h:")
    foreach(offender IN LISTS offenders)
        message("  ${offender}")
    endforeach()
    message(FATAL_ERROR "the version belongs in one place, and this is a second one")
endif()

message("ok   only gl/src/real_dlsym.h names a dlsym symbol version")
