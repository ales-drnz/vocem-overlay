# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The present hook under the Khronos validation layer, where the machine has it.
#
# tests/vk_witness_layer.cpp exists because this machine has no validation
# layer: it knows the handful of calls this project's rules are about and
# nothing else. Where `vulkan-validation-layers` is installed, the real thing
# can sit in the chain too -- below the overlay, so it sees what the overlay
# asks -- and every message it prints is a fault. Skips out loud where the
# layer is absent; never installs anything (the owner installs).
#
# "Below the overlay" was this header's claim and not the chain's until entry
# 192. An extra manifest lands wherever the loader puts implicit layers, and
# VK_LOADER_DEBUG=layer measured it ABOVE: Application -> validation ->
# overlay -> driver. Seven scenes "clean" were seven scenes of the probe's own
# calls; a barrier mutated to drop its dependency on earlier reads passed all
# of them. VOCEM_VK_BELOW now puts the validation layer under the overlay
# through the loader's override meta-layer, the way the witness goes, and the
# first pass below reads the device chain off the loader and refuses unless the
# overlay really is above it.
#
# The validation manifest is explicit (explicit_layer.d), and the probe builds
# an implicit chain of its own from the manifests it is handed
# (VOCEM_VK_EXTRA_MANIFESTS); copied there, and given the disable_environment
# an implicit manifest must carry, it loads as an implicit layer for this run
# and this run only. Copied bare it did not: the loader skips an implicit layer
# without that key, and this test failed on "never mapped" the first time the
# layer was installed on this machine (2026-09-10; entry 143). Its messages go
# to stdout, which is where the probe's own lines go, so the whole output is
# read.
#
# Expects PROBE, MANIFEST, LIBRARY; resolves the validation manifest when it
# runs (a package installed after configure is found, one removed is a skip).

foreach(candidate
        "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
        "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
        "$ENV{HOME}/.local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
    if(EXISTS "${candidate}")
        set(VALIDATION "${candidate}")
        break()
    endif()
endforeach()
if(NOT VALIDATION)
    message(STATUS "skip the Khronos validation layer is not installed here "
                   "(vulkan-validation-layers); vk_witness_layer.cpp is the stand-in")
    return()
endif()
if(NOT EXISTS "${PROBE}")
    message(STATUS "skip the Vulkan probe was not built")
    return()
endif()

# Every scenario, not the default one alone.
#
# This ran the plain loop and nothing else, so the legs that CREATE and DESTROY
# things -- a swapchain recreated in another format, a second device coming and
# going, frames chained with two in flight, and the daemon-stopped release that
# tears the renderer down on an arbitrary present of a live device -- were
# exactly the legs the real layer never saw. That is where a validation layer
# earns its keep, and it is where the wait this release path needs would have
# been missing in silence.
#
# And "arrivals" (entry 192): the one leg that copies into a font image frames
# already in flight are sampling, ordered only by a barrier -- the kind of
# claim a validation layer's synchronisation checks exist for. It needs the
# repository's emoji bank, or there is nothing to fold.
set(scenarios "" "recreate" "second-device" "in-flight" "daemon-gone" "idle" "arrivals")
set(measured 0)

# The positive control on the ORDER, every run: the loader prints the device
# chain from the application down, and the overlay has to come before the
# validation layer in it. The default scene is used because it leaves stderr
# alone (the scenes that count the layer's lines redirect it into a file).
execute_process(
    COMMAND "${PROBE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 240
    ENVIRONMENT_MODIFICATION
        "VOCEM_VK_MANIFEST=set:${MANIFEST}"
        "VOCEM_VK_LIBRARY=set:${LIBRARY}"
        "VOCEM_VK_EXTRA_MANIFESTS=set:${VALIDATION}"
        "VOCEM_VK_BELOW=set:VK_LAYER_KHRONOS_validation"
        "VOCEM_VK_SCENARIO=set:"
        "VK_LOADER_DEBUG=set:layer")
if(status EQUAL 77)
    message(STATUS "skip the probe could not measure the chain here; its output says why")
    return()
endif()
string(FIND "${output}${errors}" "vkCreateDevice layer callstack" callstack_at)
if(callstack_at EQUAL -1)
    message(FATAL_ERROR "the loader printed no device chain to check the order against:\n"
                        "${output}${errors}")
endif()
string(SUBSTRING "${output}${errors}" ${callstack_at} -1 callstack)
string(FIND "${callstack}" "VK_LAYER_VOCEM_overlay" overlay_at)
string(FIND "${callstack}" "VK_LAYER_KHRONOS_validation" validation_at)
if(overlay_at EQUAL -1 OR validation_at EQUAL -1 OR NOT overlay_at LESS validation_at)
    message(FATAL_ERROR "the validation layer is not below the overlay in the device chain, so "
                        "it would be validating the probe and not the overlay:\n${callstack}")
endif()
message(STATUS "     device chain: the overlay above the validation layer")

foreach(scenario IN LISTS scenarios)
    if(scenario STREQUAL "")
        set(label "the default loop")
    else()
        set(label "${scenario}")
    endif()
    execute_process(
        COMMAND "${PROBE}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 240
        ENVIRONMENT_MODIFICATION
            "VOCEM_VK_MANIFEST=set:${MANIFEST}"
            "VOCEM_VK_LIBRARY=set:${LIBRARY}"
            "VOCEM_VK_EXTRA_MANIFESTS=set:${VALIDATION}"
            "VOCEM_VK_BELOW=set:VK_LAYER_KHRONOS_validation"
            "VOCEM_VK_SCENARIO=set:${scenario}"
            "VOCEM_EMOJI_BANK=set:${EMOJI_BANK}"
            # The synchronisation checks too: the arrivals leg's copy into a
            # live image is ordered by a barrier and nothing else.
            "VK_LAYER_VALIDATE_SYNC=set:1"
            # Every message, and to stdout: the layer's defaults print only errors.
            "VK_LAYER_MESSAGE_ID_FILTER=unset:"
            "VK_LAYER_LOG_FILENAME=set:stdout")
    set(both "${output}${errors}")
    if(status EQUAL 77)
        message(STATUS "skip the probe could not measure ${label} here; its output says why")
        return()
    endif()
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "the probe failed under validation (${label}):\n${both}")
    endif()
    if(NOT both MATCHES "libVkLayer_khronos_validation")
        message(FATAL_ERROR
                "the validation layer was named and never mapped (${label}): the chain measured "
                "was not the one this test is about\n${both}")
    endif()
    string(REGEX MATCHALL "(Validation Error|VUID-[A-Za-z0-9_-]+|Validation Warning)" faults
           "${both}")
    list(LENGTH faults fault_count)
    if(fault_count GREATER 0)
        message("${both}")
        message(FATAL_ERROR
                "the validation layer reported ${fault_count} message(s) with the overlay in the "
                "chain (${label})")
    endif()
    message(STATUS "     ${label}: clean")
    math(EXPR measured "${measured} + 1")
endforeach()

# A loop that measured nothing would print the same closing line as one that
# measured everything (entry 105's rule).
list(LENGTH scenarios wanted)
if(NOT measured EQUAL wanted)
    message(FATAL_ERROR "only ${measured} of ${wanted} scenarios were measured")
endif()
message("ok   the validation layer sat under the overlay for ${measured} scenarios and reported "
        "nothing")
