# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The window binds to no private Qt symbol.
#
# qt_add_qml_module() compiles QML to C++ by default, and that C++ calls
# QQmlPrivate::AOTCompiledContext -- symbols versioned Qt_6_PRIVATE_API, which
# Qt promises nothing about across minor releases. Arch rebuilds every package
# of its own that uses them when Qt moves (qt6 goes through extra-testing for
# that reason); nothing rebuilds a package built here, so the first Qt minor
# after an install could leave the window unable to start. 0.1.12-2's
# /usr/bin/vocem-config carries two such references (readelf -V, measured
# 2026-09-29 with Qt 6.11.2 installed and 6.12 due the next day). The window is
# built bytecode-only now: bytecode is data, a Qt that does not recognise it
# loads the .qml sources embedded beside it, and the window's startup was the
# same within the noise (five runs a side, 1674-1703 ms against 1690-1780).
#
# The version tag of every symbol the binary needs is read, so the check holds
# for any private symbol, not only the two that were there.
#
# Expects CONFIG_BINARY.

if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()
find_program(READELF readelf)
if(NOT READELF)
    message(STATUS "skip readelf is missing")
    return()
endif()

execute_process(
    COMMAND "${READELF}" -W -V "${CONFIG_BINARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE versions
    ERROR_VARIABLE errors
    ENVIRONMENT_MODIFICATION "LC_ALL=set:C")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "readelf could not read ${CONFIG_BINARY}: ${errors}")
endif()

# The needed versions, from .gnu.version_r ("Name: Qt_6_PRIVATE_API").
string(REGEX MATCHALL "Name: [A-Za-z0-9_.]+" names "${versions}")
set(private "")
set(qt_seen FALSE)
foreach(name IN LISTS names)
    string(REPLACE "Name: " "" name "${name}")
    if(name MATCHES "^Qt_")
        set(qt_seen TRUE)
    endif()
    if(name MATCHES "PRIVATE")
        list(APPEND private "${name}")
    endif()
endforeach()
if(NOT qt_seen)
    message(FATAL_ERROR "no Qt symbol version found in ${CONFIG_BINARY}: this test "
                        "reads nothing and would pass against anything")
endif()
if(private)
    list(REMOVE_DUPLICATES private)
    execute_process(COMMAND nm -D --with-symbol-versions --undefined-only "${CONFIG_BINARY}"
                    OUTPUT_VARIABLE undefined)
    string(REGEX MATCHALL "[^\n ]+@[A-Za-z0-9_]*PRIVATE[A-Za-z0-9_]*" symbols "${undefined}")
    message(FATAL_ERROR
        "${CONFIG_BINARY} needs ${private}: ${symbols} -- Qt's private API, kept "
        "only within one minor release, so the next Qt can leave the window unable "
        "to start; build the QML bytecode-only (gui/CMakeLists.txt)")
endif()
message(STATUS "ok the window needs Qt's public symbols only")
