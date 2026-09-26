# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")

add_library(vocem_vk SHARED
    src/vocem_layer.cpp
    src/overlay_renderer.cpp
    src/texture_cache.cpp
    src/hdr_pipeline.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)

# The library keeps one name for both architectures and is distinguished by its
# directory; only the manifests need separate names, since they live together in
# implicit_layer.d. The loader resolves a bare library name through the normal
# search path, which is already architecture-aware.
set(VOCEM_LIBRARY_NAME vocem_vk)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(VOCEM_MANIFEST_NAME VkLayer_vocem_overlay.x86.json)
    set(VOCEM_ARCH_SUFFIX x86)
else()
    set(VOCEM_MANIFEST_NAME VkLayer_vocem_overlay.json)
    set(VOCEM_ARCH_SUFFIX x86_64)
endif()

# The two things a Flatpak extension needs that a distribution package does not.
#
# An extension is mounted at /usr/lib/extensions/vulkan/VocemOverlay, and that
# directory is on no loader search path -- the extension point declares no
# add-ld-path -- so the bare soname below resolves to nothing there and the
# library_path has to be absolute. And once the two architectures' manifests
# carry *different* library paths they must also carry different layer NAMES:
# on the host both manifests are byte-identical and the loader collapses them
# into one entry, which is exactly what must not happen when only one of the two
# is the right ABI. This is what MangoHud's VK_LAYER_MANGOHUD_overlay_x86_64 and
# _x86 are for.
option(VOCEM_LAYER_FLATPAK_EXTENSION
    "Build the layer manifest for a Flatpak VulkanLayer extension: absolute \
library_path, and a layer name carrying the architecture" OFF)
if(VOCEM_LAYER_FLATPAK_EXTENSION)
    set(VOCEM_LAYER_NAME "VK_LAYER_VOCEM_overlay_${VOCEM_ARCH_SUFFIX}")
else()
    set(VOCEM_LAYER_NAME "VK_LAYER_VOCEM_overlay")
endif()

set_target_properties(vocem_vk PROPERTIES
    OUTPUT_NAME ${VOCEM_LIBRARY_NAME}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(vocem_vk PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    "${PROJECT_SOURCE_DIR}/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
# common/CMakeLists.txt: the panel, the fonts, the registry and the palette,
# built once per width with the visibility and the ImGui definitions this
# target needs them to have.
target_link_libraries(vocem_vk PRIVATE vocem_common Threads::Threads)
# The export list, stated: hidden visibility does not reach the libstdc++
# templates this library instantiates (vocem_vk.map says which escaped).
target_link_options(vocem_vk PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/vocem_vk.map")
# The C++ runtime inside, not NEEDED (the top-level CMakeLists.txt says why).
target_link_options(vocem_vk PRIVATE ${VOCEM_INJECTED_RUNTIME_LINK_OPTIONS})
set_target_properties(vocem_vk PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vocem_vk.map")

target_compile_options(vocem_vk PRIVATE
    -Wall -Wextra
    -fno-exceptions
)

# ImGui is third-party: build it without our warning flags so its output does not
# drown ours, and keep it out of any -Werror we adopt later.
set_source_files_properties(
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# A layer must not pull in the Vulkan loader: it is loaded *by* it and resolves
# everything through the dispatch chain. The matching ImGui flag forces its Vulkan
# backend to go through ImGui_ImplVulkan_LoadFunctions, which we point at our own
# dispatch tables.
target_compile_definitions(vocem_vk PRIVATE
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_DEBUG_TOOLS
    # No abort() reachable from ImGui inside a game whatever the build type:
    # gl/CMakeLists.txt says why.
    IMGUI_USER_CONFIG="vocem/imconfig_injected.h"
)

# --- manifest -------------------------------------------------------------
# Two variants: one for the build tree (absolute path, used by scripts/dev-run.sh)
# and one for installation (bare filename, resolved via the loader search path).

set(VOCEM_LAYER_LIBRARY_PATH "$<TARGET_FILE:vocem_vk>")
file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/dev/${VOCEM_MANIFEST_NAME}"
    CONTENT "{
    \"file_format_version\": \"1.2.1\",
    \"layer\": {
        \"name\": \"VK_LAYER_VOCEM_overlay\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"$<TARGET_FILE:vocem_vk>\",
        \"api_version\": \"${VOCEM_LAYER_API_VERSION}\",
        \"implementation_version\": \"1\",
        \"description\": \"Vocem Overlay - Discord voice overlay drawn inside the game\",
        \"enable_environment\": { \"VOCEM\": \"1\" },
        \"disable_environment\": { \"VOCEM_DISABLE\": \"1\" }
    }
}
"
)

if(VOCEM_ENABLE_BY_DEFAULT)
    set(VOCEM_LAYER_ENABLE_BLOCK "")
else()
    set(VOCEM_LAYER_ENABLE_BLOCK "        \"enable_environment\": {\n            \"VOCEM\": \"1\"\n        },\n")
endif()

if(VOCEM_LAYER_FLATPAK_EXTENSION)
    set(VOCEM_LAYER_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/lib${VOCEM_LIBRARY_NAME}.so")
else()
    set(VOCEM_LAYER_LIBRARY_PATH "lib${VOCEM_LIBRARY_NAME}.so")
endif()
configure_file(
    manifest/VkLayer_vocem_overlay.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/install/${VOCEM_MANIFEST_NAME}"
    @ONLY
)

install(TARGETS vocem_vk LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/${VOCEM_MANIFEST_NAME}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d"
)
