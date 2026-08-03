# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")

add_library(vocem_gl SHARED
    src/vocem_gl.cpp
    "${PROJECT_SOURCE_DIR}/common/src/panel.cpp"
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
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
    "${PROJECT_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

# The interposer must export its hooks; everything else stays hidden so we cannot
# accidentally shadow an application's own symbols. The hooks carry an explicit
# visibility attribute, so hiding the rest is what actually holds this to the
# handful of names the interposition needs -- without it the library exported its
# entire bundled copy of ImGui.
target_compile_options(vocem_gl PRIVATE -Wall -Wextra)
set_target_properties(vocem_gl PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(vocem_gl PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_DEBUG_TOOLS
)

set_source_files_properties(
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_link_libraries(vocem_gl PRIVATE ${CMAKE_DL_LIBS})

# --- shim -------------------------------------------------------------------
# The only library that gets preloaded. Kept deliberately tiny: it is mapped into
# every process in the session, so it must contain nothing that costs anything.
add_library(vocem_gl_shim SHARED src/vocem_gl_shim.cpp)

set_target_properties(vocem_gl_shim PROPERTIES OUTPUT_NAME vocem_gl_shim)

# No exceptions, no RTTI, no thread-safe static guards: all three would create a
# dependency on the C++ runtime, and this library is mapped into every process.
target_compile_options(vocem_gl_shim PRIVATE
    -Wall -Wextra -fno-exceptions -fno-rtti -fno-threadsafe-statics)

# Where this build installs the overlay, so the shim can find it by its path on the
# host when it is running inside a container and the soname resolves to nothing.
# Each of the two builds -- 64-bit and 32-bit -- carries its own answer, which is
# why this is a compile-time define and not a guess made at runtime.
target_compile_definitions(vocem_gl_shim PRIVATE
    VOCEM_LIBDIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
# -static-libgcc as well as -nostdlib++, so the only thing this needs is libc.
#
# It is a two-kilobyte trade for a real property: this object is preloaded into
# every process in the session, and a preloaded object that cannot be loaded is not
# a quiet failure -- ld.so prints "cannot be preloaded: ignored" to the stderr of
# every process that starts. Depending on libgcc_s meant the 32-bit half needed
# lib32-gcc-libs present or the whole session started talking. Now it needs the C
# library, which is there by definition.
target_link_options(vocem_gl_shim PRIVATE -nostdlib++ -static-libgcc)
target_link_libraries(vocem_gl_shim PRIVATE ${CMAKE_DL_LIBS})

install(TARGETS vocem_gl vocem_gl_shim LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
