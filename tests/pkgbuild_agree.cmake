# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The two PKGBUILDs build the same package. `PKGBUILD` fetches a tag and is
# what a stranger uses; `PKGBUILD.local` builds the working tree and is what
# the owner packages with. Entry 76 is what their drifting cost -- the licences
# entry 50 established were in the local one only -- and entry 137 is the next
# drift: the tagged file compiled the whole test suite into every stranger's
# build, because `-DVOCEM_BUILD_TESTS=OFF` was in the local file alone.
#
# What is held: the build() and package() bodies, and the depends/makedepends
# lists, are the same text once the two files' legitimate differences are
# normalised away -- the local file's `$_root/` prefix and `$srcdir/build`
# directories against the tagged file's relative paths, and `git` in the tagged
# file's makedepends, which fetches nothing locally. Comments are stripped
# first: the two files explain themselves differently on purpose.

if(NOT SOURCE_DIR)
    get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# One function's body, comments and blank lines gone, paths normalised.
function(function_body file name out)
    file(READ "${file}" text)
    string(REGEX MATCH "\n${name}\\(\\) {\n(.*)\n}\n" _ "${text}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "pkgbuild_agree: ${file} has no ${name}() function")
    endif()
    set(body "${CMAKE_MATCH_1}")
    string(REPLACE ";" "<semicolon>" body "${body}")
    string(REPLACE "\n" ";" lines "${body}")
    set(kept "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^[ \t]*#.*$" "" line "${line}")
        string(REGEX REPLACE "[ \t]+$" "" line "${line}")
        if(line STREQUAL "")
            continue()
        endif()
        # PKGBUILD.local builds from `$_root` into `$srcdir/build`; the tagged
        # file cds into its checkout and builds from `.` into `build`. Same
        # directories, other names; the cd is the tagged file's alone.
        if(line MATCHES "^[ \t]*cd ")
            continue()
        endif()
        string(REPLACE "\"$srcdir/build32\"" "build32" line "${line}")
        string(REPLACE "\"$srcdir/build\"" "build" line "${line}")
        string(REPLACE "\"$_root\"" "." line "${line}")
        string(REGEX REPLACE "\"\\$_root/([^\"]*)\"" "\\1" line "${line}")
        string(REPLACE "$_root/" "" line "${line}")
        list(APPEND kept "${line}")
    endforeach()
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

# A bash array's members, one per line, comments stripped.
function(array_members file name out)
    file(READ "${file}" text)
    string(REGEX MATCH "\n${name}=\\(([^)]*)\\)" _ "${text}")
    if(NOT DEFINED CMAKE_MATCH_1)
        message(FATAL_ERROR "pkgbuild_agree: ${file} has no ${name}=( ) array")
    endif()
    set(body "${CMAKE_MATCH_1}")
    string(REPLACE ";" "<semicolon>" body "${body}")
    string(REPLACE "\n" ";" lines "${body}")
    set(kept "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*$" "" line "${line}")
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        list(APPEND kept "${line}")
    endforeach()
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

set(tagged "${SOURCE_DIR}/packaging/PKGBUILD")
set(local "${SOURCE_DIR}/packaging/PKGBUILD.local")
set(problems "")

foreach(name build package)
    function_body("${tagged}" "${name}" tagged_body)
    function_body("${local}" "${name}" local_body)
    if(NOT tagged_body STREQUAL local_body)
        list(APPEND problems "${name}() differs between PKGBUILD and PKGBUILD.local")
        message("--- PKGBUILD ${name}():")
        foreach(line IN LISTS tagged_body)
            message("    ${line}")
        endforeach()
        message("--- PKGBUILD.local ${name}():")
        foreach(line IN LISTS local_body)
            message("    ${line}")
        endforeach()
    endif()
endforeach()

array_members("${tagged}" depends tagged_depends)
array_members("${local}" depends local_depends)
if(NOT tagged_depends STREQUAL local_depends)
    list(APPEND problems "depends=() differs: PKGBUILD has [${tagged_depends}], PKGBUILD.local has [${local_depends}]")
endif()
array_members("${tagged}" makedepends tagged_make)
array_members("${local}" makedepends local_make)
list(REMOVE_ITEM tagged_make "'git'")
if(NOT tagged_make STREQUAL local_make)
    list(APPEND problems "makedepends=() differs beyond git: PKGBUILD has [${tagged_make}], PKGBUILD.local has [${local_make}]")
endif()

# Whole-file sanity, so this cannot pass on two empty bodies.
list(LENGTH tagged_body tagged_lines)
if(tagged_lines LESS 5)
    message(FATAL_ERROR "pkgbuild_agree: package() read as ${tagged_lines} lines, which is not a body")
endif()

if(problems)
    string(REPLACE ";" "\n  " problems "${problems}")
    message(FATAL_ERROR "the two PKGBUILDs have drifted (entry 76's shape):\n  ${problems}")
endif()
message(STATUS "ok pkgbuild_agree: build(), package(), depends and makedepends agree")
