# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# What a window run's RESULT_VARIABLE means, in one place.
#
# Every window-driving script had this, hand-written, sixteen times:
#
#     if(NOT status EQUAL 0)
#         message(STATUS "skip the window could not run here: ${status} ${errors}")
#         return()
#     endif()
#
# and `execute_process` does not put an exit code in that variable when the
# child did not exit. Measured with this machine's cmake in a scratch
# directory: `status` holds the **string** `Segmentation fault` for a killed
# child and `Process terminated due to timeout` for one that hung, and
# `NOT <string> EQUAL 0` is true, so both took the skip branch. Skips do not
# affect ctest's exit status -- so `cd build && ctest`, which is this project's
# stated merge gate, returned **0** with a settings window that segfaulted on
# startup. A QML type error, a null dereference in ConfigBridge, entry 140's
# race returning as a deadlock: sixteen of the project's most expensive
# instruments went dark at once and the summary said `0 failed`.
#
# The legitimate skip -- the window was not built -- is already handled one
# block earlier in every one of those scripts, by
# `if(NOT EXISTS "${CONFIG_BINARY}")`. So this branch is reachable only for a
# window that exists and did not come back cleanly, and **neither case is a
# skip**, which took a measurement to settle rather than a guess:
#
#   * anything that is not a number is a signal or a timeout. A crash or a hang
#     is never a property of the environment.
#   * a number is an exit code, and the guess was that an environment which
#     cannot run the window lands here. Measured on this machine, three ways:
#     with QT_QPA_PLATFORM naming a plugin that does not exist the window
#     **aborts** (134 through a shell, a signal to execute_process, and not one
#     byte on stderr); with no DISPLAY and no WAYLAND_DISPLAY at all it exits
#     **0** and writes its dump; and a QML binding on a null object -- one was
#     written into SettingRow.qml during this very pass -- makes it exit **1**
#     silently. So on this machine a non-zero exit is the window deciding to
#     fail, which is news, and the environment's own failure is a signal.
#
# Residual, said rather than left: on a machine where Qt exits instead of
# aborting, this reports a red where the old code reported a skip. That red is
# a true statement -- the window did not run -- and the suite's own discipline
# is that a skip on this machine is a regression to look at, so the louder of
# the two is the right default. The stderr is printed either way.
#
# Entry 123 is this defect's own precedent: twelve scripts reported a skip as a
# PASS because `cmake -P` exits 0 after `return()`, and the fix was one
# property set centrally in VocemTest.cmake. This is the same shape one layer
# in -- the spelling of the skip itself -- so it is also fixed in one place,
# the way gl_probe_witness.cmake is shared between the two Steam scripts (entry
# 125) rather than copied.
#
# A macro and not a function, deliberately: `return()` has to leave the
# including script, which is what every call site already does.
#
# The optional third argument names what was run, for the one script here whose
# `execute_process` child is not the window itself: `single_instance.cmake`
# runs a shell script that starts three windows and always ends `exit 0`. That
# script was the **seventeenth** and entry 166's sweep did not reach it -- it
# kept the hand-written shape verbatim, so a race that hung reported a skip and
# `ctest` exited 0, which is the whole of the defect entry 166 is about, in a
# script the entry says was swept.

macro(vocem_window_ran _result _errors)
    set(_subject "the window")
    if(${ARGC} GREATER 2)
        set(_subject "${ARGV2}")
    endif()
    if(NOT "${_result}" STREQUAL "0")
        if("${_result}" MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "${_subject} exited ${_result}. It ran and decided to fail, which is news "
                "rather than an environment that cannot run it -- measured of the window "
                "itself: a missing platform plugin aborts, and no display at all is exit 0. "
                "This used to be reported as a skip, which ctest counts as a pass."
                "\n${_errors}")
        endif()
        message(FATAL_ERROR
            "${_subject} did not come back from its run: ${_result}.\n"
            "That is a signal or a timeout, not an exit code, so it is a crash or a hang -- "
            "and it used to be reported as a skip, which ctest counts as a pass.\n"
            "${_errors}")
    endif()
endmacro()
