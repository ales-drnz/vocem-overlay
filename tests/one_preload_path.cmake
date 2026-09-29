# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The two ways the shim gets into a process, held to one path.
#
# Entry 16: `vocem-run` preloads the same shim the session preloads through
# environment.d, "so the two entry paths cannot drift" -- a difference between
# them shows up only in the games launched one way, which is the worst shape a
# difference can have. They had drifted: `vocem-run.in` took a `/usr/$LIB`
# token compiled into CMakeLists.txt, and `50-vocem.conf.in` became a template
# over `@CMAKE_INSTALL_PREFIX@` when entry 137 made the install work under
# `~/.local`. On any prefix but `/usr` the session preloaded one file and
# vocem-run named another, which does not exist: `ld.so` prints
# `cannot be preloaded: ignored` into that program's stderr and the game gets no
# OpenGL overlay, with nothing from this project saying why. The README tells a
# reader to install under `~/.local` and then tells the same reader to use
# `vocem-run`.
#
# **This test configures at a prefix of its own, and that is the whole point.**
# At the default prefix the two agree by accident, so a comparison of the two
# installed files on this machine passes against the defect -- entry 77's shape,
# and the reason the drift survived entry 137's own pass over these files. The
# measurement is on the GENERATED files (the artefacts), not on the templates:
# one CMake configure at `/opt/vocem-preload-test`, one grep each.
#
# Cost: about a second, measured. Nothing is built and nothing is installed.

if(NOT DEFINED SOURCE_DIR OR NOT EXISTS "${SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SOURCE_DIR does not name this project: ${SOURCE_DIR}")
endif()
# Run through ctest, or from the test directory: in script mode CMake sets
# CMAKE_CURRENT_BINARY_DIR to the working directory whatever -D says, and this
# one configures a whole tree -- so without the check a hand run leaves it
# wherever it was started, which is what happened to the repository root twice
# (entry 137). Same spelling as the window-driving scripts.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a test "
        "directory: run this from <build>/tests, or through ctest")
endif()

set(prefix "/opt/vocem-preload-test")
set(tree "${CMAKE_CURRENT_BINARY_DIR}/one_preload_path")
file(REMOVE_RECURSE "${tree}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${tree}"
            "-DCMAKE_INSTALL_PREFIX=${prefix}"
            -DVOCEM_BUILD_TESTS=OFF
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 300)
if(NOT status EQUAL 0)
    message(STATUS "skip the project does not configure here: ${status}\n${output}${errors}")
    return()
endif()

set(run "${tree}/vocem-run")
set(conf "${tree}/packaging/50-vocem.conf")
foreach(file "${run}" "${conf}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR
            "a configure at ${prefix} did not generate ${file}, so the two entry paths cannot "
            "be compared: this test no longer measures what it says")
    endif()
endforeach()

# What vocem-run preloads.
file(READ "${run}" run_text)
if(NOT run_text MATCHES "VOCEM_PRELOAD='([^']+)'")
    message(FATAL_ERROR "no VOCEM_PRELOAD= line in the generated vocem-run")
endif()
set(run_path "${CMAKE_MATCH_1}")

# What the session preloads. systemd expands `$$LIB` to `$LIB` before ld.so
# sees it, so that doubling is the same path written for another reader.
file(READ "${conf}" conf_text)
if(NOT conf_text MATCHES "LD_PRELOAD=\\$\\{LD_PRELOAD\\}:([^\n]+)")
    message(FATAL_ERROR "no LD_PRELOAD= line in the generated 50-vocem.conf")
endif()
string(REPLACE "$$LIB" "$LIB" conf_path "${CMAKE_MATCH_1}")
string(STRIP "${conf_path}" conf_path)

message(STATUS "     vocem-run:     ${run_path}")
message(STATUS "     environment.d: ${conf_path}")

if(NOT run_path STREQUAL conf_path)
    message(FATAL_ERROR
        "the two entry paths name different files at prefix ${prefix}:\n"
        "  vocem-run     ${run_path}\n"
        "  environment.d ${conf_path}\n"
        "Entry 16: a program started through vocem-run must be preloaded with the same shim the "
        "session preloads. One of the two is not derived from CMAKE_INSTALL_PREFIX.")
endif()

# And it has to be the prefix that was asked for, not a path that merely
# matches: two hardcoded copies of /usr would also compare equal.
if(NOT run_path MATCHES "^${prefix}/")
    message(FATAL_ERROR
        "both entry paths agree on ${run_path}, which is not under the prefix this configure "
        "asked for (${prefix}): they agree because neither follows the install prefix")
endif()
if(NOT run_path MATCHES "\\$LIB")
    message(FATAL_ERROR
        "${run_path} carries no \$LIB token, so one script cannot serve both architectures "
        "(the literal has to survive CMake's configure step)")
endif()

message("ok   vocem-run and environment.d preload one path, and it follows the install prefix")

# And vocem-run puts it FIRST. Of two OpenGL interposers only the one ahead can
# hand the frame on (tests/shim_chain.cpp); appended, `mangohud vocem-run game`
# left MangoHud ahead and this overlay never saw a frame. Run, not grepped: the
# generated script with another preload already set, printing what it hands
# the command. The shim it names does not exist at this prefix, so ld.so says
# "cannot be preloaded" on stderr and nothing is loaded.
find_program(printenv_program printenv)
if(NOT printenv_program)
    message(FATAL_ERROR "no printenv here, so the order vocem-run preloads in cannot be read")
endif()
execute_process(
    COMMAND sh "${run}" "${printenv_program}" LD_PRELOAD
    OUTPUT_VARIABLE handed
    ERROR_QUIET
    RESULT_VARIABLE status
    ENVIRONMENT_MODIFICATION "LD_PRELOAD=set:/opt/another-interposer/libother.so")
string(STRIP "${handed}" handed)
message(STATUS "     vocem-run hands the command LD_PRELOAD=${handed}")
if(NOT status EQUAL 0 OR NOT handed STREQUAL "${run_path}:/opt/another-interposer/libother.so")
    message(FATAL_ERROR
        "vocem-run did not put the shim ahead of a preload already set (exit ${status}): "
        "an OpenGL interposer ahead of us calls the driver and we never see a frame")
endif()

file(REMOVE_RECURSE "${tree}")
message("ok   vocem-run puts the shim ahead of whatever was already preloaded")
