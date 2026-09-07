# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# One window per user, when two are launched at once.
#
# The single-instance guard was a socket: a launch asked whoever was listening
# to show itself and, hearing nobody, built a window and listened. The ask ran
# before the window was built and the listen after, and building the window
# takes a good fraction of a second -- so two launches inside that window (the
# autostart at login and a menu click) each heard nobody, each built a window,
# and each ran removeServer() before listening: the second unlinked the
# first's socket. Two processes, two tray icons, the first unreachable by a
# third launch. A lock file taken before the engine loads decides it now, and
# the loser waits for the winner's socket rather than giving up on it.
#
# Two instances 100 ms apart, offscreen, under a runtime directory of this
# test's own so the owner's real window is not asked anything and the socket
# is nobody else's. VOCEM_CONFIG_NO_DAEMON keeps both away from the session's
# services while leaving the startup path -- which is the subject -- as a real
# launch has it. Against the window as it stood, both instances are alive four
# seconds later. Needs a shell, python3 is not needed.
#
# Expects CONFIG_BINARY and CMAKE_CURRENT_BINARY_DIR (a test directory).

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest.")
endif()
if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/single-instance")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache" "${scratch}/data" "${scratch}/run")
file(WRITE "${scratch}/config/vocem/config.ini" "")

# The script: A in the background, B 100 ms later, C once B has answered. What
# it prints is the evidence; the checks below read it.
# The script: A in the background, B 100 ms later, C once B has answered. B
# and C are waited for with a bound, because against the window as it stood B
# never exits -- it IS the second window -- and a script that waited on it
# would hang instead of failing. What it prints is the evidence; the checks
# below read it. The instances counted are the pids this script started, not a
# pgrep over the machine: the owner's own window may be running beside this.
file(WRITE "${scratch}/race.sh" "#!/bin/sh
bin=\"$1\"
# Runs \"$bin\" in the background and waits up to five seconds for it to
# exit; prints its exit code, or `running` when it is still there.
bounded() {
    label=\"$1\"
    start=$(date +%s%3N)
    \"$bin\" >/dev/null 2>\"${scratch}/$label.err\" &
    child=$!
    code=running
    i=0
    while [ $i -lt 50 ]; do
        if ! kill -0 $child 2>/dev/null; then wait $child; code=$?; break; fi
        sleep 0.1
        i=$((i+1))
    done
    echo \"$label exit=$code after=$(( $(date +%s%3N) - start )) ms\"
    eval \"\${label}_pid=$child\"
}
\"$bin\" >/dev/null 2>\"${scratch}/a.err\" &
a=$!
sleep 0.1
bounded B
sleep 1
alive=0
kill -0 $a 2>/dev/null && alive=$((alive+1)) && echo 'A alive'
kill -0 $B_pid 2>/dev/null && alive=$((alive+1)) && echo 'B alive'
echo \"instances alive: $alive\"
socket=\"${scratch}/run/vocem-config-$(id -u)\"
if [ -S \"$socket\" ]; then echo 'socket present'; else echo 'socket missing'; fi
bounded C
kill $a $B_pid $C_pid 2>/dev/null
wait $a $B_pid $C_pid 2>/dev/null
exit 0
")

execute_process(
    COMMAND sh "${scratch}/race.sh" "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE report
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:"
        "XDG_RUNTIME_DIR=set:${scratch}/run"
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "XDG_DATA_HOME=set:${scratch}/data"
        "VOCEM_DRM_ROOT=set:${scratch}/data"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_NO_DAEMON=set:1")
message("${report}")
if(NOT status EQUAL 0)
    message(STATUS "skip the race could not be run here: ${status} ${errors}")
    return()
endif()
if(NOT report MATCHES "A alive")
    file(READ "${scratch}/a.err" a_err)
    message(STATUS "skip the first window did not stay up offscreen here: ${a_err}")
    return()
endif()

set(failures 0)
# The condition follows the text as separate arguments, because a condition
# handed over in one string reaches if() as a constant (and a constant that is
# not a true-word is false: the first version of this failed every check while
# printing the right numbers beside each).
macro(check text)
    if(${ARGN})
        message("ok   ${text}")
    else()
        message("FAIL ${text}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

string(REGEX MATCH "B exit=([a-z0-9]+) after=([0-9]+) ms" _ "${report}")
set(b_exit "${CMAKE_MATCH_1}")
set(b_ms "${CMAKE_MATCH_2}")
string(REGEX MATCH "instances alive: ([0-9]+)" _ "${report}")
set(alive "${CMAKE_MATCH_1}")
string(REGEX MATCH "C exit=([a-z0-9]+) after=([0-9]+) ms" _ "${report}")
set(c_exit "${CMAKE_MATCH_1}")
set(c_ms "${CMAKE_MATCH_2}")

check("the second launch, 100 ms after the first, exits 0 (${b_exit})"
      b_exit STREQUAL "0")
check("and does so without building a window of its own (${b_ms} ms)"
      b_ms LESS 5000)
check("a second later exactly one instance is alive (${alive})"
      alive EQUAL 1)
check("and its socket is still there: the loser did not unlink it"
      report MATCHES "socket present")
check("a third launch reaches the first at once (${c_ms} ms)"
      c_exit STREQUAL "0" AND c_ms LESS 2000)

if(failures)
    message(FATAL_ERROR "two launches inside the window's own startup became two windows")
endif()
