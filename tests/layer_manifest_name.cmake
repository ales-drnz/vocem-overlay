# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The two properties that keep the 32-bit Vulkan layer alive, and why they can
# only be changed together.
#
# The installed layer ships two manifests -- VkLayer_vocem_overlay.json and
# .x86.json -- carrying the SAME layer name, and the Vulkan loader says out loud
# what it does about that:
#
#   WARNING: Removing layer VK_LAYER_VOCEM_overlay (...x86.json) because it is a
#   duplicate of VK_LAYER_VOCEM_overlay (...json)
#
# Measured on the host and, which is the half nobody had looked at, inside
# pressure-vessel: Steam's container symlinks both of our manifests in verbatim
# (06.json, 07.json -> /run/host/...) rather than rewriting them the way it
# rewrites MangoHud's, collapses them the same way, and the 32-bit process still
# loaded /run/host/usr/lib32/libvocem_vk.so. It works, and it works for exactly
# one reason: `library_path` is a bare soname, so whichever manifest survives,
# ld.so resolves the library per ELF class -- the container's own ldconfig lists
# libvocem_vk.so twice, once per architecture.
#
# Which makes the obvious tidy-up the defect. MangoHud names its layers
# VK_LAYER_MANGOHUD_overlay_x86_64 and _x86 *because* its library_path is
# absolute and one of the two would otherwise be the wrong ABI. Copying that here
# would leave both of our manifests alive with a soname each, and the layer would
# be loaded TWICE in one process. Copying only half of it -- absolute path,
# shared name -- silently kills 32-bit games inside the container, which is
# entries 30/33/34's shape exactly: the 64-bit half goes on passing.
#
# So this asserts the pair, and the coupling in the source that keeps the Flatpak
# variant (absolute path, arch-suffixed name -- the extension directory is on no
# loader search path) from drifting into the ordinary one.
#
# Expects MANIFEST, MANIFEST32 (may be empty) and LAYER_CMAKE.

set(failures 0)

if(NOT EXISTS "${MANIFEST}")
    message(STATUS "skip the installed layer manifest was not generated")
    return()
endif()

file(READ "${MANIFEST}" manifest)

# --- the name carries no architecture ---------------------------------------
if(manifest MATCHES "\"name\"[ \t]*:[ \t]*\"([^\"]+)\"")
    set(name "${CMAKE_MATCH_1}")
else()
    message("FAIL the manifest has no layer name")
    math(EXPR failures "${failures} + 1")
    set(name "")
endif()
message("     layer name:   ${name}")
if(name STREQUAL "VK_LAYER_VOCEM_overlay")
    message("ok   the ordinary manifest's name carries no architecture, so the loader "
            "collapses the pair")
else()
    message("FAIL the name is '${name}'. An arch-suffixed name leaves both manifests alive, "
            "and with a soname library_path both resolve to the SAME library: the layer "
            "would load twice in one process. If this is deliberate, library_path has to "
            "become absolute in the same change, and this test's reasoning rewritten "
            "rather than its assertion flipped.")
    math(EXPR failures "${failures} + 1")
endif()

# --- the library is a bare soname -------------------------------------------
if(manifest MATCHES "\"library_path\"[ \t]*:[ \t]*\"([^\"]+)\"")
    set(library "${CMAKE_MATCH_1}")
else()
    message("FAIL the manifest has no library_path")
    math(EXPR failures "${failures} + 1")
    set(library "")
endif()
message("     library_path: ${library}")
if(library MATCHES "/")
    message("FAIL library_path is '${library}'. An absolute path plus a shared name means the "
            "surviving manifest names ONE architecture's library and the other width gets "
            "nothing -- silently, with the 64-bit half still passing. Inside Steam's "
            "container this is measured behaviour, not a worry.")
    math(EXPR failures "${failures} + 1")
else()
    message("ok   library_path is a bare soname, which resolves per ELF class in every "
            "loader path we have measured, the container's included")
endif()

# --- both widths agree about the two fields the collapse makes load-bearing --
# Not a byte comparison: the enable_environment block legitimately differs
# between two dev trees configured with different flags. These two fields are
# the ones where a difference would mean one architecture silently getting the
# other's value, because only one manifest survives.
if(MANIFEST32 AND EXISTS "${MANIFEST32}")
    file(READ "${MANIFEST32}" manifest32)
    set(name32 "")
    set(library32 "")
    if(manifest32 MATCHES "\"name\"[ \t]*:[ \t]*\"([^\"]+)\"")
        set(name32 "${CMAKE_MATCH_1}")
    endif()
    if(manifest32 MATCHES "\"library_path\"[ \t]*:[ \t]*\"([^\"]+)\"")
        set(library32 "${CMAKE_MATCH_1}")
    endif()
    if(name32 STREQUAL name AND library32 STREQUAL library)
        message("ok   the 32-bit manifest agrees on both, so whichever one the loader keeps "
                "governs both widths correctly")
    else()
        message("FAIL the 32-bit manifest says name '${name32}', library '${library32}'. One of "
                "the two manifests is discarded and the survivor governs both widths.")
        math(EXPR failures "${failures} + 1")
    endif()
else()
    message("     (no 32-bit manifest in this tree; the cross-width claim is unmeasured here "
            "and is verified on the packaged artifacts)")
endif()

# --- the Flatpak variant's two halves stay coupled in the source -------------
# The extension is mounted where no loader search path reaches, so its
# library_path must be absolute -- and an absolute path forces a per-architecture
# name. One without the other is the defect above; the option exists so the pair
# moves together, and this checks the source still says so.
if(NOT EXISTS "${LAYER_CMAKE}")
    message("FAIL ${LAYER_CMAKE} not found")
    math(EXPR failures "${failures} + 1")
else()
    file(READ "${LAYER_CMAKE}" layer_cmake)
    if(layer_cmake MATCHES
       "if\\(VOCEM_LAYER_FLATPAK_EXTENSION\\)[ \t\r\n]*set\\(VOCEM_LAYER_NAME \"VK_LAYER_VOCEM_overlay_\\$\\{VOCEM_ARCH_SUFFIX\\}\"\\)")
        message("ok   the arch-suffixed name is reachable only through the Flatpak-extension "
                "option, which is the same branch that makes library_path absolute")
    else()
        message("FAIL the coupling between the arch-suffixed name and the absolute "
                "library_path is no longer visible in layer/CMakeLists.txt. Either half "
                "alone is a defect -- see this file's header for which.")
        math(EXPR failures "${failures} + 1")
    endif()
endif()

if(failures)
    message(FATAL_ERROR "the layer manifest's two load-bearing properties no longer hold together")
endif()
message("ok   the shared name and the bare soname hold together")
