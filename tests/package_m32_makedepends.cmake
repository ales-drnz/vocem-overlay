# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# What the 32-bit configure links, makepkg installs first.
#
# Since 0.1.11 the injected libraries carry their C++ runtime inside
# (-static-libstdc++ -static-libgcc), so at run time they NEED lib32-glibc
# alone and lib32-gcc-libs left depends=(). But the build still needs it:
# CMake's compiler check links a plain `c++ -m32` executable before anything
# of ours is compiled, and that link opens /usr/lib32/libgcc_s.so.1 and
# libstdc++.so, which lib32-gcc-libs owns. Measured in bwrap with /usr/lib32
# minus lib32-gcc-libs' 37 files: the -m32 configure fails. With the package
# in neither array, makepkg -s on a machine without it fails there.
#
# The claim: every file a plain `c++ -m32` link opens (-Wl,-t) belongs to a
# package in depends=() or makedepends=() of both PKGBUILDs, or to base-devel,
# which makepkg assumes (its direct dependencies, read from pacman).
# Skips without pacman, or where this machine cannot link -m32 at all.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
find_program(PACMAN pacman)
find_program(CXX_DRIVER NAMES c++ g++)
if(NOT PACMAN OR NOT CXX_DRIVER)
    message(STATUS "skip pacman or a C++ compiler driver is missing")
    return()
endif()

function(array_members file name out)
    file(READ "${file}" text)
    string(REGEX MATCH "\n${name}=\\(([^)]*)\\)" _ "${text}")
    set(body "${CMAKE_MATCH_1}")
    string(REPLACE ";" "<semicolon>" body "${body}")
    string(REPLACE "\n" ";" lines "${body}")
    set(kept "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        string(REGEX REPLACE "^[ \t]*'([^':]*).*$" "\\1" line "${line}")
        string(STRIP "${line}" line)
        if(NOT line STREQUAL "")
            list(APPEND kept "${line}")
        endif()
    endforeach()
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/package_m32_makedepends")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}")
file(WRITE "${scratch}/probe.cpp" "int main() { return 0; }\n")
# The same link CMake's compiler check makes for CMAKE_CXX_FLAGS=-m32.
execute_process(COMMAND "${CXX_DRIVER}" -m32 probe.cpp -o probe -Wl,-t
    WORKING_DIRECTORY "${scratch}"
    RESULT_VARIABLE linked OUTPUT_VARIABLE trace ERROR_VARIABLE trace_err
    ENVIRONMENT_MODIFICATION "LC_ALL=set:C" "LD_PRELOAD=unset:")
if(NOT linked EQUAL 0)
    message(STATUS "skip this machine cannot link a -m32 executable: ${trace_err}")
    return()
endif()

string(REPLACE "\n" ";" opened "${trace}${trace_err}")
set(owners "")
set(files 0)
foreach(line IN LISTS opened)
    string(STRIP "${line}" line)
    if(NOT line MATCHES "^/" OR line MATCHES "^/tmp/" OR line MATCHES "^${scratch}")
        continue()
    endif()
    file(REAL_PATH "${line}" real)
    execute_process(COMMAND "${PACMAN}" -Qqo "${real}"
        OUTPUT_VARIABLE owner OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        ENVIRONMENT_MODIFICATION "LC_ALL=set:C")
    math(EXPR files "${files} + 1")
    if(owner STREQUAL "")
        message("?    ${real} belongs to no package")
        continue()
    endif()
    message("     ${real} <- ${owner}")
    list(APPEND owners "${owner}")
endforeach()
list(REMOVE_DUPLICATES owners)
if(files LESS 5)
    message(FATAL_ERROR "read ${files} files out of the linker's trace: the parse is broken")
endif()

execute_process(COMMAND "${PACMAN}" -Qi base-devel
    OUTPUT_VARIABLE base_devel ERROR_QUIET ENVIRONMENT_MODIFICATION "LC_ALL=set:C")
set(assumed "")
if(base_devel MATCHES "\nDepends On *: ([^\n]*)")
    string(REGEX REPLACE "[ \t]+" ";" assumed "${CMAKE_MATCH_1}")
else()
    # Not installed here: the two of its members a link is made of.
    set(assumed gcc binutils)
endif()

set(problems "")
foreach(recipe PKGBUILD PKGBUILD.local)
    array_members("${SOURCE_DIR}/packaging/${recipe}" depends depends)
    array_members("${SOURCE_DIR}/packaging/${recipe}" makedepends makedepends)
    foreach(owner IN LISTS owners)
        if(NOT owner IN_LIST depends AND NOT owner IN_LIST makedepends
           AND NOT owner IN_LIST assumed)
            list(APPEND problems "${recipe}: a -m32 link opens files of ${owner}, which is in neither depends=() nor makedepends=() nor base-devel")
        endif()
    endforeach()
endforeach()
file(REMOVE_RECURSE "${scratch}")

if(problems)
    foreach(problem IN LISTS problems)
        message("FAIL ${problem}")
    endforeach()
    message(FATAL_ERROR "makepkg -s would not install what the 32-bit configure links")
endif()
message(STATUS "ok the -m32 link's ${files} files come from [${owners}], each installed by makepkg")
