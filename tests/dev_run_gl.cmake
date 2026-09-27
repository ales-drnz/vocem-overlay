# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# scripts/dev-run-gl.sh has to run the application against THIS tree's shim.
#
# The session already preloads the installed shim (environment.d:
# LD_PRELOAD=:/usr/$LIB/libvocem_gl_shim.so), and the script used to APPEND the
# build tree's shim after it. The first definition in the global scope wins, so
# the installed shim took every hook and the build tree's was mapped and never
# called -- while LD_LIBRARY_PATH made the installed shim dlopen the build
# tree's heavy library: a mixed stack, the package's hooks drawing with this
# tree's overlay, in exactly the script a developer runs to test a change to
# the hooks. The script strips every libvocem_gl_shim.so entry from LD_PRELOAD
# and puts this tree's in front.
#
# Measured the way it matters: which_shim, run through the script under the
# session's own LD_PRELOAD value, names the object that answers for
# glXSwapBuffers and eglSwapBuffers.
#
# And a 32-bit child -- a 32-bit game, or the 32-bit half of one started from a
# 64-bit launcher -- has to get a 32-bit shim. The first fix stripped every
# libvocem_gl_shim.so entry, /usr/$LIB/... included, and preloaded the 64-bit
# build shim alone: a 32-bit child got "wrong ELF class" and no shim at all.
# PROBE32 (which_shim built -m32) runs through the script the same way and must
# be answered by build32/'s shim, or by the installed 32-bit one when build32/
# has none.
#
# Expects SOURCE_DIR, PROBE (the which_shim executable) and PROBE32 (its -m32
# twin; empty when there is no 32-bit toolchain).

set(shim "${SOURCE_DIR}/build/gl/libvocem_gl_shim.so")
if(NOT EXISTS "${shim}")
    message(STATUS "skip ${shim} is not built: the script only knows the tree's build/ directory")
    return()
endif()

# The object that answered, compared by the file it is: the script preloads
# through a directory of per-width links.
function(expect_shim probe wanted label)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=:/usr/\$LIB/libvocem_gl_shim.so"
                sh "${SOURCE_DIR}/scripts/dev-run-gl.sh" "${probe}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    message(STATUS "which_shim (${label}) through the script:\n${out}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "dev-run-gl.sh exited ${status} (${label}): ${err}")
    endif()
    file(REAL_PATH "${wanted}" wanted_real)
    foreach(name glXSwapBuffers eglSwapBuffers)
        string(REGEX MATCH "${name} [^\n]*" line "${out}")
        string(REPLACE "${name} " "" where "${line}")
        set(where_real "${where}")
        if(EXISTS "${where}")
            file(REAL_PATH "${where}" where_real)
        endif()
        if(NOT where_real STREQUAL wanted_real)
            message(FATAL_ERROR "under the session's LD_PRELOAD, dev-run-gl.sh gave ${name} "
                                "(${label}) to '${where}', not to ${wanted}")
        endif()
    endforeach()
    if(err MATCHES "libvocem_gl_shim")
        message(FATAL_ERROR "ld.so said something about a shim on the way in (${label}):\n"
                            "${err}")
    endif()
    message(STATUS "ok dev-run-gl.sh runs the ${label} application against ${wanted}")
endfunction()

expect_shim("${PROBE}" "${shim}" "64-bit")
if(PROBE32)
    set(shim32 "${SOURCE_DIR}/build32/gl/libvocem_gl_shim.so")
    if(NOT EXISTS "${shim32}")
        set(shim32 "/usr/lib32/libvocem_gl_shim.so")
    endif()
    if(NOT EXISTS "${shim32}")
        message(STATUS "skip no 32-bit shim in build32/ or /usr/lib32 for the 32-bit child")
        return()
    endif()
    expect_shim("${PROBE32}" "${shim32}" "32-bit")
endif()
