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
# all. `XDG_CACHE_HOME` and `XDG_CONFIG_HOME` are scratch directories, so no
# real registry and no real settings are read.

# WORK_DIR and not CMAKE_CURRENT_BINARY_DIR, which the other .cmake tests here
# are passed and which this one deliberately does not use: under `cmake -P` that
# name is a built-in bound to the current working directory, so a -D of it is
# accepted and ignored. It goes unnoticed in the others because ctest runs them
# with the build directory as their cwd, which is the value being passed anyway
# -- but run one by hand from the source tree and it writes its scratch there.
# This one did, once, and left a directory in the repository root.
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
            "XDG_CONFIG_HOME=${work}/config"
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

# --- and the half that was never run at all ---------------------------------
#
# Everything above drives the script with NO arguments, so the per-process
# report -- eighty lines of it, where both of the faults this file is named for
# lived and where a third was found on 2026-09-19 -- never executed. Two
# targets, both hermetic:
#
#   * a `sleep` this test starts, which is the ordinary case: a live process of
#     this user's, whose map can be read.
#   * `kthreadd`, pid 2 on every Linux, a kernel thread with no memory map and
#     another uid. That is the case the third fault was about: `grep -c` over a
#     map it cannot read counts nothing, so `found=0`, so the report printed
#     `shim loaded: NO`, `GL overlay: no`, `Vulkan layer: no` and then the
#     confident sentence "we are not inside it at all: a sandbox (Flatpak/Snap),
#     or the program started before the preload existed in the session" --
#     measured against pid 1. An unreadable map and a map with nothing of ours
#     in it were one answer, in the one tool whose whole purpose is to tell
#     "never started" from "ran for an hour without our code reaching it".
#
# Both legs also assert the script's STDERR is empty: it used to exit 0 while
# printing "Permesso negato" into the middle of its own answer, because a
# redirect fails in the shell and `2>/dev/null` on the command does not cover
# it -- and this test captured stderr and printed it only on a non-zero exit.

function(run_why target out_output out_errors)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${work}/bin:$ENV{PATH}"
                "XDG_CACHE_HOME=${work}/cache"
                "XDG_CONFIG_HOME=${work}/config"
                sh "${SOURCE_DIR}/scripts/vocem-why.sh" "${target}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE complaints
        RESULT_VARIABLE ignored
        TIMEOUT 60)
    set(${out_output} "${captured}" PARENT_SCOPE)
    set(${out_errors} "${complaints}" PARENT_SCOPE)
endfunction()

# Leg 1: this very script's own process. `cmake -P` is what is running these
# lines, so `vocem-why.sh cmake` is guaranteed a live process of this user's
# with a readable map, and nothing has to be started or cleaned up -- a
# background `sleep` did not survive execute_process's own child handling,
# which is the sort of thing that makes a fixture flaky rather than hermetic.
run_why("cmake" own_output own_errors)
if(NOT own_output MATCHES "shim loaded:")
    message("${own_output}")
    message(FATAL_ERROR
        "the per-process report never ran for a live process of this user's, so the half of "
        "the script this test exists for was not measured")
endif()
if(own_output MATCHES "cannot read")
    message("${own_output}")
    message(FATAL_ERROR
        "the script could not read the map of a process this test started itself: the "
        "readability check is refusing something it can read")
endif()
if(NOT own_errors STREQUAL "")
    message("${own_errors}")
    message(FATAL_ERROR "the script wrote to stderr while reporting on a live process")
endif()
message("ok   the per-process report runs for an ordinary process, in silence")

# Leg 1b: what the USER decided, which the overlay obeys before anything the
# report above measures. The script never looked: not at VOCEM_DISABLE in the
# process's environment, not at the master switch `enabled`, not at whether
# hidden_apps or shown_apps names the process -- 0 references to any of them
# (measured on 0.1.10) -- so a game the user had hidden came out as "shim
# loaded: yes, GL overlay: yes" with nothing to say why it drew nothing. The
# target is a copy of `sleep` named whyprobe, started in the same shell as the
# script with VOCEM_DISABLE=1, and a scratch config.ini that switches the
# overlay off and both hides and shows it (hidden wins, as in draw_here).
find_program(SLEEP_BINARY sleep)
if(NOT SLEEP_BINARY)
    message(FATAL_ERROR "no sleep binary to make a target of")
endif()
file(MAKE_DIRECTORY "${work}/config/vocem" "${work}/target")
file(WRITE "${work}/config/vocem/config.ini"
     "# the user's own\nenabled = false\nhidden_apps = other, whyprobe\nshown_apps = whyprobe\n")
execute_process(COMMAND cp "${SLEEP_BINARY}" "${work}/target/whyprobe")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${work}/bin:$ENV{PATH}"
            "XDG_CACHE_HOME=${work}/cache"
            "XDG_CONFIG_HOME=${work}/config"
            sh -c "VOCEM_DISABLE=1 '${work}/target/whyprobe' 30 & target=$!; sleep 0.3; sh '${SOURCE_DIR}/scripts/vocem-why.sh' whyprobe; kill $target"
    OUTPUT_VARIABLE switches_output
    ERROR_VARIABLE switches_errors
    RESULT_VARIABLE ignored
    TIMEOUT 60)
message("---- the user's switches ----")
message("${switches_output}")
if(NOT switches_output MATCHES "== whyprobe")
    message(FATAL_ERROR "the script did not report on the target it was given")
endif()
if(NOT switches_output MATCHES "VOCEM_DISABLE:[ \t]*1")
    message(FATAL_ERROR "the report does not say the process runs with VOCEM_DISABLE=1")
