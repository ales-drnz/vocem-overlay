# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Runs a probe that needs artefacts of the 32-bit tree, resolving them now.
#
# build32 is configured and built on its own (CLAUDE.md), and ctest never sees
# it. The tests that run against it used to find its files when build/ was
# CONFIGURED -- find_library into build32/gl, an EXISTS on its manifest -- so a
# build32 created after build/ was configured was invisible until somebody
# reconfigured, and one deleted afterwards left a path that no longer existed
# baked into the test's environment, where ld.so would print "cannot be
# preloaded: ignored" and the probe would go on measuring the wrong thing. The
# files are looked for here, when the test runs, and a missing one is a skip
# that says which file.
#
# Expects PROBE (the executable), REQUIRES (the files that must exist), ENV
# (KEY=VALUE items for the probe's environment) and ARGS (its arguments).

foreach(file IN LISTS REQUIRES)
    if(NOT EXISTS "${file}")
        message(STATUS "skip ${file} is not built: this width runs against the 32-bit tree, "
                       "which is looked for when the test runs and not when the build was configured")
        return()
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${ENV} "${PROBE}" ${ARGS}
    RESULT_VARIABLE status)

if(status EQUAL 77)
    message(STATUS "skip the probe could not measure here; its own output above says why")
elseif(NOT status EQUAL 0)
    message(FATAL_ERROR "${PROBE} exited ${status}")
endif()
