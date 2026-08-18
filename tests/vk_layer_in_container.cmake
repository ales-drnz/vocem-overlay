# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Both widths of the installed layer, inside Steam's container, where the
# documented defect lives.
#
# The Vulkan loader keeps "only the first occurrence of any layer name", and
# pressure-vessel generates a manifest per architecture -- which for MangoHud
# once meant one width silently lost its layer, and is why its layers are named
# VK_LAYER_MANGOHUD_overlay_x86_64 and _x86. Our two manifests share one name, so
# on paper this project is exposed to the same defect. On this machine it is not,
# and the reason is worth a test rather than a sentence: the container symlinks
# our manifests in verbatim (06.json, 07.json -> /run/host/...) instead of
# rewriting them the way it rewrites MangoHud's, collapses them exactly as the
# host does, and the survivor names the library by bare soname -- so the
# container's own ldconfig resolves it per ELF class, twice, once per
# architecture.
#
# Which means the claim under test is not "a layer named ours is known" but "this
# process got OUR CODE AT ITS OWN WIDTH", and the probe answers with the path it
# mapped. lib32 for the 32-bit run, lib (or lib64) for the 64-bit one: asserted
# per run, because a pair of runs that both loaded the 64-bit library would
# otherwise look like success and is the whole failure being guarded against.
#
# Measures the INSTALLED layer, not the build tree -- there is no other way to
# ask about the installed pair of manifests, and the container reaches the host's
# /usr only under /run/host. So it skips where nothing is installed, and a stale
# installation measures the stale installation, which the message says.
#
# Expects PROBE64, PROBE32 (either may be empty) and SNIPER.

if(NOT SNIPER OR NOT EXISTS "${SNIPER}")
    message(STATUS "skip Steam's sniper runtime is not installed here")
    return()
endif()
if(NOT EXISTS "/usr/share/vulkan/implicit_layer.d/VkLayer_vocem_overlay.json")
    message(STATUS "skip the layer is not installed, and this test is about the installed pair")
    return()
endif()

# One run of a probe inside the container. Returns the path it mapped, or "none".
function(run_in_container probe out_path)
    execute_process(
        COMMAND "${SNIPER}" -- "${probe}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 300)
    set(both "${output}${errors}")
    if(both MATCHES "probe: mapped (/[^\n\r]+)")
        set(${out_path} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    elseif(both MATCHES "skip ([^\n\r]+)")
        set(${out_path} "skip:${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_path} "none" PARENT_SCOPE)
    endif()
endfunction()

set(failures 0)
set(measured 0)

foreach(width 64 32)
    set(probe "${PROBE${width}}")
    if(NOT probe OR NOT EXISTS "${probe}")
        message("     ${width}-bit: probe not built, nothing measured at this width")
        continue()
    endif()
    run_in_container("${probe}" mapped)
    message("     ${width}-bit: ${mapped}")
    if(mapped MATCHES "^skip:")
        # The container refusing Vulkan at a width is not this test's claim, and
        # calling it a pass would be the vacuous kind.
        message("     ${width}-bit: the container gave no usable Vulkan at this width")
        continue()
    endif()
    math(EXPR measured "${measured} + 1")
    if(mapped STREQUAL "none")
        message("FAIL ${width}-bit: our layer never arrived in the process. If the container has "
                "started rewriting our manifests instead of symlinking them, the shared layer "
                "name now costs one architecture its overlay -- silently, which is entries "
                "30/33/34's shape.")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()
    # The width of the library, from its path. lib32 is unambiguous; the 64-bit
    # library lives in lib or lib64 depending on the distribution, so it is
    # asserted as "not the 32-bit one" rather than by a name that would make this
    # test Arch-only.
    if(width STREQUAL "32")
        if(mapped MATCHES "lib32|i386")
            message("ok   32-bit: the 32-bit process mapped the 32-bit library")
        else()
            message("FAIL 32-bit: mapped '${mapped}', which is not a 32-bit path. A 32-bit "
                    "process holding the 64-bit library is the duplicate-name defect itself.")
            math(EXPR failures "${failures} + 1")
        endif()
    else()
        if(mapped MATCHES "lib32|i386")
            message("FAIL 64-bit: mapped '${mapped}', a 32-bit path")
            math(EXPR failures "${failures} + 1")
        else()
            message("ok   64-bit: the 64-bit process mapped the 64-bit library")
        endif()
    endif()
endforeach()

if(measured EQUAL 0)
    message(STATUS "skip neither width could be measured inside the container")
    return()
endif()
if(measured EQUAL 1)
    message("     only one width was measured, so the cross-width claim is HALF made: "
            "one width working is what entries 30/33/34 are about")
endif()
if(failures)
    message(FATAL_ERROR "the installed layer no longer reaches both widths inside the container")
endif()
message("ok   both widths of the installed layer load inside Steam's container")
