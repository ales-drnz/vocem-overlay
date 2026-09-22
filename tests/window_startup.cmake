# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The window's first frame does not wait on systemctl.
#
# `openglPreloadActive` was a CONSTANT property whose getter spawned
# `systemctl --user show-environment` and waited up to three seconds for it,
# and the Debug page reads that property while the window is being built -- so
# a slow service manager was a slow first frame, paid by every launch, hidden
# or not. Nothing in the suite timed a startup (entry 104 says so): this does,
# differentially. Two runs of the same offscreen walk, each with a stub
# `systemctl` earlier on PATH (the vocem_why.cmake shape, so the owner's live
# manager is never asked anything): one stub answers at once, the other sleeps
# `SLOW_SECONDS` first. The difference between the two runs is what the spawn
# costs the window. Against the window as it stood the whole sleep lands in the
# first frame -- 2.5 s of it -- where the asynchronous probe costs the frame
# nothing and the page says "Checking" until the answer arrives.
#
# Expects CONFIG_BINARY and CMAKE_CURRENT_BINARY_DIR (a test directory).

# Run through ctest, or with -DCMAKE_CURRENT_BINARY_DIR=<build>/tests: in script
# mode the variable is otherwise the current working directory (entry 137).
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

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/window-startup")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache" "${scratch}/data"
     "${scratch}/fast" "${scratch}/slow")
file(WRITE "${scratch}/config/vocem/config.ini" "")

set(SLOW_SECONDS 2.5)
# The two stubs. `show-environment` is what the probe asks; anything else a
# window might ask of systemctl answers at once, so the only thing that differs
# between the runs is how long that one answer takes.
file(WRITE "${scratch}/fast/systemctl"
     "#!/bin/sh\nif [ \"$2\" = show-environment ]; then echo 'PATH=/usr/bin'; fi\nexit 0\n")
file(WRITE "${scratch}/slow/systemctl"
     "#!/bin/sh\nif [ \"$2\" = show-environment ]; then sleep ${SLOW_SECONDS}; echo 'PATH=/usr/bin'; fi\nexit 0\n")
file(CHMOD "${scratch}/fast/systemctl" "${scratch}/slow/systemctl"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Milliseconds since the epoch: CMake's own clock is whole seconds.
function(now_ms out)
    execute_process(COMMAND date +%s%3N OUTPUT_VARIABLE stamp OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out} "${stamp}" PARENT_SCOPE)
endfunction()

# One run of the window: the Debug section (which reads the preload property)
# three times, offscreen, with `${stub}` first on PATH and no shim in LD_PRELOAD (a
# hit there is answered without asking systemctl, which is not the case under
# measurement). Returns the run's wall time in milliseconds.
#
# Three grabs and not one, because the walk has to outlast the slow stub: a
# window that exits while `sleep` is still running leaves it holding the
# stderr pipe this function reads, and execute_process then waits for the
# sleep -- which would charge the fix with the very 2.5 s it removed. "8,8,8"
# is about 4.7 s; the walk was every section up to 8, 11 s, until the suite's
# review (DESIGN 193) took out the pages this test never reads.
function(time_run stub out)
    now_ms(before)
    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        ERROR_VARIABLE errors
        OUTPUT_QUIET
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${scratch}/${stub}"
            "LD_PRELOAD=unset:"
            "XDG_CONFIG_HOME=set:${scratch}/config"
            "XDG_CACHE_HOME=set:${scratch}/cache"
            "XDG_DATA_HOME=set:${scratch}/data"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_SECTIONS=set:8,8,8"
            "VOCEM_CONFIG_GEOMETRY=set:${scratch}/${stub}.json")
    now_ms(after)
    # window_status.cmake's rule, in a function that cannot use its macro
    # (return() would leave only this function, and the caller has a skip
    # protocol of its own through ${out}): a window that did not come back
    # cleanly is a failure either way, and that file carries the measurements
    # the rule rests on.
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR
            "the window did not come back from its run: ${status}. A skip here would be "
            "counted as a pass, and this is the window failing rather than the machine "
            "refusing to run it -- window_status.cmake says how that was measured.\n${errors}")
    endif()
    math(EXPR elapsed "${after} - ${before}")
    set(${out} "${elapsed}" PARENT_SCOPE)
endfunction()

# Fast first, then slow, then fast again: the second fast run is the control
# that says the difference is the stub's sleep and not the machine warming up.
time_run(fast fast_ms)
if(fast_ms STREQUAL "skip")
    return()
endif()
time_run(slow slow_ms)
if(slow_ms STREQUAL "skip")
    return()
endif()
time_run(fast fast_again_ms)
if(fast_again_ms STREQUAL "skip")
    return()
endif()

math(EXPR added "${slow_ms} - ${fast_ms}")
message("     window run, systemctl answering at once:      ${fast_ms} ms, then ${fast_again_ms} ms")
message("     window run, systemctl sleeping ${SLOW_SECONDS} s first: ${slow_ms} ms")
message("     the sleeping service manager added:            ${added} ms")

# Half the sleep is the line: a synchronous spawn adds all 2500 ms of it (the
# window as it stood), an asynchronous one adds none, and the walk's own
# jitter on this machine is a few hundred milliseconds either way.
if(added GREATER 1250)
    message(FATAL_ERROR
        "the window's run grew by ${added} ms when systemctl slept ${SLOW_SECONDS} s: the "
        "preload probe is waited for on the first frame's path")
endif()
message("ok   a slow service manager does not hold the first frame: the probe is asynchronous")
