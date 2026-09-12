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
        # Every message, and to stdout: the layer's defaults print only errors.
        "VK_LAYER_MESSAGE_ID_FILTER=unset:"
        "VK_LAYER_LOG_FILENAME=set:stdout")
set(both "${output}${errors}")
if(status EQUAL 77)
    message(STATUS "skip the probe could not measure here; its output says why")
    return()
endif()
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the probe failed under validation:\n${both}")
endif()
if(NOT both MATCHES "libVkLayer_khronos_validation")
    message(FATAL_ERROR "the validation layer was named and never mapped: the chain measured was "
                        "not the one this test is about\n${both}")
endif()
string(REGEX MATCHALL "(Validation Error|VUID-[A-Za-z0-9_-]+|Validation Warning)" faults "${both}")
list(LENGTH faults fault_count)
if(fault_count GREATER 0)
    message("${both}")
    message(FATAL_ERROR "the validation layer reported ${fault_count} message(s) with the overlay in the chain")
endif()
message("ok   the validation layer sat under the overlay for the whole scene and reported nothing")
