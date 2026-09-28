# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A window that cannot listen on its single-instance socket says so, and does
# not swallow the next launch.
#
# The socket is $XDG_RUNTIME_DIR/vocem-config-<uid>, and a local socket's
# address holds 107 bytes. Past that, QLocalServer::listen() fails -- measured
# first as single_instance failing in a worktree whose build path is long --
# and main() did not look at the answer: the window ran unreachable and said
# nothing, and every later launch found the lock held, asked a socket that was
# never there, and exited 0 three seconds later having shown nothing.
#
# Here the runtime directory is made long on purpose. The first window must
# name the socket it could not create; a second launch must not be swallowed:
# the first gives the lock back, so the second opens a window of its own.
#
# Offscreen, scratch XDG directories, VOCEM_CONFIG_NO_DAEMON; the instances
# counted are the pids this script started. Expects CONFIG_BINARY and
# CMAKE_CURRENT_BINARY_DIR (a test directory).

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
include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/single-instance-long")
string(REPEAT "x" 120 long_name)
set(runtime "${scratch}/run/${long_name}")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache" "${scratch}/data" "${runtime}")
file(WRITE "${scratch}/config/vocem/config.ini" "")

file(WRITE "${scratch}/launch.sh" "#!/bin/sh
bin=\"$1\"
\"$bin\" >/dev/null 2>\"${scratch}/a.err\" &
a=$!
sleep 2
kill -0 $a 2>/dev/null && echo 'A alive'
start=$(date +%s%3N)
\"$bin\" >/dev/null 2>\"${scratch}/b.err\" &
b=$!
code=running
i=0
while [ $i -lt 50 ]; do
    if ! kill -0 $b 2>/dev/null; then wait $b; code=$?; break; fi
    sleep 0.1
    i=$((i+1))
done
echo \"B exit=$code after=$(( $(date +%s%3N) - start )) ms\"
kill $a $b 2>/dev/null
wait $a $b 2>/dev/null
exit 0
")

execute_process(
    COMMAND sh "${scratch}/launch.sh" "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE report
    ERROR_VARIABLE errors
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:"
        "LC_ALL=set:C.UTF-8"
        "XDG_RUNTIME_DIR=set:${runtime}"
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "XDG_DATA_HOME=set:${scratch}/data"
        "VOCEM_DRM_ROOT=set:${scratch}/data"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_NO_DAEMON=set:1")
message("${report}")
vocem_window_ran("${status}" "${errors}" "the launch script")
file(READ "${scratch}/a.err" a_err)
file(READ "${scratch}/b.err" b_err)
file(REMOVE_RECURSE "${scratch}/run")
if(NOT report MATCHES "A alive")
    message(STATUS "skip the first window did not stay up offscreen here: ${a_err}")
    return()
endif()
message("--  first window's stderr: ${a_err}")
message("--  second launch's stderr: ${b_err}")

set(failures 0)
macro(check text)
    if(${ARGN})
        message("ok   ${text}")
    else()
        message("FAIL ${text}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

string(REGEX MATCH "B exit=([a-z0-9]+)" _ "${report}")
set(b_exit "${CMAKE_MATCH_1}")
check("the first window says it cannot listen, naming the socket"
      a_err MATCHES "cannot listen on [^\n]*${long_name}/vocem-config-")
check("and that it runs without the single-instance guard"
      a_err MATCHES "without the single-instance guard")
check("a second launch is not swallowed: it opens a window of its own (${b_exit})"
      b_exit STREQUAL "running")

if(failures)
    message(FATAL_ERROR "a window without its socket ran unreachable and said nothing")
endif()
