# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")

add_library(vocem_gl SHARED
    src/vocem_gl.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)

# Deliberately the same name in both builds: they are told apart by the directory
# they install into (lib and lib32), which is what lets a single LD_PRELOAD value
# with the linker's $LIB token work for 32-bit and 64-bit games alike.
set_target_properties(vocem_gl PROPERTIES OUTPUT_NAME vocem_gl)

target_include_directories(vocem_gl PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

# This library has no hooks (the shim owns all three doors): it exports exactly
# the four vocem_gl_* entry points the shim resolves, which carry an explicit
# visibility attribute. Hidden visibility is what holds it to those names, or
# its bundled ImGui would be exported ahead of any ImGui a game links
# (entry 16). tests/gl_entry_points.cmake holds the set both ways.
target_compile_options(vocem_gl PRIVATE -Wall -Wextra)
set_target_properties(vocem_gl PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(vocem_gl PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_DEBUG_TOOLS
    # ImGui's IM_ASSERT is assert(), an abort() in somebody's game if it
    # fires (rule 7). Compiled out for this target alone, whatever the build
    # type, by include/vocem/imconfig_injected.h (a header, not a -D, so the
    # tests keep their asserts).
    IMGUI_USER_CONFIG="vocem/imconfig_injected.h"
)

set_source_files_properties(
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# The panel, the fonts, the registry and the palette, compiled once per width
# in common/ with hidden visibility (common/CMakeLists.txt). Threads: the first
# font atlas is rasterised on a worker; the shim stays libc-only.
find_package(Threads REQUIRED)
target_link_libraries(vocem_gl PRIVATE vocem_common ${CMAKE_DL_LIBS} Threads::Threads)
# The export list, stated: hidden visibility does not reach the libstdc++
# templates this library instantiates (vocem_gl.map says which escaped).
target_link_options(vocem_gl PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/vocem_gl.map")
# The C++ runtime inside, not NEEDED (the top-level CMakeLists.txt says why).
target_link_options(vocem_gl PRIVATE ${VOCEM_INJECTED_RUNTIME_LINK_OPTIONS})
set_target_properties(vocem_gl PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vocem_gl.map")

# --- shim -------------------------------------------------------------------
# The only library that gets preloaded. Kept deliberately tiny: it is mapped into
# every process in the session, so it must contain nothing that costs anything.
add_library(vocem_gl_shim SHARED src/vocem_gl_shim.cpp)

set_target_properties(vocem_gl_shim PROPERTIES OUTPUT_NAME vocem_gl_shim)

# No exceptions, no RTTI, no thread-safe static guards: all three would create a
# dependency on the C++ runtime, and this library is mapped into every process.
target_compile_options(vocem_gl_shim PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti -fno-threadsafe-statics)

# Where this build installs the overlay, so the shim can find it by its path on
# the host inside a container, where the soname resolves to nothing. A
# compile-time define because each width carries its own answer.
target_compile_definitions(vocem_gl_shim PRIVATE
    VOCEM_LIBDIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
# -static-libgcc as well as -nostdlib++, so the only thing this needs is libc:
# a preloaded object that cannot be loaded makes ld.so print "cannot be
# preloaded: ignored" on the stderr of every process that starts, so it may
# depend on nothing that might be absent (libgcc_s at 32 bits).
#
# --no-undefined because -nostdlib++ alone refuses nothing: a stray C++ symbol
# would link and, under LD_BIND_NOW=1, stop every program in the session from
# starting (tests/shim_artifact.cmake compiles such a toy with these flags).
target_link_options(vocem_gl_shim PRIVATE -nostdlib++ -static-libgcc -Wl,--no-undefined)
target_link_libraries(vocem_gl_shim PRIVATE ${CMAKE_DL_LIBS})

install(TARGETS vocem_gl vocem_gl_shim LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
