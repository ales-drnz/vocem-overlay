# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The tool for the person having the problem must not print shell errors into
# its own answer.
#
# `systemctl is-active` reports a STATE, so its exit status is that state and
# not an error: "inactive" is printed on stdout together with exit 3, and a unit
# systemd has never heard of with exit 4. So
#
#     echo "daemon: $(systemctl --user is-active vocemd.service || echo unknown)"
#
# ran both branches and put two lines where one belongs -- and it did it exactly
# when the daemon is down, which is the situation somebody runs this tool in.
# Measured three times against the script as it stood:
#
#     daemon: inactive
#     unknown
#
# That output is pasted into bug reports. A line reading "unknown" under a line
# reading "daemon: inactive" makes the reader doubt both.
#
# **This is entry 99 happening again in the same file.** Entry 99 was `grep -c`,
# which prints 0 and exits 1, five lines further down; it was found, fixed and
# written up while this one stood untouched -- which is entry 33's lesson (fix
# the pattern, not the instance) unlearned inside the very file that taught it.
# The script had no test at all, and that is part of why.
#
# There is no skip in here and there is deliberately no dependency on the real
# daemon's state. The mechanism is reproduced with a stub `systemctl` earlier on
# PATH than the real one: it prints "inactive" and exits 3, which is what the
# real one does for a stopped unit. That makes the test hermetic, keeps it away
# from the owner's live daemon -- which may be serving a voice channel while this
# runs -- and means it measures the same thing on a machine with no systemd at
# all. `XDG_CACHE_HOME` is a scratch directory, so no real registry is read.

# WORK_DIR and not CMAKE_CURRENT_BINARY_DIR, which the other .cmake tests here
# are passed and which this one deliberately does not use: under `cmake -P` that
# name is a built-in bound to the current working directory, so a -D of it is
# accepted and ignored. It goes unnoticed in the others because ctest runs them
# with the build directory as their cwd, which is the value being passed anyway
# -- but run one by hand from the source tree and it writes its scratch there.
# This one did, once, and left a directory in the repository root.
set(work "${WORK_DIR}/vocem-why-test")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/bin")
file(MAKE_DIRECTORY "${work}/cache/vocem/apps")

# The stopped daemon, as systemd reports one: the word on stdout, and a
# non-zero status that means "inactive" rather than "something went wrong".
file(WRITE "${work}/bin/systemctl" "#!/bin/sh\necho inactive\nexit 3\n")

# The script also asks pacman whether the package is installed. Stubbed to
# "not installed" so the test says the same thing on any machine.
file(WRITE "${work}/bin/pacman" "#!/bin/sh\nexit 1\n")

if(CMAKE_VERSION VERSION_LESS 3.19)
    execute_process(COMMAND chmod +x "${work}/bin/systemctl" "${work}/bin/pacman")
else()
    file(CHMOD "${work}/bin/systemctl" "${work}/bin/pacman"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${work}/bin:$ENV{PATH}"
            "XDG_CACHE_HOME=${work}/cache"
            sh "${SOURCE_DIR}/scripts/vocem-why.sh"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE status)

message("---- what the tool printed ----")
message("${output}")
message("-------------------------------")

if(NOT status EQUAL 0)
    message("the script exited ${status}")
    message("${errors}")
    message(FATAL_ERROR "vocem-why.sh did not run to completion")
endif()

# The claim: the daemon's state is one line, and it is the state.
string(REGEX MATCHALL "\n?daemon: [^\n]*" daemon_lines "\n${output}")
list(LENGTH daemon_lines daemon_count)
if(NOT daemon_count EQUAL 1)
    message(FATAL_ERROR "expected exactly one 'daemon:' line, found ${daemon_count}")
endif()
list(GET daemon_lines 0 daemon_line)
string(STRIP "${daemon_line}" daemon_line)
if(NOT daemon_line STREQUAL "daemon: inactive")
    message(FATAL_ERROR "expected 'daemon: inactive', got '${daemon_line}'")
endif()

# And the fallback did not run alongside it. Against the defective script the
# word sits on its own line straight after the daemon's; asserting on the whole
# output rather than on that position keeps it failing if the stray line ever
# moves somewhere else.
if(output MATCHES "(^|\n)[ \t]*unknown[ \t]*(\n|$)")
    message("the fallback ran as well as the command, which is the defect:")
    message("${output}")
    message(FATAL_ERROR "'unknown' was printed on a line of its own")
endif()

# Nothing else in the file may acquire the same shape. The fault needs a first
# branch that PRINTS and then FAILS -- `systemctl is-active`, `grep -c` -- so the
# `[ ... ] && echo x || echo y` lines are sound and stay. This catches a new
# `$(command || echo ...)` whose command is one of the two known to do it.
file(STRINGS "${SOURCE_DIR}/scripts/vocem-why.sh" script_lines)
set(offenders "")
set(number 0)
foreach(line IN LISTS script_lines)
    math(EXPR number "${number} + 1")
    if(line MATCHES "^[ \t]*#")
        continue()
    endif()
    if(line MATCHES "\\$\\(" AND line MATCHES "\\|\\|[ \t]*echo"
       AND line MATCHES "(is-active|is-enabled|grep -c)")
        list(APPEND offenders "${number}: ${line}")
    endif()
endforeach()
if(offenders)
    message("a command that prints its answer AND exits non-zero, with a || echo fallback:")
    foreach(offender IN LISTS offenders)
        message("  ${offender}")
    endforeach()
    message(FATAL_ERROR "both branches run, and both land in the answer (entries 99 and 33)")
endif()

message("ok   the daemon's state is one line, and the fallback did not run beside it")
message("ok   and no other line in the script has that shape")
