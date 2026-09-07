# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# vocem_common: the code both injected paths, the daemon, the settings window and
# the tests share, compiled once per width instead of once per consumer.
# overlay_session.cpp is the injected paths' shared bookkeeping and logger
# (vocem/overlay_session.h); a static archive, so the daemon and the window that
# never reference it never carry it.
#
# Until 0.1.8 the four files here were either listed by path in every target that
# needed them (panel.cpp and fonts.cpp, in gl/, layer/ and nine test targets) or
# were header-only and compiled into every translation unit that included them
# (apps.h at 1300 lines, theme.h at 600 -- half of each prose, all of it in every
# injected library, the daemon, the window and seven tests). A static library
# pulls in only the objects a consumer needs, so the daemon linking it gets the
# record writer and nothing that mentions ImGui.
#
# What every consumer has to agree on is stated once here, on the objects:
#   * hidden visibility, because these objects are linked into two shared
#     libraries that export exactly their hooks (entry 16, gl_entry_points).
#   * IMGUI_USE_WCHAR32 and the two IMGUI_DISABLE_* definitions, the same ones
#     every ImGui these objects are ever linked with is built under -- a layout
#     that differed between panel.o and the ImGui beside it would be silent.
#   * IM_ASSERT compiled out: panel.o and fonts.o go into somebody's game, where
#     an abort() is rule 7's failure (gl/CMakeLists.txt says why). The tests
#     that link these objects get them assert-free too; their own ImGui and
#     their own code keep their asserts, and under the default RelWithDebInfo
#     NDEBUG makes the two the same thing anyway.
#
# The 32-bit twin exists for the -m32 test executables built inside build/tests
# (panel_geometry32, system_font32): entries 30/33/34/55 are what testing the
# drawing at one width cost. It is not the build32 tree's library -- that tree is
# -m32 throughout and builds vocem_common itself at that width.

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")

function(vocem_add_common target)
    add_library(${target} STATIC
        src/apps.cpp
        src/theme.cpp
        src/overlay_log.cpp
        src/overlay_session.cpp
        src/panel.cpp
        src/fonts.cpp
    )
    target_include_directories(${target}
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/common/fonts" "${IMGUI_DIR}")
    target_compile_definitions(${target} PRIVATE
        IMGUI_USE_WCHAR32
        IMGUI_DISABLE_FILE_FUNCTIONS
        IMGUI_DISABLE_DEBUG_TOOLS
        "IM_ASSERT(_EXPR)=((void)0)")
    target_compile_options(${target} PRIVATE -Wall -Wextra ${ARGN})
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)
endfunction()

vocem_add_common(vocem_common)

if(VOCEM_HAVE_M32)
    vocem_add_common(vocem_common32 -m32)
endif()
