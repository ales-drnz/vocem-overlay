# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

add_executable(vocem_config_roundtrip config_roundtrip.cpp)
target_include_directories(vocem_config_roundtrip PRIVATE "${CMAKE_SOURCE_DIR}/include")

# Once per locale. The Italian one is the case that broke in the first place; if it
# is not generated on this machine the test reports itself as skipped rather than
# passing without having run.
add_test(NAME config_roundtrip_c
    COMMAND vocem_config_roundtrip C)
add_test(NAME config_roundtrip_it
    COMMAND vocem_config_roundtrip it_IT.UTF-8)
set_tests_properties(config_roundtrip_it PROPERTIES SKIP_RETURN_CODE 77)

# Two writes inside one second must both reach the overlay: the live reload
# compared whole seconds, so the second Apply of a busy second was invisible
# until the file moved again. Deterministic via utimensat, no sleeping.
add_executable(vocem_live_config_reparse live_config_reparse.cpp)
target_include_directories(vocem_live_config_reparse PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME live_config_reparse COMMAND vocem_live_config_reparse)

# Whether the palette can be read on the box it is drawn on. Header-only, like the
# roundtrip above: theme_for() is a pure function of the settings, so this needs
# neither ImGui nor a font atlas, and holding the colours to something before they
# reach a screen is the whole point.
add_executable(vocem_theme_contrast theme_contrast.cpp)
target_include_directories(vocem_theme_contrast PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME theme_contrast COMMAND vocem_theme_contrast)

