# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The daemon's unit is confined, and confined in the ways that actually start.
#
# vocemd is the one process here that parses input from the network -- WebSocket
# framing, JSON, a PNG off a CDN -- and it holds a token whose scopes include
# messages.read. Until this test existed the unit's only protective directive was
# MemoryMax, so a defect in any of those parsers had the user's whole session.
#
# The second half of the test is the more interesting one. An empty
# `CapabilityBoundingSet=` is the first line of every hardening recipe on the
# internet, and on a *user* unit it makes the service fail to start outright:
# measured with `systemd-run --user`, exit 218 (EXIT_CAPABILITIES), three runs out
# of three, while every other directive below ran a real HTTPS fetch to
# completion. A daemon that will not start is worse than one that is not confined,
# so its absence is asserted rather than left to be re-added by somebody reading
# the same recipe.

set(unit "${SOURCE_DIR}/packaging/systemd/vocemd.service")
if(NOT EXISTS "${unit}")
    message(FATAL_ERROR "no unit at ${unit}")
endif()
file(READ "${unit}" raw)

# Comments out first. `string(FIND)` over the whole file is satisfied by a
# directive somebody has commented out: the first version of this test passed
# against a unit with every one of them prefixed with a `#`, and printed that the
# unit was confined. A test that measures the presence of a string is not a test.
string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" text "${raw}")

set(required
    "NoNewPrivileges=yes"
    "SystemCallArchitectures=native"
    "SystemCallFilter=@system-service"
    "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6"
    "RestrictNamespaces=yes"
    "RestrictRealtime=yes"
    "RestrictSUIDSGID=yes"
    "LockPersonality=yes"
    "MemoryDenyWriteExecute=yes"
    "ProtectClock=yes"
    "ProtectKernelTunables=yes"
    "ProtectKernelModules=yes"
    # A stop that runs past its timeout ends in SIGKILL, which skips the unlink
    # that takes the shared segment away with the daemon.
    "TimeoutStopSec=")

set(missing "")
foreach(directive IN LISTS required)
    # Anchored to the start of a line, so only a directive systemd will read
    # counts. None of the names below carries a regex metacharacter -- `@` and
    # `=` are literal -- so they go into the pattern as they are.
    if(NOT text MATCHES "(^|\n)[ \t]*${directive}")
        list(APPEND missing "${directive}")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR
        "the daemon's unit has lost hardening it had: ${missing}")
endif()

# The negation sets must each carry their own leading ~: on one line the ~ binds
# to the first name only and systemd ignores the rest as unknown syscalls, which
# `systemd-analyze verify` reports and nothing else does.
if(text MATCHES "SystemCallFilter=~[^\n]*[ \t]~?@[a-z]+[ \t]+~?@")
    message(FATAL_ERROR
        "a SystemCallFilter negation line carries more than one set: the ~ binds "
        "to the first name only and the rest are ignored")
endif()

if(text MATCHES "\n[ \t]*CapabilityBoundingSet=")
    message(FATAL_ERROR
        "CapabilityBoundingSet= is back in the unit: on a user service an empty "
        "bounding set fails to start (exit 218, measured three times with "
        "systemd-run --user), so the daemon would simply never come up")
endif()

# And the file has to be a unit systemd will actually load.
find_program(SYSTEMD_ANALYZE systemd-analyze)
if(SYSTEMD_ANALYZE)
    execute_process(COMMAND "${SYSTEMD_ANALYZE}" --user verify "${unit}"
                    RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "systemd-analyze rejects the unit: ${errors}")
    endif()
    if(errors MATCHES "not known, ignoring")
        message(FATAL_ERROR
            "systemd is ignoring part of the unit: ${errors}")
    endif()
    message(STATUS "the unit is confined and systemd-analyze accepts it whole")
else()
    message(STATUS "systemd-analyze is not installed; the directives were still checked")
endif()
