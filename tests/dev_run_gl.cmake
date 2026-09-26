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
# Expects SOURCE_DIR and PROBE (the which_shim executable).

set(shim "${SOURCE_DIR}/build/gl/libvocem_gl_shim.so")
if(NOT EXISTS "${shim}")
    message(STATUS "skip ${shim} is not built: the script only knows the tree's build/ directory")
    return()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=:/usr/\$LIB/libvocem_gl_shim.so"
            sh "${SOURCE_DIR}/scripts/dev-run-gl.sh" "${PROBE}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
message(STATUS "which_shim through the script:\n${out}")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "dev-run-gl.sh exited ${status}: ${err}")
endif()
foreach(name glXSwapBuffers eglSwapBuffers)
    string(REGEX MATCH "${name} [^\n]*" line "${out}")
    string(REPLACE "${name} " "" where "${line}")
    if(NOT where STREQUAL shim)
        message(FATAL_ERROR "under the session's LD_PRELOAD, dev-run-gl.sh gave ${name} to "
                            "'${where}', not to this tree's shim ${shim}: the script is testing "
                            "the installed hooks")
    endif()
endforeach()
if(err MATCHES "libvocem_gl_shim")
    message(FATAL_ERROR "ld.so said something about a shim on the way in:\n${err}")
endif()
message(STATUS "ok dev-run-gl.sh runs the application against ${shim} alone")
