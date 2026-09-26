# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# A library package in depends=() is one something shipped actually needs.
#
# vulkan-icd-loader and lib32-vulkan-icd-loader sat in depends=() beside a
# comment saying the 32-bit libraries "fail to load" without them. Measured
# with readelf on every ELF file of the packaged 0.1.10-7 (both widths):
# nothing NEEDs libvulkan.so.1 and nothing dlopens it -- the overlay's layer
# is loaded BY the Vulkan loader, which every Vulkan game brings with it; it
# never calls the loader itself. The comment was true of lib32-gcc-libs
# alone (the 32-bit libraries NEED libstdc++.so.6 and libgcc_s.so.1), and
# namcap reports both loaders as dependencies nothing uses.
#
# Two claims, on the build trees the package is made from:
#   * if a Vulkan loader package is in depends=(), some shipped ELF file of
#     that width NEEDs libvulkan.so.1;
#   * PKGBUILD, PKGBUILD.local and .SRCINFO list the same depends and the same
#     optdepends (pkgbuild_agree holds the two PKGBUILDs' depends; .SRCINFO and
#     the optdepends were held by nothing -- entry 76's shape).

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
find_program(READELF readelf)
if(NOT READELF)
    message(STATUS "skip readelf is missing")
    return()
endif()

# A bash array's members, one per line, comments stripped; an optdepends
# member is its package name (the text before the colon).
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

function(srcinfo_members file name out)
    file(STRINGS "${file}" lines REGEX "^[ \t]*${name} = ")
    set(kept "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^[ \t]*${name} = ([^:]*).*$" "\\1" line "${line}")
        string(STRIP "${line}" line)
        list(APPEND kept "${line}")
    endforeach()
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

set(problems "")
set(tagged "${SOURCE_DIR}/packaging/PKGBUILD")
foreach(kind depends optdepends)
    array_members("${tagged}" ${kind} tagged_${kind})
    array_members("${SOURCE_DIR}/packaging/PKGBUILD.local" ${kind} local_${kind})
    srcinfo_members("${SOURCE_DIR}/packaging/.SRCINFO" ${kind} srcinfo_${kind})
    if(NOT tagged_${kind} STREQUAL local_${kind})
        list(APPEND problems "${kind} differs: PKGBUILD [${tagged_${kind}}], PKGBUILD.local [${local_${kind}}]")
    endif()
    if(NOT tagged_${kind} STREQUAL srcinfo_${kind})
        list(APPEND problems "${kind} differs: PKGBUILD [${tagged_${kind}}], .SRCINFO [${srcinfo_${kind}}]")
    endif()
endforeach()
message("depends:    ${tagged_depends}")
message("optdepends: ${tagged_optdepends}")

# What the shipped files NEED, per width. The 32-bit tree is where the
# packaged lib32 files come from; without it that half is not measured, and
# a 32-bit loader in depends then cannot be held to anything, which is said.
function(needed_of files out)
    set(all "")
    foreach(file IN LISTS files)
        if(NOT EXISTS "${file}")
            continue()
        endif()
        execute_process(COMMAND "${READELF}" -d "${file}" OUTPUT_VARIABLE dynamic
                        ENVIRONMENT_MODIFICATION "LC_ALL=set:C")
        string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[([^]]*)\\]" rows "${dynamic}")
        foreach(row IN LISTS rows)
            string(REGEX REPLACE ".*\\[([^]]*)\\].*" "\\1" soname "${row}")
            list(APPEND all "${soname}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES all)
    set(${out} "${all}" PARENT_SCOPE)
endfunction()
set(shipped64
    "${BUILD_DIR}/gl/libvocem_gl_shim.so" "${BUILD_DIR}/gl/libvocem_gl.so"
    "${BUILD_DIR}/layer/libvocem_vk.so" "${BUILD_DIR}/daemon/vocemd"
    "${BUILD_DIR}/cli/vocem" "${BUILD_DIR}/gui/vocem-config")
set(shipped32
    "${BUILD32_DIR}/gl/libvocem_gl_shim.so" "${BUILD32_DIR}/gl/libvocem_gl.so"
    "${BUILD32_DIR}/layer/libvocem_vk.so")
needed_of("${shipped64}" needed64)
needed_of("${shipped32}" needed32)
message("64-bit NEEDED: ${needed64}")
message("32-bit NEEDED: ${needed32}")

if("vulkan-icd-loader" IN_LIST tagged_depends AND NOT "libvulkan.so.1" IN_LIST needed64)
    list(APPEND problems
        "vulkan-icd-loader is in depends, and no shipped 64-bit file NEEDs libvulkan.so.1")
endif()
if("lib32-vulkan-icd-loader" IN_LIST tagged_depends)
    if(NOT EXISTS "${BUILD32_DIR}/layer/libvocem_vk.so")
        message(STATUS "skip the 32-bit tree (${BUILD32_DIR}) is not built, so "
                       "lib32-vulkan-icd-loader in depends cannot be measured")
        return()
    endif()
    if(NOT "libvulkan.so.1" IN_LIST needed32)
        list(APPEND problems
            "lib32-vulkan-icd-loader is in depends, and no shipped 32-bit file NEEDs libvulkan.so.1")
    endif()
endif()

if(problems)
    foreach(problem IN LISTS problems)
        message("FAIL ${problem}")
    endforeach()
    message(FATAL_ERROR "the package's dependencies do not match what it ships")
endif()
message(STATUS "every library package in depends is NEEDED, and the three files agree")
