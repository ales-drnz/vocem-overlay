# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# `vocem-why --system` names what an update of the system took away
# (entry 307).
#
# Three of the shapes it looks for, each one silent from inside a game, staged
# here with nothing of the machine's but ld.so:
#
#   * a shipped program the dynamic linker no longer satisfies -- a real one,
#     built here against a library carrying version VOCEM_TEST_2 and then
#     handed a library carrying only VOCEM_TEST_1, which is what a Qt minor does
#     to a window that binds a private symbol, and what an older glibc does to
#     the injected libraries (GLIBC_2.43 in 0.1.12-2);
#   * a session preload whose $LIB expands to a file that is not there, for one
#     of the two widths;
#   * a Flatpak runtime declaring the VulkanLayer extension point at 26.08 with
#     only the 25.08 branch of the extension installed -- Flathub's Steam, on
#     2026-09-29.
#
# pacman, systemctl and flatpak are stubs earlier on PATH; ldd is the real one,
# because asking the real dynamic linker is the check. The Vulkan manifests and
# the NVIDIA comparison read the machine and are not asserted here. Against
# 0.1.12-2's vocem-why, `--system` is taken for a program's name: "not running",
# no finding, and this test fails on the first.
#
# Expects CXX.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
find_program(LDD ldd)
if(NOT LDD OR NOT CXX)
    message(STATUS "skip ldd or a compiler is missing, and the linkage check is the subject")
    return()
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/vocem-why-system")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/stubs" "${work}/usr/bin" "${work}/usr/lib" "${work}/build")

# --- a program that no longer links -----------------------------------------
file(WRITE "${work}/build/lib.cpp" "extern \"C\" int vocem_test_symbol() { return 2; }\n")
file(WRITE "${work}/build/v2.map" "VOCEM_TEST_1 { local: *; };\nVOCEM_TEST_2 { global: vocem_test_symbol; } VOCEM_TEST_1;\n")
file(WRITE "${work}/build/v1.map" "VOCEM_TEST_1 { global: vocem_test_symbol; local: *; };\n")
file(WRITE "${work}/build/main.cpp"
     "extern \"C\" int vocem_test_symbol();\nint main() { return vocem_test_symbol() == 2 ? 0 : 1; }\n")
foreach(step
        "-shared|-fPIC|-Wl,--version-script=${work}/build/v2.map|-Wl,-soname,libvocemtest.so|-o|${work}/build/libvocemtest.so|${work}/build/lib.cpp"
        "-o|${work}/usr/bin/vocem-config|${work}/build/main.cpp|-L${work}/build|-lvocemtest|-Wl,-rpath,${work}/usr/lib"
        "-shared|-fPIC|-Wl,--version-script=${work}/build/v1.map|-Wl,-soname,libvocemtest.so|-o|${work}/usr/lib/libvocemtest.so|${work}/build/lib.cpp")
    string(REPLACE "|" ";" step "${step}")
    execute_process(COMMAND "${CXX}" ${step} RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "the staged program did not build: ${errors}")
    endif()
endforeach()
# And one that does link, so the check is shown to say ok as well.
execute_process(COMMAND "${CXX}" -o "${work}/usr/bin/vocemd" "${work}/build/main.cpp"
                "${work}/build/lib.cpp" RESULT_VARIABLE status)

# --- one width of the preload present, the other not -----------------------
file(WRITE "${work}/usr/lib/libvocem_gl_shim.so" "")

# --- the stubs ---------------------------------------------------------------
file(WRITE "${work}/stubs/pacman"
"#!/bin/sh
case \"$*\" in
    '-Qq vocem-overlay') echo vocem-overlay ;;
    '-Q vocem-overlay') echo 'vocem-overlay 0.0.0-test' ;;
    '-Qlq vocem-overlay') printf '%s\\n' '${work}/usr/bin/vocem-config' '${work}/usr/bin/vocemd' ;;
    *) exit 1 ;;
esac
")
file(WRITE "${work}/stubs/systemctl"
"#!/bin/sh
case \"$*\" in
    *show-environment*) echo \"LD_PRELOAD=\\$':${work}/usr/\\$LIB/libvocem_gl_shim.so'\" ;;
    *is-active*) echo active ;;
esac
")
file(WRITE "${work}/stubs/flatpak"
"#!/bin/sh
case \"$*\" in
    'list --runtime --columns=ref')
        printf '%s\\n' org.freedesktop.Platform/x86_64/25.08 org.freedesktop.Platform/x86_64/26.08 \\
            org.freedesktop.Platform.VulkanLayer.VocemOverlay/x86_64/25.08 ;;
    'info -m org.freedesktop.Platform/x86_64/25.08')
        printf '[Runtime]\\nname=x\\n\\n[Extension org.freedesktop.Platform.VulkanLayer]\\nversion = 25.08\\ndirectory = lib/extensions/vulkan\\n' ;;
    'info -m org.freedesktop.Platform/x86_64/26.08')
        printf '[Runtime]\\nname=x\\n\\n[Extension org.freedesktop.Platform.GL]\\nversion = 1.4\\n\\n[Extension org.freedesktop.Platform.VulkanLayer]\\nversion = 26.08\\ndirectory = lib/extensions/vulkan\\n' ;;
esac
")
file(CHMOD "${work}/stubs/pacman" "${work}/stubs/systemctl" "${work}/stubs/flatpak"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${work}/stubs:$ENV{PATH}"
            "XDG_CACHE_HOME=${work}/cache"
            "XDG_CONFIG_HOME=${work}/config"
            "XDG_DATA_HOME=${work}/data"
            sh "${SOURCE_DIR}/scripts/vocem-why.sh" --system
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE status)
message("---- what the tool printed ----\n${output}${errors}-------------------------------")

set(failures 0)
macro(expect pattern what)
    if(output MATCHES "${pattern}")
        message("ok   ${what}")
    else()
        message("FAIL ${what}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()
expect("!! [^\n]*/usr/bin/vocem-config no longer links[^\n]*\n[^\n]*VOCEM_TEST_2"
       "the program ld.so no longer satisfies is named, with the missing version")
expect("vocem-config no longer links[^!]*-> rebuild and reinstall the package: the window was built against another Qt"
       "and the window's line says what to do")
expect("ok  [^\n]*/usr/bin/vocemd" "a program that links says ok")
expect("ok  [^\n]*/usr/lib/libvocem_gl_shim.so" "the width whose shim is there says ok")
expect("!! [^\n]*/usr/lib32/libvocem_gl_shim.so is not there"
       "the width whose shim is missing is named")
expect("!! org.freedesktop.Platform/x86_64/26.08 mounts layers at 26.08[^\n]*//26.08"
       "the runtime whose extension point has no installed branch is named, with the command")
expect("ok  org.freedesktop.Platform/x86_64/25.08" "the runtime whose branch is installed says ok")
if(NOT status EQUAL 1)
    message("FAIL the tool exited ${status} with problems found; 1 is what says so to a script")
    math(EXPR failures "${failures} + 1")
endif()
if(failures)
    message(FATAL_ERROR "vocem-why --system missed what an update took away")
endif()
