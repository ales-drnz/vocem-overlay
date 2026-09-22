# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# What an idle window costs, in counts.
#
# The bridge's tick emitted stateChanged twice a second whatever the daemon did,
# and every binding on it re-evaluated: `segmentAbiVersion` (an shm_open and a
# pread), `displays()` (a list built from the enumeration), the header's four
# sentences -- for the life of a tray process nobody was looking at. And the
# four-second sweep looked an application's icon up by walking every desktop
# entry on the machine, twice, with a QFileInfo built per candidate:
# O(applications x entries) per sweep, and again on every click of a switch.
#
# Counts, not a clock (tests/apps_cost.cpp's rule): the bridge counts what it
# does and the geometry harness prints the counts as the dump's last line
# (ConfigBridge::counters). A fixture of ENTRIES fake desktop entries and
# RECORDS records, under scratch data and cache roots so the machine's own
# registry is not in the number, and a walk of ten steps of the Games page --
# about eleven seconds, so twenty-odd ticks and three or four sweeps. Against
# the window as it stood: stateChanged once per tick, and every lookup
# examining every entry twice.
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

set(ENTRIES 200)
set(RECORDS 20)
set(scratch "${CMAKE_CURRENT_BINARY_DIR}/window-cost")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem" "${scratch}/cache/vocem/apps"
     "${scratch}/data/applications" "${scratch}/empty")
file(WRITE "${scratch}/config/vocem/config.ini" "")
math(EXPR last_entry "${ENTRIES} - 1")
foreach(i RANGE ${last_entry})
    # Every entry a wrapper in front of its program, the shape 44 of this
    # machine's 53 Game entries have.
    file(WRITE "${scratch}/data/applications/game${i}.desktop"
         "[Desktop Entry]\nType=Application\nName=Game ${i}\nExec=env FOO=1 game${i} %U\n"
         "Icon=applications-games\nCategories=Game;\n")
endforeach()
math(EXPR last_record "${RECORDS} - 1")
foreach(i RANGE ${last_record})
    file(WRITE "${scratch}/cache/vocem/apps/game${i}"
         "name = game${i}\nexecutable = /opt/game${i}/game${i}\napi = opengl\ngame = true\n"
         "why = entry:game${i}.desktop\nseen = 1788700000\n")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/window_status.cmake")

# A /dev/shm of the window's own, so "nothing changed for the whole run" below
# is a fact and not a hope: the window reads the daemon's segment on its tick,
# and the owner's daemon is running while this suite runs. Measured with a
# segment whose participant count changed twice during the walk: three
# stateChanged emits, and a FAIL that said the window was wasteful when the
# daemon had simply spoken (entry 200). debug_section.cmake does the same for
# the same reason.
find_program(BWRAP_BINARY bwrap)
if(NOT BWRAP_BINARY)
    message(STATUS "skip bwrap is missing, so the window cannot be given a /dev/shm of its own")
    return()
endif()

execute_process(
    COMMAND "${BWRAP_BINARY}" --dev-bind / / --tmpfs /dev/shm --die-with-parent
            "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    ERROR_VARIABLE errors
    OUTPUT_QUIET
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "LD_PRELOAD=unset:"
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "XDG_DATA_HOME=set:${scratch}/data"
        "XDG_DATA_DIRS=set:${scratch}/empty"
        "VOCEM_DRM_ROOT=set:${scratch}/empty"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:5,5,5,5,5,5,5,5,5,5"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")
vocem_window_ran("${status}" "${errors}")

file(READ "${scratch}/geometry.json" dump)
if(NOT dump MATCHES "\"counters\": ({[^}]*})")
    message(FATAL_ERROR "the dump carries no counters line: the window does not count what it does")
endif()
set(counters "${CMAKE_MATCH_1}")

function(counter name out)
    if(counters MATCHES "\"${name}\": ([0-9]+)")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "no counter named ${name} in ${counters}")
    endif()
endfunction()
counter(stateChangedEmits emits)
counter(abiPeeks peeks)
counter(displayReads display_reads)
counter(procWalks proc_walks)
counter(applicationSweeps sweeps)
counter(entryScans scans)
counter(iconLookups lookups)
counter(entriesExamined examined)
counter(desktopEntries entries)

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

message("     ${sweeps} sweeps of the registry; ${emits} stateChanged emits; ${peeks} ABI peeks; "
        "${display_reads} display reads; ${proc_walks} /proc walks; ${scans} desktop-entry scans "
        "over ${entries} entries; ${lookups} icon lookups examining ${examined} entries")

check("the fixture's ${ENTRIES} entries are the ones scanned, and nothing of the machine's"
      entries EQUAL ${ENTRIES})
check("the walk was long enough for the sweep to run more than twice (${sweeps})"
      sweeps GREATER_EQUAL 3)
# Nothing changed for the whole run -- no daemon in the window's /dev/shm, no
# display, the same records -- so the state was announced once: at the first
# tick, and never again.
check("an idle window announces its state once, not once per tick (${emits})"
      emits LESS_EQUAL 2)
# The segment and the display tree are asked on the sweep, not on the tick.
math(EXPR sweep_cap "${sweeps} + 1")
check("the segment's ABI is asked once per sweep at most (${peeks})"
      peeks LESS_EQUAL ${sweep_cap})
check("and so is the display tree (${display_reads})"
      display_reads LESS_EQUAL ${sweep_cap})
# One scan of the entries: nothing turned up that the first scan did not
# account for, so the tree was not walked again.
check("the desktop entries were walked once (${scans})"
      scans EQUAL 1)
# The indexed lookup: each of the RECORDS applications per sweep examines the
# entries carrying its name -- one here -- and not the whole registry.
math(EXPR lookups_expected "${RECORDS} * ${sweeps}")
check("one lookup per application per sweep (${lookups})"
      lookups EQUAL ${lookups_expected})
math(EXPR examined_cap "${lookups} * 2")
math(EXPR examined_before "${lookups} * ${ENTRIES} * 2")
check("a lookup examines the entries carrying its name and no others: ${examined} examined, where the walk over every entry twice would be ${examined_before}"
      examined LESS_EQUAL ${examined_cap})

if(failures)
    message(FATAL_ERROR "the window's idle cost is not what it should be")
endif()