# The font atlas, built exactly as the overlay builds it but with no renderer
# attached. Needs ImGui's core, not its backends.
set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")
add_executable(vocem_font_atlas
    font_atlas.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_font_atlas PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_font_atlas PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
set_source_files_properties(
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")

add_test(NAME font_atlas COMMAND vocem_font_atlas)

# The fonts across the death of the ImGui context they were built in -- the
# lifecycle both injected paths actually have (the GL toggle, a second Vulkan
# device), which used to leave fonts() pointing into a freed atlas and crash the
# game on the first frame after the overlay came back.
add_executable(vocem_fonts_lifecycle
    fonts_lifecycle.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_fonts_lifecycle PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_fonts_lifecycle PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
add_test(NAME fonts_lifecycle COMMAND vocem_fonts_lifecycle)

# The font the settings name, drawn -- with Inter merged underneath it for what
# it does not carry, and a spoken reason whenever the file cannot be used.
# Measured against this machine's own installed fonts, so it skips (77) on a
# machine with none.
add_executable(vocem_system_font
    system_font.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_system_font PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_system_font PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
add_test(NAME system_font COMMAND vocem_system_font)
set_tests_properties(system_font PROPERTIES SKIP_RETURN_CODE 77)

# The panel's geometry, measured out of the vertices ImGui produces rather than
# judged from a screenshot. Same sources as above plus the panel itself, which is
# the point: this runs the code the layer runs.
add_executable(vocem_panel_geometry
    panel_geometry.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${PROJECT_SOURCE_DIR}/common/src/panel.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_panel_geometry PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_panel_geometry PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)

add_test(NAME panel_geometry COMMAND vocem_panel_geometry)

# Not a test: a channel that is not there, for looking at the configuration window
# with a full one in it. scripts/compare-preview.py runs it against a private
# /dev/shm so it cannot be mistaken for the daemon.
add_executable(vocem_fake_state fake_state.cpp
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp")
target_include_directories(vocem_fake_state PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/daemon/src")
target_link_libraries(vocem_fake_state PRIVATE rt)

# The list of applications the overlay stays out of, and the record each process
# writes of itself. Both are read from inside other people's games, so both are
# held to the cases that come out of a game rather than out of a test: a name
# truncated by the kernel, a name with a space in it, a list edited by hand.
add_executable(vocem_apps apps.cpp)
target_include_directories(vocem_apps PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME apps COMMAND vocem_apps)
# One of its cases is a FIFO left where the record's temporary file goes: the
# defective version blocks in open() forever rather than failing, so the timeout
# is how that case reports itself.
set_tests_properties(apps PROPERTIES TIMEOUT 60)

# What the decision costs in the process that pays it, which is every OpenGL and
# Vulkan process in the session and not only the games. Allocations are asserted
# because they are the same on every machine; the timings are printed beside them.
add_executable(vocem_apps_cost apps_cost.cpp)
target_include_directories(vocem_apps_cost PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME apps_cost COMMAND vocem_apps_cost)

# The per-application decision, live: an edited list changes the answer on the
# next ask in the same process -- the property the Vulkan layer's decided-once
# static could not have while the window promised it for both paths.
add_executable(vocem_draw_decision draw_decision.cpp)
target_include_directories(vocem_draw_decision PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME draw_decision COMMAND vocem_draw_decision)

# The crash journal: left behind by a process that dies, taken along by one
# that unwinds cleanly, found by the scanner only when the process is gone.
add_executable(vocem_journal journal.cpp)
target_include_directories(vocem_journal PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME journal COMMAND vocem_journal)

# The clean-exit rename through the destructor's REAL door: a probe library
# dlopened the way the shim loads the heavy one, ended by its ELF destructor
# after static destruction -- the exit order in which the first version's
# rename read a freed std::string and silently did nothing, leaving every
# clean session as a false crash report. journal.cpp calls journal_end()
# directly and could never see it: the wrong witness, in the test built to
# prevent exactly that.
add_library(vocem_journal_exitprobe MODULE journal_exitprobe.cpp)
target_include_directories(vocem_journal_exitprobe PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_executable(vocem_journal_exit journal_exit.cpp)
target_link_libraries(vocem_journal_exit PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME journal_exit COMMAND vocem_journal_exit)
set_tests_properties(journal_exit PROPERTIES
    ENVIRONMENT "VOCEM_JOURNAL_EXITPROBE=$<TARGET_FILE:vocem_journal_exitprobe>"
    SKIP_RETURN_CODE 77)

# The Vulkan path's two-step handoff of a message's words: draw() records
# which toast it wants, the post-present phase fetches the words for the next
# frame, and the toast's end forgets them. The GL side has a frame-level
# witness (gl_toast_alone); Vulkan has none, and this holds the half that is
# ours rather than letting the suite imply otherwise.
add_executable(vocem_note_handoff note_handoff.cpp)
target_include_directories(vocem_note_handoff PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vocem_note_handoff PRIVATE rt)
add_test(NAME note_handoff COMMAND vocem_note_handoff)
set_tests_properties(note_handoff PROPERTIES SKIP_RETURN_CODE 77)

# Which swapchains get the overlay's colours re-encoded: exactly the two
# implemented HDR spaces, and nothing else -- HLG above all, whose encode is
# not implemented and must not inherit PQ's on the grounds that both are HDR.
add_executable(vocem_hdr_mode
    hdr_mode.cpp
    "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
)
target_include_directories(vocem_hdr_mode PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/layer/src"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_hdr_mode PRIVATE IMGUI_USE_WCHAR32)
add_test(NAME hdr_mode COMMAND vocem_hdr_mode)

# The shim's dlsym, at both the architectures the install ships.
#
# The shim interposes dlsym for the whole session, so the symbol version it looks
# the real one up by has to be right on every architecture built -- and the 32-bit
# libraries are configured in a tree of their own with tests switched off, so a
# version that resolved on x86-64 and not on i386 shipped and took the Steam client
# down with it. Hence a test built twice, out of one source, and run against the
# preloaded shim rather than against a copy of its logic.
add_executable(vocem_shim_dlsym shim_dlsym.cpp)
target_include_directories(vocem_shim_dlsym PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
target_link_libraries(vocem_shim_dlsym PRIVATE ${CMAKE_DL_LIBS})

add_test(NAME shim_dlsym COMMAND vocem_shim_dlsym)
set_tests_properties(shim_dlsym PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1")

# The 32-bit half, which is the one that failed. Built here with -m32 rather than
# taken from the separate 32-bit tree, so `ctest` in an ordinary build covers it;
# where the toolchain cannot produce a 32-bit binary the test reports itself skipped
# instead of passing without having run.
# The 32-bit injected libraries are built in a tree of their own (build32), which
# ctest does not cover -- entry 30's lesson is that this is exactly where a fault
# hides. Where that tree exists, the 32-bit probes run against it.
find_library(VOCEM_GL32_LIBRARY vocem_gl
    PATHS "${CMAKE_SOURCE_DIR}/build32/gl" NO_DEFAULT_PATH)
find_library(VOCEM_GL32_SHIM vocem_gl_shim
    PATHS "${CMAKE_SOURCE_DIR}/build32/gl" NO_DEFAULT_PATH)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-m32")
set(CMAKE_REQUIRED_LINK_OPTIONS "-m32")
check_cxx_source_compiles("#include <dlfcn.h>\nint main(){return dlvsym(0,\"\",\"\")!=0;}"
    VOCEM_HAVE_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(VOCEM_HAVE_M32)
    add_library(vocem_gl_shim32 SHARED "${CMAKE_SOURCE_DIR}/gl/src/vocem_gl_shim.cpp")
    target_include_directories(vocem_gl_shim32 PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
    target_compile_options(vocem_gl_shim32 PRIVATE -m32 -fno-exceptions -fno-rtti)
    target_compile_definitions(vocem_gl_shim32 PRIVATE
        VOCEM_LIBDIR="${CMAKE_INSTALL_PREFIX}/lib32")
    target_link_options(vocem_gl_shim32 PRIVATE -m32 -nostdlib++)
    target_link_libraries(vocem_gl_shim32 PRIVATE ${CMAKE_DL_LIBS})

    add_executable(vocem_shim_dlsym32 shim_dlsym.cpp)
    target_include_directories(vocem_shim_dlsym32 PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
    target_compile_options(vocem_shim_dlsym32 PRIVATE -m32)
    target_link_options(vocem_shim_dlsym32 PRIVATE -m32)
    target_link_libraries(vocem_shim_dlsym32 PRIVATE ${CMAKE_DL_LIBS})

    add_test(NAME shim_dlsym32 COMMAND vocem_shim_dlsym32)
    set_tests_properties(shim_dlsym32 PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim32>;VOCEM_SHIM_PRELOADED=1")

    # The *drawing* at 32 bits, which until now nothing ran. Both of these were
    # compiled at -m32 in build32 and exercised only at 64, which entries 30, 33
    # and 34 all say is not the same claim: the layout arithmetic is floats and
    # ints whose widths differ, the font file read turns an off_t into a size_t,
    # and a game under Proton is very often the 32-bit binary. Same sources, same
    # definitions, one flag apart -- so a difference that appears here is a
    # difference the architecture made and nothing else.
    foreach(probe panel_geometry system_font)
        add_executable(vocem_${probe}32
            ${probe}.cpp
            "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
            "${IMGUI_DIR}/imgui.cpp"
            "${IMGUI_DIR}/imgui_draw.cpp"
            "${IMGUI_DIR}/imgui_tables.cpp"
            "${IMGUI_DIR}/imgui_widgets.cpp")
        target_include_directories(vocem_${probe}32 PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_SOURCE_DIR}/common/fonts"
            "${IMGUI_DIR}")
        target_compile_definitions(vocem_${probe}32 PRIVATE
            IMGUI_USE_WCHAR32
            IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
        target_compile_options(vocem_${probe}32 PRIVATE -m32)
        target_link_options(vocem_${probe}32 PRIVATE -m32)
    endforeach()
    target_sources(vocem_panel_geometry32 PRIVATE
        "${PROJECT_SOURCE_DIR}/common/src/panel.cpp")

    add_test(NAME panel_geometry32 COMMAND vocem_panel_geometry32)
    add_test(NAME system_font32 COMMAND vocem_system_font32)
    set_tests_properties(system_font32 PROPERTIES SKIP_RETURN_CODE 77)
else()
    add_test(NAME shim_dlsym32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skipped: no 32-bit toolchain, so the architecture that broke is untested here")
    set_tests_properties(shim_dlsym32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
    foreach(probe panel_geometry system_font)
        add_test(NAME ${probe}32 COMMAND "${CMAKE_COMMAND}" -E echo
            "skipped: no 32-bit toolchain, so the drawing is measured at one width only")
        set_tests_properties(${probe}32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
    endforeach()
endif()

# Two dispatch functions with the same name, one process -- the Electron topology
# that -42 froze on. The stub plays ANGLE; the system's libEGL plays itself. See
# the test's own header for the sequence and the two ways it failed before.
add_library(vocem_stub_egl SHARED stub_egl.cpp)
set_target_properties(vocem_stub_egl PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_executable(vocem_shim_two_egls shim_two_egls.cpp)
target_link_libraries(vocem_shim_two_egls PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME shim_two_egls COMMAND vocem_shim_two_egls)
set_tests_properties(shim_two_egls PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_STUB_EGL=$<TARGET_FILE:vocem_stub_egl>"
    SKIP_RETURN_CODE 77)

# VOCEM_DISABLE=1 switches off the interposition itself, not only the drawing --
# the escape hatch 50-vocem.conf documents has to exist (it did not, twice).
add_executable(vocem_shim_disable shim_disable.cpp)
target_include_directories(vocem_shim_disable PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
target_link_libraries(vocem_shim_disable PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME shim_disable COMMAND vocem_shim_disable)
set_tests_properties(shim_disable PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_DISABLE=1"
    SKIP_RETURN_CODE 77)

# No file syscalls in the resolution paths, enforced by the same kind of sandbox
# that killed Discord under -41: a seccomp filter that kills on any file syscall,
# with the shim's hooks walked while nothing they look for is mapped.
add_executable(vocem_shim_seccomp shim_seccomp.cpp)
target_include_directories(vocem_shim_seccomp PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
target_link_libraries(vocem_shim_seccomp PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME shim_seccomp COMMAND vocem_shim_seccomp)
set_tests_properties(shim_seccomp PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1"
    SKIP_RETURN_CODE 77)

if(VOCEM_HAVE_M32)
    add_executable(vocem_shim_seccomp32 shim_seccomp.cpp)
    target_include_directories(vocem_shim_seccomp32 PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
    target_compile_options(vocem_shim_seccomp32 PRIVATE -m32)
    target_link_options(vocem_shim_seccomp32 PRIVATE -m32)
    target_link_libraries(vocem_shim_seccomp32 PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME shim_seccomp32 COMMAND vocem_shim_seccomp32)
    set_tests_properties(shim_seccomp32 PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim32>;VOCEM_SHIM_PRELOADED=1"
        SKIP_RETURN_CODE 77)
else()
    add_test(NAME shim_seccomp32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skipped: no 32-bit toolchain, so the architecture that broke is untested here")
    set_tests_properties(shim_seccomp32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
endif()

# One place says which symbol version dlsym has. The first time that version was
# wrong it was fixed in the shim and left alone in libvocem_gl.so, and the second
# copy went on quietly blanking every 32-bit game -- so the duplication itself is
# what this checks, since getting the value wrong is invisible from outside.
add_test(NAME one_dlsym_version
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/one_dlsym_version.cmake")

# A crash is reported inside the window's Debug section, and nowhere else: the
# dead process's journal appears in the page, and no separate crashReport
# window exists any more (it was built, fixed to be genuinely top-level -- the
# crash_window_alone test's era -- and then removed on the owner's judgement;
# DESIGN entry 60). Uses the same harness window line that test introduced.
if(TARGET vocem-config)
    add_test(NAME debug_section
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DQML_DIR=${CMAKE_SOURCE_DIR}/gui/qml"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/debug_section.cmake")

    # The Debug section reads journals and only journals: a symlink planted in
    # the journal directory was followed by the unattended four-second scan and
    # its contents shown in the window. Refused at the scanner (lstat: a journal
    # is a regular file) and at the reader (canonical paths, a size cap).
    add_test(NAME journal_paths
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/journal_paths.cmake")

    # "Drawing now" follows the world while the window is open: the live list
    # was computed once, in the constructor, so a window opened before a game
    # started said nothing was drawing while a game drew 28223 frames.
    add_test(NAME live_while_open
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/live_while_open.cmake")

    # The message preview may only promise what the daemon will publish: its
    # example body follows notification_show_body. The preview drew a body
    # unconditionally while the switch had no interface, and the owner read the
    # difference between the promise and the game as text failing to draw.
    add_test(NAME toast_preview_honest
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/toast_preview_honest.cmake")
endif()

# One place turns pixels back into a position fraction. The wrong line was written
# three times -- the old inverse survived in the drag handler, was fixed there, and
# survived again in the release handler, which shipped. The rule is about the
# duplication, as entry 33's is.
add_test(NAME one_placement_inverse
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/one_placement_inverse.cmake")

# The emoji bank is a Modified Version of Noto Color Emoji under the OFL 1.1
# and shipped through 0.1.0-52's sources with no licence text anywhere; the
# licence now lives beside the bank and installs with it, and this holds both.
add_test(NAME emoji_bank_licence
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/emoji_bank_licence.cmake")

# The same rule for the project's other one-place facts: the monotonic clock
# (vocem/clock.h), the mkdir -p loop (vocem/paths.h) and the curl write
# callback (daemon/src/curl_sink.h) each existed in several hand-written
# copies before the consolidation sweep; this keeps a new copy from growing
# back quietly.
add_test(NAME one_spelling
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/one_spelling.cmake")

# The daemon's unit: confined, and confined in ways that actually start. An empty
# CapabilityBoundingSet on a user service fails to start (exit 218, measured), so
# its absence is asserted alongside the directives that must be there.
add_test(NAME unit_hardening
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/unit_hardening.cmake")

# Anything that turns a 64-bit value into text has to produce the same text in the
# 32-bit half of the install as in the 64-bit one, because both talk to the same
# 64-bit daemon and look for the same files. Built twice for that reason; the 32-bit
# build is the one that has caught things.
add_executable(vocem_widths widths.cpp)
target_include_directories(vocem_widths PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME widths COMMAND vocem_widths)

# The DRM reader that decides how large the overlay is, against a /sys tree
# built to order rather than whatever the build machine has plugged in.
add_executable(vocem_display_height display_height.cpp)
target_include_directories(vocem_display_height PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME display_height COMMAND vocem_display_height)

# A reader that survived the daemon must be able to find the next daemon:
# unlink removes the name and not the pages, and quit-and-reopen has to work
# for the game being played, not only for the next one. Two writer lives, one
# process, a private /dev/shm.
add_executable(vocem_shm_reattach shm_reattach.cpp)
target_include_directories(vocem_shm_reattach PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_shm_reattach PRIVATE rt)
add_test(NAME shm_reattach COMMAND vocem_shm_reattach)
set_tests_properties(shm_reattach PROPERTIES SKIP_RETURN_CODE 77)

# A shared segment shorter than the struct maps fine and kills the reader on the
# first touch. The writer creates the name and sizes it in two calls, so a daemon
# that dies between them leaves one behind -- and every game in the session then
# takes SIGBUS. The read runs in a forked child so the signal is a result.
add_executable(vocem_shm_short_segment shm_short_segment.cpp)
target_include_directories(vocem_shm_short_segment PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_shm_short_segment PRIVATE rt)
add_test(NAME shm_short_segment COMMAND vocem_shm_short_segment)
set_tests_properties(shm_short_segment PROPERTIES SKIP_RETURN_CODE 77)

# The overlay must draw for a game whose GL arrives by dlopen(RTLD_LOCAL) --
# Minecraft, GLFW, LWJGL, SDL: the level-3 door. The library shipped in -44
# resolved its own GL lookups through scopes that are empty in exactly those
# games, and the overlay drew nothing while logging that it was drawing. Needs a
# display and bwrap; skips where either is missing. Opens a small window for a
# moment, which is the honest cost of measuring pixels.
add_executable(vocem_gl_draw_local gl_draw_local.cpp)
target_include_directories(vocem_gl_draw_local PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_draw_local PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_draw_local COMMAND vocem_gl_draw_local)
set_tests_properties(gl_draw_local PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# The overlay's colours in a game that writes sRGB. GL_FRAMEBUFFER_SRGB makes the
# hardware encode whatever the shader writes, and ImGui's colours are encoded
# already: the panel's surface came out lifted, white and saturated colours
# unchanged, which is why it hid. The same measurement the Vulkan half makes
# through the swapchain's format (hdr_srgb_pixels), on the switch this side has.
add_executable(vocem_gl_srgb_write gl_srgb_write.cpp)
target_include_directories(vocem_gl_srgb_write PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_srgb_write PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_srgb_write COMMAND vocem_gl_srgb_write)
set_tests_properties(gl_srgb_write PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# A process the overlay declines must be quiet on the X wire: the present hook
# used to pay two glXQueryDrawable X round trips per frame in every GL process
# of the session, before asking whether it would draw at all. Compares X request
# serials between a disabled child and a declining one; the -50 library shows
# +3 requests per frame, the fixed one 0. Needs a display and real GLX.
add_executable(vocem_gl_noop_quiet gl_noop_quiet.cpp)
target_include_directories(vocem_gl_noop_quiet PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_noop_quiet PRIVATE GL X11)
add_test(NAME gl_noop_quiet COMMAND vocem_gl_noop_quiet)
set_tests_properties(gl_noop_quiet PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# A toast with nobody in a voice channel: the frame guard used to ask only
# about the channel, in both injection paths, which made build_notification
# unreachable in exactly the situation a "somebody wrote to you" toast is for.
# Same miniature dlopen-local game as gl_draw_local, publishing a fresh
# notification and an empty channel; fails against the library shipped in -50.
add_executable(vocem_gl_toast_alone gl_toast_alone.cpp)
target_include_directories(vocem_gl_toast_alone PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_toast_alone PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_toast_alone COMMAND vocem_gl_toast_alone)
set_tests_properties(gl_toast_alone PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# Colour emoji all the way to a real framebuffer, which emoji_atlas cannot claim:
# it proves the bank reaches the atlas, and everything between the atlas and the
# screen is the library. Differential rather than a colour detector -- the same
# scene twice, bank reachable and bank absent -- because the speaking ring is
# green and the muted badge is red, so a colour filter on a capture lies (DESIGN).
# Point VOCEM_GL_LIBRARY at a packaged file and this measures what ships.
add_executable(vocem_gl_emoji_colour gl_emoji_colour.cpp)
target_include_directories(vocem_gl_emoji_colour PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_emoji_colour PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_emoji_colour COMMAND vocem_gl_emoji_colour)
set_tests_properties(gl_emoji_colour PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>;VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba"
    SKIP_RETURN_CODE 77)

# What a drawn frame costs the filesystem on the avatar path -- where entry 49's
# filter stopped. Counting rather than killing, because this path is SUPPOSED to
# touch files: the probe interposes the file syscalls in its own executable (a
# definition in the main program wins over the dlopened library's lookups) and
# reports per-frame numbers across the three phases a game actually goes through:
# nobody's picture arrived yet, the pictures land, everything resolved.
add_executable(vocem_gl_avatar_quiet gl_avatar_quiet.cpp)
target_include_directories(vocem_gl_avatar_quiet PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_avatar_quiet PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_avatar_quiet COMMAND vocem_gl_avatar_quiet)
set_tests_properties(gl_avatar_quiet PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# The upload reads its pixels through the game's GL_UNPACK_* state and its
# pixel-unpack buffer, which the overlay neither neutralised nor restored: a
# miniature game leaves a hostile one bound and asks GL afterwards what it finds.
add_executable(vocem_gl_unpack_state gl_unpack_state.cpp)
target_include_directories(vocem_gl_unpack_state PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vocem_gl_unpack_state PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_unpack_state COMMAND vocem_gl_unpack_state)
set_tests_properties(gl_unpack_state PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

# The segment's layout at both widths, and crossed: the daemon is 64-bit and every
# 32-bit game reads what it wrote, so the struct's layout IS the ABI -- and it did
# not hold. `User` starts with a uint64_t, whose alignment is 8 on x86-64 and 4 on
# i386, so `users` began at 96 in the daemon and at 92 in every 32-bit game. The
# earlier 32-bit probes could not see it: they published with a writer of their own
# width, so writer and reader were wrong together. Here each width writes a segment
# and the *other* reads it.
add_executable(vocem_shared_state_layout shared_state_layout.cpp)
target_include_directories(vocem_shared_state_layout PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME shared_state_layout COMMAND vocem_shared_state_layout)

if(VOCEM_HAVE_M32)
    add_executable(vocem_shared_state_layout32 shared_state_layout.cpp)
    target_include_directories(vocem_shared_state_layout32 PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(vocem_shared_state_layout32 PRIVATE -m32)
    target_link_options(vocem_shared_state_layout32 PRIVATE -m32)
    add_test(NAME shared_state_layout32 COMMAND vocem_shared_state_layout32)

    # 64 writes, 32 reads -- the daemon-to-Isaac direction, which is the one that
    # was broken -- and then the other way round for symmetry. The dependencies
    # make the order real rather than hoped for.
    add_test(NAME shared_state_cross_write64
        COMMAND vocem_shared_state_layout write "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin")
    add_test(NAME shared_state_cross_read32
        COMMAND vocem_shared_state_layout32 read "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin")
    set_tests_properties(shared_state_cross_read32 PROPERTIES
        DEPENDS shared_state_cross_write64 SKIP_RETURN_CODE 77)
    add_test(NAME shared_state_cross_write32
        COMMAND vocem_shared_state_layout32 write "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin")
    add_test(NAME shared_state_cross_read64
        COMMAND vocem_shared_state_layout read "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin")
    set_tests_properties(shared_state_cross_read64 PROPERTIES
        DEPENDS shared_state_cross_write32 SKIP_RETURN_CODE 77)
else()
    add_test(NAME shared_state_layout32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skip needs a 32-bit toolchain")
    set_tests_properties(shared_state_layout32 PROPERTIES SKIP_REGULAR_EXPRESSION "^skip")
endif()

# A face on screen, at both widths, through the GL path -- because the width that
# has failed invisibly three times (entries 30, 33, 34) is the one the owner's
# Isaac runs at: its record says api = opengl and wine-preloader i386-unix. The
# 64-bit build is the control; the 32-bit build is the question. Both use the
# libraries of their own width, so the shim, the heavy library and the probe agree
# about what a pointer is.
add_executable(vocem_gl_avatar_width gl_avatar_width.cpp)
target_include_directories(vocem_gl_avatar_width PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(vocem_gl_avatar_width PRIVATE ${CMAKE_DL_LIBS} X11 rt)
add_test(NAME gl_avatar_width COMMAND vocem_gl_avatar_width)
set_tests_properties(gl_avatar_width PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
    SKIP_RETURN_CODE 77)

if(VOCEM_HAVE_M32 AND VOCEM_GL32_LIBRARY AND VOCEM_GL32_SHIM)
    add_executable(vocem_gl_avatar_width32 gl_avatar_width.cpp)
    target_include_directories(vocem_gl_avatar_width32 PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(vocem_gl_avatar_width32 PRIVATE -m32)
    target_link_options(vocem_gl_avatar_width32 PRIVATE -m32)
    target_link_libraries(vocem_gl_avatar_width32 PRIVATE ${CMAKE_DL_LIBS} X11 rt)
    add_test(NAME gl_avatar_width32 COMMAND vocem_gl_avatar_width32)
    set_tests_properties(gl_avatar_width32 PROPERTIES
        ENVIRONMENT "LD_PRELOAD=${VOCEM_GL32_SHIM};VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=${VOCEM_GL32_LIBRARY}"
        SKIP_RETURN_CODE 77)
else()
    add_test(NAME gl_avatar_width32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skip needs a 32-bit toolchain and the 32-bit libraries (build32)")
    set_tests_properties(gl_avatar_width32 PROPERTIES SKIP_REGULAR_EXPRESSION "^skip")
endif()

# Beside Steam's own overlay. DESIGN listed this as a declared class never
# measured: Steam appends gameoverlayrenderer.so to LD_PRELOAD for everything it
# launches, so in a native GL Steam title both interposers are in the chain, and
# "both forward, so it should stack" was an expectation. Measured now, in both
# orders, with the same probe and the same pixel count -- Steam's library hooks
# glXSwapBuffers AND dlsym, so whichever is first in LD_PRELOAD owns the door and
# the other must still see the frame. Skipped where Steam is not installed.
find_file(VOCEM_STEAM_OVERLAY gameoverlayrenderer.so
    PATHS "$ENV{HOME}/.steam/root/ubuntu12_64" "$ENV{HOME}/.local/share/Steam/ubuntu12_64"
    NO_DEFAULT_PATH)
if(VOCEM_STEAM_OVERLAY)
    # The order Steam actually produces: the session's preload first (ours, from
    # environment.d), Steam's appended after it.
    add_test(NAME gl_beside_steam COMMAND vocem_gl_draw_local)
    set_tests_properties(gl_beside_steam PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>:${VOCEM_STEAM_OVERLAY};VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
        SKIP_RETURN_CODE 77)
    # And the harder way round, where Steam's hook owns the symbol and ours only
    # sees the frame if Steam forwards down the chain.
    add_test(NAME gl_under_steam COMMAND vocem_gl_draw_local)
    set_tests_properties(gl_under_steam PROPERTIES
        ENVIRONMENT "LD_PRELOAD=${VOCEM_STEAM_OVERLAY}:$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
        SKIP_RETURN_CODE 77)
else()
    add_test(NAME gl_beside_steam COMMAND "${CMAKE_COMMAND}" -E echo
        "skip Steam's gameoverlayrenderer.so is not installed here")
    set_tests_properties(gl_beside_steam PROPERTIES SKIP_REGULAR_EXPRESSION "^skip")
endif()

# The avatar upload's command buffer through the loader contract: a dispatchable
# object a layer creates is registered with pfnSetDeviceLoaderData or dispatch
# on it crashes (layer rule 4). The TextureCache allocated one under a comment
# claiming the renderer had registered it, and nothing had. Runs the real
# TextureCache against a stub Vulkan supplied through its own resolver seam.
add_executable(vocem_texture_loader_data
    texture_loader_data.cpp
    "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
)
target_include_directories(vocem_texture_loader_data PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/layer/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_compile_definitions(vocem_texture_loader_data PRIVATE IMGUI_USE_WCHAR32)
add_test(NAME texture_loader_data COMMAND vocem_texture_loader_data)

# The avatar cache against its descriptor budget: AddTexture on an exhausted
# pool updates an uninitialised descriptor set and the driver dies on it --
# the Minecraft crash of 2026-07-29, symbolised out of the core dump. The
# cache must stop at its budget and hand out placeholders beyond it.
add_executable(vocem_texture_descriptor_budget
    texture_descriptor_budget.cpp
    "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
)
target_include_directories(vocem_texture_descriptor_budget PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/layer/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_compile_definitions(vocem_texture_descriptor_budget PRIVATE IMGUI_USE_WCHAR32)
add_test(NAME texture_descriptor_budget COMMAND vocem_texture_descriptor_budget)

# The colour emoji bank's reader, at both widths: written by a 64-bit script,
# read inside games of both, which is the exact shape of entries 30/33/34.
add_executable(vocem_emoji_bank emoji_bank.cpp)
target_include_directories(vocem_emoji_bank PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME emoji_bank COMMAND vocem_emoji_bank)
set_tests_properties(emoji_bank PROPERTIES
    ENVIRONMENT "VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba"
    SKIP_RETURN_CODE 77)

if(VOCEM_HAVE_M32)
    add_executable(vocem_emoji_bank32 emoji_bank.cpp)
    target_include_directories(vocem_emoji_bank32 PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(vocem_emoji_bank32 PRIVATE -m32)
    target_link_options(vocem_emoji_bank32 PRIVATE -m32)
    add_test(NAME emoji_bank32 COMMAND vocem_emoji_bank32)
    set_tests_properties(emoji_bank32 PROPERTIES
        ENVIRONMENT "VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba"
        SKIP_RETURN_CODE 77)
else()
    add_test(NAME emoji_bank32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skipped: no 32-bit toolchain, so the half that keeps breaking is untested here")
    set_tests_properties(emoji_bank32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
endif()

# A colour emoji seen in a name reaches the atlas in colour, in both weights,
# and a new one triggers a rebuild the way a size change does. Fails against
# the atlas as it stood: the glyph existed, white.
add_executable(vocem_emoji_atlas
    emoji_atlas.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_emoji_atlas PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_emoji_atlas PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
add_test(NAME emoji_atlas COMMAND vocem_emoji_atlas)
set_tests_properties(emoji_atlas PROPERTIES
    ENVIRONMENT "VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba"
    SKIP_RETURN_CODE 77)

# The frame path's silence, held by a seccomp filter rather than by reading:
# fonts_note_emoji runs inside the present on both paths, and what makes that
# legal is that every codepoint costs its bank lookup exactly once -- rejections
# remembered as well as hits. The layer's comment used to claim "no I/O at all",
# which was never quite the property it had.
add_executable(vocem_fonts_frame_quiet
    fonts_frame_quiet.cpp
    "${PROJECT_SOURCE_DIR}/common/src/fonts.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(vocem_fonts_frame_quiet PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/common/fonts"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_fonts_frame_quiet PRIVATE
    IMGUI_USE_WCHAR32
    IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
add_test(NAME fonts_frame_quiet COMMAND vocem_fonts_frame_quiet)
set_tests_properties(fonts_frame_quiet PROPERTIES
    ENVIRONMENT "VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba"
    SKIP_RETURN_CODE 77)

# The daemon's decode, which is the project's trust boundary: bytes from the
# internet against the only parser in the install. It counts what stb's
# allocator is asked for, because both defects it holds -- a decompression bomb
# inflated in full before the size check refused it, and a migration read that
# followed a symlink to /dev/zero and never returned -- are about cost, and a
# test comparing booleans passes against both.
find_package(ZLIB REQUIRED)
add_executable(vocem_avatar_decode avatar_decode.cpp)
target_include_directories(vocem_avatar_decode PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/daemon/src"
    "${CMAKE_SOURCE_DIR}/third_party")
target_compile_options(vocem_avatar_decode PRIVATE -Wno-unused-function)
target_link_libraries(vocem_avatar_decode PRIVATE ZLIB::ZLIB)
add_test(NAME avatar_decode COMMAND vocem_avatar_decode)
set_tests_properties(avatar_decode PROPERTIES SKIP_RETURN_CODE 77)

# The faces must not change when the decoder does. The daemon decoded with
# stb_image up to 0.1.0-53 and with Wuffs after it; every other test is blind to
# that swap, because the cache file has the same name and length whichever
# decoder wrote it -- a decoder that swapped red for blue would pass all of them.
# This one holds the new decoder to the picture itself AND to the old decoder,
# byte for byte, across six colour types.
add_executable(vocem_avatar_decoders_agree avatar_decoders_agree.cpp)
target_include_directories(vocem_avatar_decoders_agree PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/daemon/src"
    "${CMAKE_SOURCE_DIR}/third_party")
target_compile_options(vocem_avatar_decoders_agree PRIVATE -Wno-unused-function)
target_link_libraries(vocem_avatar_decoders_agree PRIVATE ZLIB::ZLIB)
add_test(NAME avatar_decoders_agree COMMAND vocem_avatar_decoders_agree)
set_tests_properties(avatar_decoders_agree PROPERTIES SKIP_RETURN_CODE 77)

# The avatar cache format, from both ends: the daemon's writer (resample and
# rename into place) and the games' reader, whose strictness -- exactly one
# size or nothing -- is the entire format now that no image parser runs inside
# a game process. Built at both widths below, beside the paths.
add_executable(vocem_avatar_rgba avatar_rgba.cpp)
target_include_directories(vocem_avatar_rgba PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME avatar_rgba COMMAND vocem_avatar_rgba)

if(VOCEM_HAVE_M32)
    add_executable(vocem_avatar_rgba32 avatar_rgba.cpp)
    target_include_directories(vocem_avatar_rgba32 PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(vocem_avatar_rgba32 PRIVATE -m32)
    target_link_options(vocem_avatar_rgba32 PRIVATE -m32)
    add_test(NAME avatar_rgba32 COMMAND vocem_avatar_rgba32)
else()
    add_test(NAME avatar_rgba32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skipped: no 32-bit toolchain, so the half that keeps breaking is untested here")
    set_tests_properties(avatar_rgba32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
endif()

# What a message becomes by the time a game can read it: the real vocemd, driven
# by a stub Discord RPC, with the segment read back the way a game reads it. The
# body of a message must stay out of the segment unless the settings say
# otherwise -- the daemon as shipped in 0.1.0-43 published it unconditionally,
# and this fails against that binary. Sandboxed under bwrap (private /dev/shm,
# private network) and skipped where bwrap is missing.
if(TARGET vocemd)
    add_executable(vocem_daemon_notification daemon_notification.cpp)
    target_include_directories(vocem_daemon_notification PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(vocem_daemon_notification PRIVATE rt)
    add_test(NAME daemon_notification COMMAND vocem_daemon_notification)
    set_tests_properties(daemon_notification PROPERTIES
        ENVIRONMENT "VOCEM_DAEMON=$<TARGET_FILE:vocemd>"
        SKIP_RETURN_CODE 77)

    # What the daemon does when the port answers and the session does not. The
    # backoff covered "nothing is listening" and not "something took the
    # connection and dropped it", which is what Discord's own client does to an
    # RPC connection it will not serve: measured against the packaged 0.1.4-1
    # daemon, 1914 connections in five seconds against 3. Same sandbox as above.
    add_executable(vocem_daemon_reconnect daemon_reconnect.cpp)
    add_test(NAME daemon_reconnect COMMAND vocem_daemon_reconnect)
    set_tests_properties(daemon_reconnect PROPERTIES
        ENVIRONMENT "VOCEM_DAEMON=$<TARGET_FILE:vocemd>"
        TIMEOUT 120
        SKIP_RETURN_CODE 77)
endif()

# The one place in this project that reads bytes off a socket, held against a peer
# that is not Discord. Compiles the daemon's WebSocket client directly and talks to
# it from a stub on an ephemeral loopback port: no daemon, no /dev/shm, no 6463,
# so it needs neither bwrap nor a sandbox.
# Who the daemon believes is on the other end before it sends a token with
# messages.read scope. Three cases: the real /proc for a connection this process
# owns, a dual-stack listener whose row lives in /proc/net/tcp6 (a case that
# answers Unknown if only /proc/net/tcp is read, which is every ordinary Discord
# client), and a hand-written row owned by somebody else.
add_executable(vocem_peer_identity peer_identity.cpp)
target_include_directories(vocem_peer_identity PRIVATE "${CMAKE_SOURCE_DIR}/daemon/src")
add_test(NAME peer_identity COMMAND vocem_peer_identity)

add_executable(vocem_daemon_ws_bounds daemon_ws_bounds.cpp
    "${CMAKE_SOURCE_DIR}/daemon/src/websocket.cpp")
target_include_directories(vocem_daemon_ws_bounds PRIVATE "${CMAKE_SOURCE_DIR}/daemon/src")
add_test(NAME daemon_ws_bounds COMMAND vocem_daemon_ws_bounds)
set_tests_properties(daemon_ws_bounds PROPERTIES TIMEOUT 120)

if(VOCEM_HAVE_M32)
    add_executable(vocem_widths32 widths.cpp)
    target_include_directories(vocem_widths32 PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(vocem_widths32 PRIVATE -m32)
    target_link_options(vocem_widths32 PRIVATE -m32)
    add_test(NAME widths32 COMMAND vocem_widths32)
else()
    add_test(NAME widths32 COMMAND "${CMAKE_COMMAND}" -E echo
        "skipped: no 32-bit toolchain, so the half that keeps breaking is untested here")
    set_tests_properties(widths32 PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")
endif()

# The HDR pipelines executed on the GPU: the shipped SPIR-V through the real
# hdr_pipeline_create (compiled in, not copied), a quad of known sRGB colours
# drawn to an offscreen fp16 target on a surfaceless device, pixels read back
# against references recomputed from the standard constants -- the half of the
# HDR open risk that the arithmetic replication could not reach. No window, no
# WSI; skips 77 where no Vulkan loader or device exists.
add_executable(vocem_hdr_pixels
    hdr_pixels.cpp
    "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
)
target_include_directories(vocem_hdr_pixels PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/layer/src"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_hdr_pixels PRIVATE IMGUI_USE_WCHAR32)
target_link_libraries(vocem_hdr_pixels PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME hdr_pixels COMMAND vocem_hdr_pixels)
set_tests_properties(hdr_pixels PROPERTIES SKIP_RETURN_CODE 77)

# The same source against an sRGB-format target, which is the ordinary swapchain
# and not an HDR one: there the format carries the encoding, the hardware applies
# linear->sRGB to whatever the shader writes, and the claim is a round trip --
# what the attachment stores is the byte ImGui asked for. Against the shader
# before mode 3 existed it stores the sRGB encode of an already-encoded value:
# measured 128 -> 188, and the panel's own 79,84,92 -> 151,155,162.
add_executable(vocem_hdr_srgb_pixels
    hdr_pixels.cpp
    "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
)
target_include_directories(vocem_hdr_srgb_pixels PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/layer/src"
    "${IMGUI_DIR}"
)
target_compile_definitions(vocem_hdr_srgb_pixels PRIVATE IMGUI_USE_WCHAR32 VOCEM_SRGB_TARGET)
target_link_libraries(vocem_hdr_srgb_pixels PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME hdr_srgb_pixels COMMAND vocem_hdr_srgb_pixels)
set_tests_properties(hdr_srgb_pixels PROPERTIES SKIP_RETURN_CODE 77)

# The Appearance page previews its presets as four real panels built from the
# theme each preset would produce (distinct surfaces, one active mark), and
# Appearance and Spacing carry a live preview of both boxes beside their
# controls. Read out of the geometry dump; against the window before these
# previews existed the preset row was colour chips and neither page had a
# preview column, and this fails there.
if(TARGET vocem-config)
    add_test(NAME appearance_previews
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/appearance_previews.cmake")
endif()

# Which height sizes the atlas, in one spelling for both injected paths. The
# display wins over a window (entry 39) -- but a drawable TALLER than every
# display (supersampling/DSR) is headed for the compositor's downscale, and
# sizing it by the display landed the overlay at 0.5x on screen: the
# illegibly-small direction. Fails against the old per-path ternary, which
# answered the display's height for the supersampled case.
add_executable(vocem_sizing_height sizing_height.cpp)
target_include_directories(vocem_sizing_height PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${IMGUI_DIR}")
target_compile_definitions(vocem_sizing_height PRIVATE IMGUI_USE_WCHAR32)
add_test(NAME sizing_height COMMAND vocem_sizing_height)

# The tray's four state icons: present, token-true, well-formed, installed.
# A missing or broken state icon degrades to the generic one with nothing
# logged, which is the silence this suite exists to refuse.
add_test(NAME tray_icons
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tray_icons.cmake")

# A game running right now with the overlay off is said above the history:
# the Applications page's answer to the question somebody opens it with
# mid-session, held on the real window against a record for pid 1's name.
if(TARGET vocem-config)
    add_test(NAME running_without
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/running_without.cmake")
endif()

# A map depicting a display the overlay is not sized for shows the boxes at the
# share of THAT screen they will really cover, and each map's dropdown chooses
# the display. Fabricated two-display DRM tree via VOCEM_DRM_ROOT, real window
# offscreen; fails against the window before the dropdown existed (the
# packaged 0.1.0-59 exemplar), where the key is ignored and the ratio is 1.
if(TARGET vocem-config)
    add_test(NAME preview_display
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/preview_display.cmake")
endif()

# One padding, in every card, on every page of the settings window, at the
# default size and at the minimum. Every card carries objectName "card" and
# every padded block inside one "cardContent", so a card that is taller than
# the blocks in it is an item sitting in a card without taking the card's
# padding -- the seam the owner spotted by eye in the Debug section, and which
# was still on the Applications page in 0.1.0-64. Against that packaged window
# nothing is named and the check reports it measured no cards at all.
if(TARGET vocem-config)
    add_test(NAME window_padding
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/window_padding.cmake")
endif()

# A switch means the click did something. Three settings in this window are
# written the moment they are clicked and reach a running game a couple of
# seconds later; every other boolean waits for Apply, and the KDE guidelines
# call that a checkbox. Ten of them were switches through 0.1.0-64, and this
# fails against those sources.
add_test(NAME instant_switches
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/instant_switches.cmake")

# An empty view says so with a placeholder message, on every page that has one.
# Measured on the real window with nothing on disk; against 0.1.0-64 the Debug
# section has its placeholders and the Applications page answers the same
# situation with a card holding one row, so this fails on section 5.
if(TARGET vocem-config)
    add_test(NAME empty_views
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/empty_views.cmake")
endif()

# Where you are is asked, not remembered, and the names arrive without the
# invisible characters Discord wraps them in. Both are the owner's report from
# 0.1.0-65: moved into another channel by somebody else, the panel went on
# naming the old one; and question marks in the message box, which turned out to
# be U+2068/U+2069 with no glyph in the atlas. The real vocemd against a stub
# Discord RPC, sandboxed like daemon_notification.
if(TARGET vocemd)
    add_executable(vocem_daemon_moved daemon_moved.cpp)
    target_include_directories(vocem_daemon_moved PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(vocem_daemon_moved PRIVATE rt)
    add_test(NAME daemon_moved COMMAND vocem_daemon_moved)
    set_tests_properties(daemon_moved PROPERTIES
        ENVIRONMENT "VOCEM_DAEMON=$<TARGET_FILE:vocemd>"
        SKIP_RETURN_CODE 77)
endif()

# The System tray section: which picture the panel wears, and a preview that
# really draws it. The second half is the one that earned the test -- with the
# desktop style loaded, QIcon::fromTheme resolves every Breeze name this window
# asks for and none of this application's own six, so the About page had been
# drawing an empty square where its icon goes. The artwork is carried in the
# binary now; this holds the preview to six pictures that actually loaded.
if(TARGET vocem-config)
    add_test(NAME tray_icon_choice
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tray_icon_choice.cmake")
endif()

# The Flatpak bridge, both directions.
#
# flatpak_bridge holds the daemon's writer and the injected code's reader against
# each other across a fake $XDG_RUNTIME_DIR/app: the daemon publishes, a forked
# child with FLATPAK_ID set enters the bridge exactly as the overlay does and
# reads back the state, the settings and the avatar -- after unlinking the POSIX
# segment, so nothing it gets right can have come from /dev/shm. A private
# /dev/shm, because a StateWriter is opened.
add_executable(vocem_flatpak_bridge flatpak_bridge.cpp
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp")
target_include_directories(vocem_flatpak_bridge PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/daemon/src")
target_link_libraries(vocem_flatpak_bridge PRIVATE rt)
add_test(NAME flatpak_bridge COMMAND vocem_flatpak_bridge)
set_tests_properties(flatpak_bridge PROPERTIES SKIP_RETURN_CODE 77)

# The same directory, seen as what it is: writable by a sandbox that the daemon
# has no reason to trust, while the daemon itself runs as the user with the
# user's home reachable. Every name it opens down there is a symbolic link here.
add_executable(vocem_flatpak_bridge_hostile flatpak_bridge_hostile.cpp
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp")
target_include_directories(vocem_flatpak_bridge_hostile PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/daemon/src")
add_test(NAME flatpak_bridge_hostile COMMAND vocem_flatpak_bridge_hostile)

# One installed package, one answer to "which version is this?".
#
# The installed 0.1.4-1 gave three: pacman said 0.1.4, the About page said
# 0.1.0, the metainfo's newest release said 0.1.2 and .SRCINFO said 0.1.2 too.
# CMakeLists.txt states the release; neither PKGBUILD passes it to CMake, so the
# number in the binary and the number on the package had never been related.
#
# Not equality: the two PKGBUILDs are meant to differ mid-cycle (one builds the
# working tree, one builds a tag), so this holds each to its own rule and prints
# which state it matched. It also runs the built window and asks what version it
# carries, because every source-level claim about this was already fine while the
# binary said 0.1.0.
add_test(NAME version_agrees
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DCONFIG_BINARY=$<IF:$<TARGET_EXISTS:vocem-config>,$<TARGET_FILE:vocem-config>,>"
            "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/version_agrees.cmake")

# Whose clock the words of a message are retired on. The tick that scrubs and
# unlinks the note segment was reached only from the recv loop of a LIVE Discord
# connection, so Discord quitting left the text of the last direct message in
# /dev/shm -- readable by every process in the session -- for as long as it
# stayed away: measured against the packaged 0.1.4-1 daemon, still there forty
# seconds after the connection closed, which is only where the counting stopped.
# Second case in the same file: a SIGKILLed daemon runs no destructor, and the
# unit carries Restart=on-failure with MemoryMax=128M, so the next daemon is
# what has to decline the inheritance. daemon_notification's sandbox.
if(TARGET vocemd)
    add_executable(vocem_daemon_note_expiry daemon_note_expiry.cpp)
    target_include_directories(vocem_daemon_note_expiry PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(vocem_daemon_note_expiry PRIVATE rt)
    add_test(NAME daemon_note_expiry COMMAND vocem_daemon_note_expiry)
    set_tests_properties(daemon_note_expiry PROPERTIES
        ENVIRONMENT "VOCEM_DAEMON=$<TARGET_FILE:vocemd>"
        TIMEOUT 240
        SKIP_RETURN_CODE 77)
endif()

# The map of the display follows the machine while the window is open.
#
# environment.h answered the display question once, on first use, and a tray
# application started at login means "once" is "at login": a monitor plugged in
# afterwards resized the overlay within a minute while the picture of it went on
# drawing the display that was there this morning. preview_display.cmake could
# not see it -- it sets VOCEM_DRM_ROOT before launching, so it can only ever
# measure what the window knew at birth. This one plants the second connector
# 1.6 s in, the way live_while_open.cmake plants a journal, and holds both maps
# to the new display's shape. Against the packaged 0.1.4-1 window every section
# stays 5:4.
if(TARGET vocem-config)
    add_test(NAME display_change_while_open
        COMMAND "${CMAKE_COMMAND}"
                "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/display_change_while_open.cmake")
endif()

# A settings value is as long as the user's list, not as long as a buffer.
#
# Config::load() read the file with char line[256] and a bare fgets, so a line
# past 255 bytes arrived in pieces: the first piece parsed short and the rest,
# having no '=' in it, was skipped in silence. hidden_apps came back truncated
# in the games the user had just hidden, and the window -- which reads this same
# header -- wrote the truncation back on the next Apply, so the entries left the
# file for good. Measured against 0.1.4's header: 330 bytes in, 241 out, ending
# mid-name. The owner's own file was at 179 bytes of that budget.
add_executable(vocem_config_long_line config_long_line.cpp)
target_include_directories(vocem_config_long_line PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_test(NAME config_long_line COMMAND vocem_config_long_line)

# What the shim's dispatch hooks hand back when the real dispatcher is not the
# system's.
#
# The dlsym hook substitutes our hook only inside if (is_system_gl(real)), so a
# private GL keeps its private names -- entry 36. dispatch() has the same test
# and returns entry->hook from OUTSIDE it. Constructed here rather than reasoned
# about: a private EGL opened RTLD_GLOBAL lands in the shim's own RTLD_NEXT
# order, and from then on the two roads answer differently for one name in one
# process -- dlsym with the private library's function, the dispatcher with our
# hook. Not reachable with RTLD_LOCAL, which is what Chromium actually does.
#
# Left as a measurement, not a fix: the symmetric change would make the shim
# hand a private dispatcher's answer straight back, which is entry 35's defect
# in the case where next legitimately is the application's own linked GL.
add_library(vocem_stub_private_egl SHARED stub_private_egl.cpp)
set_target_properties(vocem_stub_private_egl PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_executable(vocem_shim_private_dispatch shim_private_dispatch.cpp)
target_link_libraries(vocem_shim_private_dispatch PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME shim_private_dispatch COMMAND vocem_shim_private_dispatch)
set_tests_properties(shim_private_dispatch PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>;VOCEM_SHIM_PRELOADED=1;VOCEM_STUB_PRIVATE_EGL=$<TARGET_FILE:vocem_stub_private_egl>"
    SKIP_RETURN_CODE 77)

# The tool for the person having the problem, held to printing one answer.
#
# `systemctl is-active` reports a STATE, so its exit status is that state: it
# prints "inactive" and exits 3. `$(... || echo unknown)` therefore ran both
# branches and put two lines in the report, exactly when the daemon is down --
# which is when somebody runs this. Entry 99 is the same fault with `grep -c`,
# five lines further down in the same file, found and fixed while this one stood.
# Hermetic: a stub systemctl earlier on PATH, so the test never asks the owner's
# live daemon anything and needs no skip.
add_test(NAME vocem_why
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/vocem_why.cmake")
