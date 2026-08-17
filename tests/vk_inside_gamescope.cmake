# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The Vulkan half inside gamescope, with gamescope's own WSI layer in the chain.
#
# This is the one place a gamescope update can reach us. gamescope ships
# VK_LAYER_FROG_gamescope_wsi, an implicit layer that replaces the WSI
# implementation -- swapchain creation and present -- and our hook is on the
# present path. Everything else about gamescope (that it strips one named library
# from LD_PRELOAD and not the variable, measured in gl_inside_gamescope) is
# settled and does not touch this.
#
# The first attempt at this measurement passed and was worthless, which is why
# the test looks the way it does. vk_present_draw builds its own layer chain
# deliberately -- MangoHud and Steam's overlay left out, because a foreign
# interposer's pixels count exactly like ours (gl_beside_mangohud's lesson). Run
# under gamescope unchanged, that same cleanliness excluded gamescope's WSI layer
# too, so the run measured a process that had been *launched by* gamescope with
# nothing of gamescope's in its dispatch chain. 1232 pixels, four ok lines, and
# no information.
#
# So: the WSI manifest is put back in on purpose (VOCEM_VK_EXTRA_MANIFESTS), and
# the test refuses to believe it worked unless the WSI library is MAPPED in the
# probe's own process -- read out of /proc/self/maps, because an enumerated layer
# name is known to the loader whether or not its library ever loaded. Without
# that assertion this test would go on passing the day gamescope renames its
# layer or its manifest moves, and would go on saying "gamescope is fine".
#
# What this measures and what it does not. It measures that our layer's draw
# reaches the swapchain image with gamescope's WSI beneath us in the chain, read
# out of the image after present. It does NOT measure gamescope's scanout: the
# backend is headless (nothing may appear on the owner's screen -- he may be in a
# game), and the probe reads the image it owns rather than whatever gamescope
# would put on a display. If gamescope's WSI ever took its copy of the image
# ahead of our hook, this test would still pass and the screen would still be
# empty. Answering that needs a visible gamescope on a real output and is the
# owner's call to make, not a test's.
#
# Expects PROBE, MANIFEST, LIBRARY, GAMESCOPE and WSI_MANIFEST.

if(NOT EXISTS "${PROBE}" OR NOT EXISTS "${MANIFEST}" OR NOT EXISTS "${LIBRARY}")
    message(STATUS "skip the Vulkan present probe was not built")
    return()
endif()
if(NOT GAMESCOPE OR NOT EXISTS "${GAMESCOPE}")
    message(STATUS "skip gamescope is not installed here")
    return()
endif()
if(NOT WSI_MANIFEST OR NOT EXISTS "${WSI_MANIFEST}")
    message(STATUS "skip gamescope's WSI layer manifest was not found, so the interesting "
                   "half cannot be put in the chain")
    return()
endif()

# One run of the present probe, optionally nested in gamescope. Returns the
# foreign-pixel count, whether our panel-drawing log line came out, and whether
# gamescope's WSI library was mapped.
function(run_present nested out_pixels out_drew out_wsi)
    set(extra "")
    if(nested)
        set(command "${GAMESCOPE}" --backend headless -- "${PROBE}")
        set(extra "${WSI_MANIFEST}")
    else()
        set(command "${PROBE}")
    endif()
    execute_process(
        COMMAND ${command}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 300
        ENVIRONMENT_MODIFICATION
            "VOCEM_VK_MANIFEST=set:${MANIFEST}"
            "VOCEM_VK_LIBRARY=set:${LIBRARY}"
            "VOCEM_VK_EXTRA_MANIFESTS=set:${extra}"
            # gamescope sets this on everything it launches; the WSI layer's
            # manifest is gated on it, and the plain run must not have it.
            "ENABLE_GAMESCOPE_WSI=set:1"
            "MANGOHUD=unset:")
    set(both "${output}${errors}")
    if(both MATCHES "^skip |\nskip ")
        set(${out_pixels} "skip" PARENT_SCOPE)
        set(${out_drew} 0 PARENT_SCOPE)
        set(${out_wsi} 0 PARENT_SCOPE)
        return()
    endif()
    if(both MATCHES "foreign pixels:[ \t]*([0-9]+)")
        set(${out_pixels} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_pixels} "none" PARENT_SCOPE)
    endif()
    if(both MATCHES "drawing panel:")
        set(${out_drew} 1 PARENT_SCOPE)
    else()
        set(${out_drew} 0 PARENT_SCOPE)
    endif()
    if(both MATCHES "mapped layer:[^\n\r]*libVkLayer_FROG")
        set(${out_wsi} 1 PARENT_SCOPE)
    else()
        set(${out_wsi} 0 PARENT_SCOPE)
    endif()
endfunction()

run_present(FALSE plain_pixels plain_drew plain_wsi)
if(plain_pixels STREQUAL "skip")
    message(STATUS "skip the present probe skipped outside gamescope")
    return()
endif()
run_present(TRUE nested_pixels nested_drew nested_wsi)
if(nested_pixels STREQUAL "skip")
    message(STATUS "skip the present probe skipped inside gamescope")
    return()
endif()

message("     plain              ${plain_pixels} px, drew=${plain_drew}, gamescope WSI mapped=${plain_wsi}")
message("     inside gamescope   ${nested_pixels} px, drew=${nested_drew}, gamescope WSI mapped=${nested_wsi}")

set(failures 0)

# The control: plainly the overlay draws through its own present hook. Without
# this the rest could be satisfied by an overlay that never draws anywhere.
if(plain_drew AND NOT plain_pixels STREQUAL "none" AND plain_pixels GREATER 500)
    message("ok   plainly, the present hook draws")
else()
    message("FAIL plainly the present hook drew nothing: the rest of this measures nothing")
    math(EXPR failures "${failures} + 1")
endif()

# The instrument: gamescope's WSI layer really is in the probe's process. This is
# the assertion the first version of this test lacked, and lacking it it reported
# success about nothing.
if(nested_wsi)
    message("ok   gamescope's WSI layer is mapped in the probe, so the chain is the one in "
            "question")
else()
    message("FAIL gamescope's WSI layer never loaded in the probe (manifest ${WSI_MANIFEST}). "
            "Whatever the pixel count says, this run measured a process that was merely "
            "launched by gamescope -- which is exactly how the first version of this test "
            "passed while measuring nothing.")
    math(EXPR failures "${failures} + 1")
endif()

# The claim.
if(nested_drew AND NOT nested_pixels STREQUAL "none" AND nested_pixels EQUAL plain_pixels)
    message("ok   and with gamescope's WSI beneath us the overlay reaches the swapchain image, "
            "the same frame as plainly")
else()
    message("FAIL inside gamescope: ${nested_pixels} px against ${plain_pixels} plainly, "
            "drew=${nested_drew}. gamescope's WSI layer and our present hook are on the same "
            "path, so this is where a gamescope update reaches us; DESIGN's entry has to be "
            "rewritten rather than this assertion relaxed.")
    math(EXPR failures "${failures} + 1")
endif()

if(failures)
    message(FATAL_ERROR "the Vulkan half no longer behaves inside gamescope as measured")
endif()
message("ok   the Vulkan half draws with gamescope's WSI layer in the chain (scanout not "
        "measured -- see this file's header)")