endif()
if(NOT switches_output MATCHES "enabled = false")
    message(FATAL_ERROR "the report does not say the master switch is off")
endif()
if(NOT switches_output MATCHES "hidden_apps:[^\n]*whyprobe")
    message(FATAL_ERROR "the report does not say hidden_apps names the process")
endif()
if(NOT switches_output MATCHES "shown_apps:[^\n]*whyprobe")
    message(FATAL_ERROR "the report does not say shown_apps names the process too")
endif()
if(NOT switches_errors STREQUAL "")
    message("${switches_errors}")
    message(FATAL_ERROR "the script wrote to stderr while reading the user's switches")
endif()
message("ok   and says what the user decided: VOCEM_DISABLE, the master switch, the two lists")

# Leg 1c: the two readings the report got wrong. The Vulkan loader drops the
# layer when VOCEM_DISABLE is SET, whatever its value (the manifest's
# disable_environment; measured with VK_LOADER_DEBUG=layer: unset inserts the
# layer, 1, 0, empty, 10 and no insert nothing), while the report said of "0"
# only "the OpenGL path ignores it" and of an empty value "(not set)". And
# `enabled =` with nothing after it is OFF for config.h's as_bool, while the
# report said "on (no 'enabled' line: the default)".
file(WRITE "${work}/config/vocem/config.ini" "enabled =\n")
execute_process(COMMAND cp "${SLEEP_BINARY}" "${work}/target/whyzero")
execute_process(COMMAND cp "${SLEEP_BINARY}" "${work}/target/whyempty")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${work}/bin:$ENV{PATH}"
            "XDG_CACHE_HOME=${work}/cache"
            "XDG_CONFIG_HOME=${work}/config"
            sh -c "VOCEM_DISABLE=0 '${work}/target/whyzero' 30 & zero=$!; VOCEM_DISABLE= '${work}/target/whyempty' 30 & empty=$!; sleep 0.3; sh '${SOURCE_DIR}/scripts/vocem-why.sh' whyzero; sh '${SOURCE_DIR}/scripts/vocem-why.sh' whyempty; kill $zero $empty"
    OUTPUT_VARIABLE values_output
    ERROR_VARIABLE values_errors
    RESULT_VARIABLE ignored
    TIMEOUT 60)
message("---- VOCEM_DISABLE=0, VOCEM_DISABLE= and enabled = ----")
message("${values_output}")
if(NOT values_output MATCHES "== whyzero" OR NOT values_output MATCHES "== whyempty")
    message(FATAL_ERROR "the script did not report on both targets it was given")
endif()
set(values_problems "")
if(values_output MATCHES "no 'enabled' line")
    list(APPEND values_problems "an empty 'enabled =' line was read as no line at all, and called on")
endif()
if(NOT values_output MATCHES "overlay switch:[ \t]*OFF")
    list(APPEND values_problems "an empty 'enabled =' is off for the overlay and the report does not say OFF")
endif()
# Each target's part of the output: whyzero was asked about first.
string(FIND "${values_output}" "== whyzero" zero_at)
string(FIND "${values_output}" "== whyempty" empty_at)
math(EXPR zero_length "${empty_at} - ${zero_at}")
string(SUBSTRING "${values_output}" ${zero_at} ${zero_length} zero_report)
string(SUBSTRING "${values_output}" ${empty_at} -1 empty_report)
if(NOT zero_report MATCHES "VOCEM_DISABLE:[^\n]*Vulkan layer is switched off")
    list(APPEND values_problems "VOCEM_DISABLE=0 turns the Vulkan layer off and the report does not say so")
endif()
if(empty_report MATCHES "VOCEM_DISABLE:[ \t]*\\(not set\\)")
    list(APPEND values_problems "an empty VOCEM_DISABLE is set, and the report says '(not set)'")
endif()
if(NOT empty_report MATCHES "VOCEM_DISABLE:[^\n]*Vulkan layer is switched off")
    list(APPEND values_problems "an empty VOCEM_DISABLE turns the Vulkan layer off and the report does not say so")
endif()
if(NOT values_errors STREQUAL "")
    list(APPEND values_problems "the script wrote to stderr: ${values_errors}")
endif()
if(values_problems)
    foreach(problem IN LISTS values_problems)
        message("FAIL ${problem}")
    endforeach()
    message(FATAL_ERROR "the report reads the switches differently from what obeys them")
endif()
message("ok   and reads VOCEM_DISABLE as the loader does, and 'enabled =' as config.h does")

# Leg 2: a kernel thread -- no map, another uid.
run_why("kthreadd" kernel_output kernel_errors)
if(NOT kernel_output MATCHES "kthreadd")
    message(STATUS "skip there is no kthreadd here to ask about")
    return()
endif()
if(NOT kernel_output MATCHES "shim loaded:[ 	]*unknown")
    message("${kernel_output}")
    message(FATAL_ERROR
        "a process whose memory map cannot be read is reported as though it had been read: "
        "an unreadable map and a map with nothing of ours in it are not the same answer, and "
        "this tool exists to tell them apart")
endif()
if(kernel_output MATCHES "we are not inside it at all")
    message("${kernel_output}")
    message(FATAL_ERROR
        "the script printed its confident 'we are not inside it at all' diagnosis about a "
        "process it could not look inside (entries 99 and 116, third round)")
endif()
if(NOT kernel_errors STREQUAL "")
    message("${kernel_errors}")
    message(FATAL_ERROR "the script wrote to stderr while reporting on a kernel thread")
endif()
message("ok   and says 'unknown' about a process it cannot look inside, rather than 'no'")
