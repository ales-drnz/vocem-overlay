# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Every test is one of the shapes VocemTest.cmake defines. The comment above
# each says what it holds, the entry it comes from and the exemplar it fails
# against.

include(VocemTest.cmake)

# The environment the GL probes run under: the shim preloaded the way the
# session preloads it, and the heavy library taken from this tree rather than
# from the soname (VOCEM_GL_LIBRARY), so what is measured is what was built.
set(VOCEM_SHIM_ENV "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim>" "VOCEM_SHIM_PRELOADED=1")
set(VOCEM_GL_PROBE_ENV ${VOCEM_SHIM_ENV} "VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>")
set(VOCEM_EMOJI_BANK_ENV "VOCEM_EMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba")

# Once per locale. The Italian one is the case that broke in the first place; if it
# is not generated on this machine the test reports itself as skipped rather than
# passing without having run.
vocem_test(config_roundtrip NO_TEST)
vocem_test_run(config_roundtrip_c TARGET config_roundtrip ARGS C)
vocem_test_run(config_roundtrip_it TARGET config_roundtrip ARGS it_IT.UTF-8 SKIP)

# The parser on a file a person writes: quotes, capitals, words where numbers
# go, and the three switches written on top of the file as it stands
# (Config::write_switch). Header-only, like the roundtrip above.
vocem_test(config_parse)

# Two writes inside one second must both reach the overlay: the live reload
# compared whole seconds, so the second Apply of a busy second was invisible
# until the file moved again. Deterministic via utimensat, no sleeping.
vocem_test(live_config_reparse)

# Whether the palette can be read on the box it is drawn on. theme_for() is a
# pure function of the settings (common/src/theme.cpp), so this needs neither
# ImGui nor a font atlas, and holding the colours to something before they reach
# a screen is the whole point.
vocem_test(theme_contrast LINK vocem_common)

# The font atlas, built exactly as the overlay builds it but with no renderer
# attached. Needs ImGui's core, not its backends.
vocem_test(font_atlas IMGUI LINK vocem_common)

# The fonts across the death of the ImGui context they were built in (the GL
# toggle, a second Vulkan device): fonts() must not point into a freed atlas
# (entry 37).
vocem_test(fonts_lifecycle IMGUI LINK vocem_common)

# The font the settings name, drawn, with Inter merged underneath and a reason
# said when the file cannot be used. Uses this machine's fonts; skips without.
vocem_test(system_font IMGUI LINK vocem_common SKIP)

# The panel's geometry, measured out of the vertices ImGui produces rather than
# judged from a screenshot. Same sources as above plus the panel itself, which is
# the point: this runs the code the layer runs.
vocem_test(panel_geometry IMGUI LINK vocem_common)

# The daemon's Flatpak bridge, for the probes that drive it in-process: the
# class and its two halves (daemon/src/flatpak_bridge_parts.h).
set(VOCEM_FLATPAK_BRIDGE_SOURCES
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_policy.cpp"
    "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_copies.cpp")

# Not a test: a channel that is not there, for looking at the configuration window
# with a full one in it. scripts/compare-preview.py runs it against a private
# /dev/shm so it cannot be mistaken for the daemon.
vocem_test(fake_state NO_TEST
    SOURCES fake_state.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src"
    LINK rt vocem_common)

# The applications list and each process's record, on names that come out of a
# game: truncated by the kernel, with a space, a list edited by hand. A FIFO at
# the record's temporary name blocks the defective version forever: the
# timeout is how that case reports.
vocem_test(apps LINK vocem_common TIMEOUT 60)

# What the decision costs in the process that pays it, which is every OpenGL and
# Vulkan process in the session and not only the games. Allocations are asserted
# because they are the same on every machine; the timings are printed beside them.
vocem_test(apps_cost LINK vocem_common)

# The per-application decision, live: an edited list changes the answer on the
# next ask in the same process -- the property the Vulkan layer's decided-once
# static could not have while the window promised it for both paths.
vocem_test(draw_decision LINK vocem_common)

# The crash journal: left behind by a process that dies, taken along by one
# that unwinds cleanly, found by the scanner only when the process is gone.
vocem_test(journal)

# The journal's clean-exit rename through the destructor's real door: a probe
# library dlopened as the shim loads the heavy one, ended by its ELF destructor
# after static destruction (journal.cpp calls journal_end() directly and cannot
# see it).
add_library(vocem_journal_exitprobe MODULE journal_exitprobe.cpp)
target_include_directories(vocem_journal_exitprobe PRIVATE "${CMAKE_SOURCE_DIR}/include")
vocem_test(journal_exit LINK ${CMAKE_DL_LIBS}
    ENV "VOCEM_JOURNAL_EXITPROBE=$<TARGET_FILE:vocem_journal_exitprobe>" SKIP)

# The Vulkan path's handoff of a message's words: draw() records the toast,
# the post-present phase fetches its words, the toast's end forgets them. GL
# has a frame-level witness (gl_toast_alone); Vulkan has only this.
vocem_test(note_handoff INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt SKIP)

# Which swapchains get the overlay's colours re-encoded: exactly the two
# implemented HDR spaces, and nothing else -- HLG above all, whose encode is
# not implemented and must not inherit PQ's on the grounds that both are HDR.
vocem_test(hdr_mode
    SOURCES hdr_mode.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32)

# The shim's dlsym at both architectures, against the preloaded shim: the
# symbol version it resolves the real one by must be right at each (a version
# that only resolved on x86-64 shipped; entries 30/33). SKIP: the ANGLE half
# needs libEGL.so.1, and a half-run is a skip (entry 123).
vocem_test(shim_dlsym INCLUDE "${CMAKE_SOURCE_DIR}/gl/src" LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} SKIP)

# The 32-bit half, built here with -m32 so ctest covers it; without a 32-bit
# toolchain the twins report a skip (VOCEM_HAVE_M32, asked once at the top).
if(VOCEM_HAVE_M32)
    # The flags gl/CMakeLists.txt gives the shipped shim, or this twin is not the
    # shipped shim (-fno-threadsafe-statics above all).
    add_library(vocem_gl_shim32 SHARED "${CMAKE_SOURCE_DIR}/gl/src/vocem_gl_shim.cpp")
    target_include_directories(vocem_gl_shim32 PRIVATE "${CMAKE_SOURCE_DIR}/gl/src")
    target_compile_options(vocem_gl_shim32 PRIVATE
        -m32 -fno-exceptions -fno-rtti -fno-threadsafe-statics)
    target_compile_definitions(vocem_gl_shim32 PRIVATE
        VOCEM_LIBDIR="${CMAKE_INSTALL_PREFIX}/lib32")
    target_link_options(vocem_gl_shim32 PRIVATE -m32 -nostdlib++ -static-libgcc -Wl,--no-undefined)
    target_link_libraries(vocem_gl_shim32 PRIVATE ${CMAKE_DL_LIBS})
    set(VOCEM_SHIM32_ENV "LD_PRELOAD=$<TARGET_FILE:vocem_gl_shim32>" "VOCEM_SHIM_PRELOADED=1")
endif()
vocem_m32_twin(shim_dlsym ENV ${VOCEM_SHIM32_ENV})

# The drawing at 32 bits (entries 30/33/34): layout arithmetic in floats and
# ints whose widths differ, an off_t read into a size_t. Same sources, one
# flag apart.
vocem_m32_twin(panel_geometry)
vocem_m32_twin(system_font)

# Two dispatch functions with one name in one process: the Electron topology
# 0.1.0-42 froze on. The stub plays ANGLE (see the test's header).
add_library(vocem_stub_egl SHARED stub_egl.cpp)
set_target_properties(vocem_stub_egl PROPERTIES CXX_VISIBILITY_PRESET hidden)
vocem_test(shim_two_egls LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} "VOCEM_STUB_EGL=$<TARGET_FILE:vocem_stub_egl>" SKIP)

# VOCEM_DISABLE=1 switches off the interposition itself, not only the drawing --
# the escape hatch 50-vocem.conf documents has to exist (it did not, twice).
vocem_test(shim_disable INCLUDE "${CMAKE_SOURCE_DIR}/gl/src" LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} "VOCEM_DISABLE=1" SKIP)

# No file syscalls in the resolution paths, enforced by the same kind of sandbox
# that killed Discord under -41: a seccomp filter that kills on any file syscall,
# with the shim's hooks walked while nothing they look for is mapped.
vocem_test(shim_seccomp INCLUDE "${CMAKE_SOURCE_DIR}/gl/src" LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} SKIP)
vocem_m32_twin(shim_seccomp ENV ${VOCEM_SHIM32_ENV})

# One place says which symbol version dlsym has (gl/src/real_dlsym.h): a
# second copy once blanked every 32-bit game after the first was fixed.
vocem_script_test(one_dlsym_version)

# The vocem_gl_* entry points the shim resolves against the symbols the
# library exports, both ways, both widths (entry 38's failure shape). The
# 32-bit library is looked for when the test runs.
vocem_script_test(gl_entry_points ARGS
    "-DLIBRARY=$<TARGET_FILE:vocem_gl>"
    "-DLIBRARY32=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# The RPC probe's own client across a pause at a frame boundary and one inside
# a frame: both frames, in order, and no close before the peer's (DESIGN 206).
vocem_script_test(rpc_probe_frames)

# Every package a test was refuted against is on the list a clean-up checks
# before deleting anything (packaging/EXEMPLARS.md, DESIGN 205).
vocem_script_test(exemplars_listed)

# The layer's exports (entry 16): the three names the loader asks for and
# nothing else, both widths.
vocem_script_test(layer_exports ARGS
    "-DLIBRARY=$<TARGET_FILE:vocem_vk>"
    "-DLIBRARY32=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# A crash is reported in the Debug section: the dead process's journal appears
# there, and no separate crash window exists (entry 60).
if(TARGET vocem-config)
    set(VOCEM_WINDOW_ARG "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>")
    vocem_script_test(debug_section ARGS ${VOCEM_WINDOW_ARG} "-DQML_DIR=${CMAKE_SOURCE_DIR}/gui/qml")

    # The Debug section reads journals and nothing else: a symlink in the journal
    # directory is refused by the scanner (lstat) and the reader (canonical path,
    # size cap).
    vocem_script_test(journal_paths ARGS ${VOCEM_WINDOW_ARG})

    # "Drawing now" follows the world while the window is open, not the list
    # computed at construction.
    vocem_script_test(live_while_open ARGS ${VOCEM_WINDOW_ARG})

    # The message preview shows a body only when notification_show_body says the
    # daemon will publish one.
    vocem_script_test(toast_preview_honest ARGS ${VOCEM_WINDOW_ARG})

    # A switch that empties the overlay dims the window's picture of it: the Panel
    # map and the live preview follow panelEnabled (the dump's `opacity`).
    vocem_script_test(preview_switch_honest ARGS ${VOCEM_WINDOW_ARG})

    # The preview quiets whoever is not talking from the overlay's own setting (an
    # opacity the dump prints). Fails against 0.1.9's window.
    vocem_script_test(preview_idle_picture ARGS ${VOCEM_WINDOW_ARG})
endif()

# One place turns pixels back into a position fraction (fraction_within):
# the wrong inverse survived twice in QML handlers. A lint; no harness drag
# exists to measure it.
vocem_script_test(one_placement_inverse)

# The emoji bank is a Modified Version of Noto Color Emoji under the OFL 1.1
# and shipped through 0.1.0-52's sources with no licence text anywhere. Held on
# a scratch install: the licence lands beside the bank.
vocem_script_test(emoji_bank_licence ARGS "-DBUILD_DIR=${CMAKE_BINARY_DIR}")

# The project's other one-place facts: the monotonic clock (vocem/clock.h),
# mkdir -p (vocem/paths.h), the curl write callback (curl_sink.h). A lint.
vocem_script_test(one_spelling)

# PKGBUILD and PKGBUILD.local build one package: build(), package(), depends
# and makedepends agree modulo their legitimate differences (entry 76).
vocem_script_test(pkgbuild_agree)

# The Flatpak extension's post-install list carries what the install rules
# guarded by VOCEM_BUILD_HOST_TOOLS would: the emoji sequence table and every
# font's and ImGui's licence (0.1.9's extension shipped without them). Derived
# from the tree.
vocem_script_test(flatpak_extension_payload)

# The shim's two ways into a process (vocem-run and environment.d) name one
# path (entry 16). Configured at a prefix of its own: at /usr the two agree by
# accident. Nothing is built.
vocem_script_test(one_preload_path TIMEOUT 300)

# Nothing in the repository root is neither tracked nor ignored, empty
# directories included, which git status cannot see (entry 137).
vocem_script_test(repo_root_clean)

# The daemon's unit is confined, and in ways that start: an empty
# CapabilityBoundingSet fails a user unit (exit 218, measured).
vocem_script_test(unit_hardening)

# Anything that turns a 64-bit value into text gives the same text at 32 bits
# (both widths talk to one 64-bit daemon). Its twin is further down.
vocem_test(widths)

# The DRM reader that decides how large the overlay is, against a /sys tree
# built to order rather than whatever the build machine has plugged in.
vocem_test(display_height)

# The CLI's --watch across a daemon restart: it showed the dead daemon's last
# state for ever. Private /dev/shm; skips without bwrap.
if(TARGET vocem)
    vocem_test(cli_watch INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV "VOCEM_CLI=$<TARGET_FILE:vocem>" TIMEOUT 90 SKIP)
endif()

# A reader that survived the daemon must be able to find the next daemon:
# unlink removes the name and not the pages, and quit-and-reopen has to work
# for the game being played, not only for the next one. Two writer lives, one
# process, a private /dev/shm.
vocem_test(shm_reattach LINK rt SKIP)

# A shared segment shorter than the struct maps fine and kills the reader on the
# first touch. The writer creates the name and sizes it in two calls, so a daemon
# that dies between them leaves one behind -- and every game in the session then
# takes SIGBUS. The read runs in a forked child so the signal is a result.
vocem_test(shm_short_segment LINK rt SKIP)

# The overlay draws for a game whose GL arrives by dlopen(RTLD_LOCAL)
# (Minecraft, GLFW, LWJGL, SDL). The library in 0.1.0-44 drew nothing there
# while logging that it drew (entry 38). Needs a display and bwrap.
# -rdynamic: the probe interposes glXMakeCurrent for the overlay's
# RTLD_DEFAULT lookups, and only an exported definition is found.
vocem_test(gl_draw_local LINK ${CMAKE_DL_LIBS} X11 rt LINK_OPTIONS -rdynamic
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# A helper context destroyed beside the drawing one: the 0.1.7 library tore
# its backend down for any context's death.
vocem_test_run(gl_second_context TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=second-context" SKIP)

# Six arrivals with a fresh colour emoji, then a message (entry 191): "font
# atlas built" once, "folded" once per arrival. 0.1.10-5's library reads 8
# builds. The longest swap is printed, not asserted.
vocem_test_run(gl_arrivals TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=arrivals" SKIP)

# A second window with an unshared context presents: nothing drawn there, no
# GL error left, its textures untouched, the first window drawing after.
vocem_test_run(gl_two_windows TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=two-windows" SKIP)

# The game gone while the first atlas rasterises on its worker (entry 192):
# release() and the destructor wait for it. A guard.
vocem_test_run(gl_early_exit TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=early-exit" SKIP)

# A game that destroys its context and makes another: 0.1.10-1 crashed on the
# second and rasterised the atlas again per context (entries 144-146). Four EGL
# contexts, no window; the log's "font atlas built" lines are the count.
vocem_test(gl_context_cycle LINK ${CMAKE_DL_LIBS} EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP TIMEOUT 120)

# Quit means quit inside the game: with the daemon's segment unlinked the
# overlay frees its atlas, GL objects and faces (139.7 MB against 22.3 MB
# before). The probe reads its own Pss on both sides.
vocem_test(gl_daemon_gone LINK ${CMAKE_DL_LIBS} EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP TIMEOUT 120)

# The same in a process the overlay declines (plasmashell's shape): no
# backend, no atlas, one verdict however many contexts.
# The system's libGL opened under a file name the shim does not know (the
# unversioned development link, a renamed library): judged by its SONAME, it
# draws. 0.1.12-2's shim judged the file name: 0 pixels and not one line, even
# under VOCEM_DEBUG. Both widths.
vocem_test_run(gl_soname TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=soname" SKIP)
vocem_m32_twin(gl_draw_local
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_m32_twin_run(gl_soname TARGET gl_soname
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=soname")

vocem_test_run(gl_context_cycle_quiet TARGET gl_context_cycle
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=declining" SKIP TIMEOUT 120)

# Both at 32 bits against build32's libraries (entries 30/33/34, 144-146).
vocem_m32_twin(gl_context_cycle
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_m32_twin(gl_daemon_gone
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# The overlay's colours under GL_FRAMEBUFFER_SRGB: the hardware must not
# encode ImGui's already-encoded colours again (the panel came out lifted).
# hdr_srgb_pixels is the Vulkan twin.
vocem_test(gl_srgb_write LINK ${CMAKE_DL_LIBS} X11 rt vocem_common
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# A process the overlay declines is quiet on the X wire: no glXQueryDrawable
# round trips per frame. X request serials, disabled child against declining
# one; 0.1.0-50's library shows +3 per frame. Needs a display and real GLX.
vocem_test(gl_noop_quiet LINK GL X11 ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# A toast with nobody in a voice channel reaches the screen: the frame guard
# asked only about the channel. Fails against 0.1.0-50's library.
vocem_test(gl_toast_alone LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# Colour emoji to a real framebuffer. Differential (bank reachable, bank
# absent), not a colour detector: the speaking ring is green and the muted
# badge red. VOCEM_GL_LIBRARY at a packaged file measures what ships.
vocem_test(gl_emoji_colour LINK ${CMAKE_DL_LIBS} X11 rt
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} SKIP)

# What a drawn frame costs the filesystem on the avatar path (entry 49): file
# syscalls counted by interposition across no picture yet, arrival, and
# settled.
vocem_test(gl_avatar_quiet LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# The upload reads through the game's GL_UNPACK_* state and pixel-unpack
# buffer: a miniature game leaves hostile ones bound and asks GL afterwards
# what it finds. Fails against 0.1.0-67's library.
vocem_test(gl_unpack_state INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK ${CMAKE_DL_LIBS} X11 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# The segment's layout at both widths, crossed: each width writes and the
# other reads (entry 55: `users` began at 96 in the daemon and 92 at i386).
vocem_test(shared_state_layout)
vocem_m32_twin(shared_state_layout)
if(VOCEM_HAVE_M32)
    # Each reader REQUIRES its writer as a fixture, so it reads the file this run
    # wrote (DEPENDS only orders tests that are both selected).
    vocem_test_run(shared_state_cross_write64 TARGET shared_state_layout
        ARGS write "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin" SETUP segment64)
    vocem_test_run(shared_state_cross_read32 TARGET shared_state_layout32
        ARGS read "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin" REQUIRES segment64 SKIP)
    vocem_test_run(shared_state_cross_write32 TARGET shared_state_layout32
        ARGS write "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin" SETUP segment32)
    vocem_test_run(shared_state_cross_read64 TARGET shared_state_layout
        ARGS read "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin" REQUIRES segment32 SKIP)
endif()

# A face on screen through the GL path at both widths, each against its own
# width's libraries (entries 30/33/34; Isaac runs 32-bit GL). build32's are
# looked for when the test runs.
vocem_test(gl_avatar_width LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)
vocem_m32_twin(gl_avatar_width
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# Beside Steam's overlay, in both orders, witnessed by our own log line: we
# draw either way, so Steam forwards down the chain. Whether Steam's overlay
# drew stays unmeasured (a probe is not a Steam title).
set(VOCEM_GL_PROBE_ARGS
    "-DPROBE=$<TARGET_FILE:vocem_gl_draw_local>"
    "-DSHIM=$<TARGET_FILE:vocem_gl_shim>"
    "-DLIBRARY=$<TARGET_FILE:vocem_gl>")
find_file(VOCEM_STEAM_OVERLAY gameoverlayrenderer.so
    PATHS "$ENV{HOME}/.steam/root/ubuntu12_64" "$ENV{HOME}/.local/share/Steam/ubuntu12_64"
    NO_DEFAULT_PATH)
if(VOCEM_STEAM_OVERLAY)
    vocem_script_test(gl_beside_steam ARGS ${VOCEM_GL_PROBE_ARGS}
        "-DSTEAM_OVERLAY=${VOCEM_STEAM_OVERLAY}")
else()
    vocem_skip(gl_beside_steam "Steam's gameoverlayrenderer.so is not installed here")
endif()

# The OpenGL half inside headless gamescope: gamescope strips Steam's library
# from LD_PRELOAD, not the variable, and the shim still draws. Its WSI layer
# sits where our present hook does.
find_program(VOCEM_GAMESCOPE gamescope)
if(VOCEM_GAMESCOPE)
    # LOCK gamescope: two nested headless sessions at once hung one to its 180 s
    # cap in a -j16 run (DESIGN 193).
    vocem_script_test(gl_inside_gamescope LOCK gamescope ARGS ${VOCEM_GL_PROBE_ARGS}
        "-DGAMESCOPE=${VOCEM_GAMESCOPE}")
else()
    vocem_skip(gl_inside_gamescope "gamescope is not installed here")
endif()

# The layer manifests' shared name and bare soname hold each other up: the
# loader keeps one manifest and the soname serves both widths. Changing either
# alone kills 32-bit games silently (entries 30/33/34; the script says why).
# build32's manifest is looked for when the test runs.
vocem_script_test(layer_manifest_name ARGS
    "-DMANIFEST=${CMAKE_BINARY_DIR}/layer/install/VkLayer_vocem_overlay.json"
    "-DMANIFEST32=${VOCEM_BUILD32_DIR}/layer/install/VkLayer_vocem_overlay.x86.json"
    "-DLAYER_CMAKE=${CMAKE_SOURCE_DIR}/layer/CMakeLists.txt")

# The same pair inside Steam's container, one instance per width: the witness
# is the path mapped, not the layer name. Measures the installed layer; skips
# without the sniper runtime or an installation.
vocem_test(vk_layer_loads NO_TEST LINK ${CMAKE_DL_LIBS})
vocem_m32_twin(vk_layer_loads)
set(VOCEM_VK_LAYER_LOADS32 "")
if(TARGET vocem_vk_layer_loads32)
    set(VOCEM_VK_LAYER_LOADS32 "$<TARGET_FILE:vocem_vk_layer_loads32>")
endif()
find_program(VOCEM_SNIPER run
    PATHS "$ENV{HOME}/.steam/root/steamapps/common/SteamLinuxRuntime_sniper"
          "$ENV{HOME}/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"
    NO_DEFAULT_PATH)
vocem_script_test(vk_layer_in_container ARGS
    "-DPROBE64=$<TARGET_FILE:vocem_vk_layer_loads>"
    "-DPROBE32=${VOCEM_VK_LAYER_LOADS32}"
    "-DSNIPER=${VOCEM_SNIPER}")

# The avatar upload's command buffer is registered with
# pfnSetDeviceLoaderData (layer rule 5). The real TextureCache against a stub
# Vulkan through its resolver seam; vocem_common for its logger only.
set(VOCEM_TEXTURE_CACHE_INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
vocem_test(texture_loader_data
    SOURCES texture_loader_data.cpp "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
    INCLUDE ${VOCEM_TEXTURE_CACHE_INCLUDE} DEFINES IMGUI_USE_WCHAR32 LINK vocem_common)

# The avatar cache stops at its descriptor budget and hands out placeholders
# (AddTexture on an exhausted pool crashed Minecraft, 2026-07-29).
vocem_test(texture_descriptor_budget
    SOURCES texture_descriptor_budget.cpp "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
    INCLUDE ${VOCEM_TEXTURE_CACHE_INCLUDE} DEFINES IMGUI_USE_WCHAR32 LINK vocem_common)

# The font texture's copies against a GPU that is behind (DESIGN 194): a fold
# does not wait for the copies before it, and a whole replacement that fails
# leaves the descriptor ImGui draws with alive. The same stub device.
vocem_test(texture_font_copies
    SOURCES texture_font_copies.cpp "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
    INCLUDE ${VOCEM_TEXTURE_CACHE_INCLUDE} DEFINES IMGUI_USE_WCHAR32 LINK vocem_common)

# The colour emoji bank's reader, at both widths: written by a 64-bit script,
# read inside games of both, which is the exact shape of entries 30/33/34.
vocem_test(emoji_bank ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)
vocem_m32_twin(emoji_bank)

# A colour emoji seen in a name reaches the atlas in colour, in both weights,
# and a new one arriving later reaches it too. Fails against the atlas as it
# stood: the glyph existed, white.
vocem_test(emoji_atlas IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)

# People with no avatar share their default picture's cache key (entries 184,
# 192): 24 keys for one file became at most six. Fails against the old key.
vocem_test(avatar_key_default)
vocem_m32_twin(avatar_key_default)

# ...without rasterising the atlas again (the join freeze). A count, not a
# clock (entry 145); against the old module every emoji is another build.
vocem_test(emoji_incremental IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)

# The decoration symbols servers name channels with have glyphs, one pinned
# per added block, both weights.
vocem_test(symbol_coverage IMGUI LINK vocem_common)

# The frame path's silence under a seccomp filter: fonts_note_emoji runs
# inside the present, so every codepoint costs its bank lookup once, misses
# included.
vocem_test(fonts_frame_quiet IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)

# The daemon's image decode, the trust boundary: stb's allocator counted,
# because both defects are about cost (a decompression bomb inflated before
# the size check, a migration read following a link to /dev/zero). The
# vendored decoder's unused helpers are silenced as in daemon/.
find_package(ZLIB REQUIRED)
set(VOCEM_DAEMON_INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" "${CMAKE_SOURCE_DIR}/third_party")
vocem_test(avatar_decode INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function LINK ZLIB::ZLIB SKIP)

# The avatar cache stops remembering requests at its ceiling (the unit's
# MemoryMax=128M). The real avatars.cpp, its worker stopped before the flood,
# no network.
find_package(CURL REQUIRED)
vocem_test(avatar_bound
    SOURCES avatar_bound.cpp "${CMAKE_SOURCE_DIR}/daemon/src/avatars.cpp"
    INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function
    DEFINES VOCEM_VERSION="${PROJECT_VERSION}"
    LINK CURL::libcurl ZLIB::ZLIB Threads::Threads)

# The faces do not change with the decoder: Wuffs held to the picture and to
# stb_image (used up to 0.1.0-53) byte for byte across six colour types.
vocem_test(avatar_decoders_agree INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function LINK ZLIB::ZLIB SKIP)

# The avatar cache format from both ends: the daemon's writer and the games'
# reader, which takes exactly one size or nothing. Both widths.
vocem_test(avatar_rgba)
vocem_m32_twin(avatar_rgba)

# What a message becomes in the segment: the real vocemd driven by a stub
# Discord RPC, read back as a game reads it. The body never reaches the state
# segment (entry 64), only the note segment, and is gone after the toast.
# Fails against 0.1.0-43's daemon. bwrap: private /dev/shm and network.
if(TARGET vocemd)
    set(VOCEM_DAEMON_ENV "VOCEM_DAEMON=$<TARGET_FILE:vocemd>")
    vocem_test(daemon_notification LINK rt ENV ${VOCEM_DAEMON_ENV} SKIP)

    # A peer that takes the connection and drops it is backed off, not hammered:
    # 0.1.3-7's daemon made 2215 connections in five seconds against 3 (entry
    # 103). Same sandbox.
    vocem_test(daemon_reconnect ENV ${VOCEM_DAEMON_ENV} TIMEOUT 120 SKIP)

    # The same peer after authenticating: the installed 0.1.10-2 daemon made
    # 5166 and 4381 connections in five seconds, the pause makes 6. Discord
    # restarting is this shape.
    vocem_test_run(daemon_reconnect_authenticated TARGET daemon_reconnect
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_RECONNECT_SCENARIO=authenticated"
        TIMEOUT 120 SKIP)
endif()

# What the daemon takes out of a name (daemon/src/text.h): directional marks
# (entry 66) and control characters (entry 133).
vocem_test(daemon_text INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src")

# The token file read as the avatar cache reads (entry 47) and written through
# a temporary: a link to /dev/zero, a FIFO, a byte that is not UTF-8 (entry
# 133). Compiles auth.cpp; its exchange is never called.
if(TARGET vocemd)
    vocem_test(token_file
        SOURCES token_file.cpp "${CMAKE_SOURCE_DIR}/daemon/src/auth.cpp"
        INCLUDE ${VOCEM_DAEMON_INCLUDE}
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}"
        LINK CURL::libcurl TIMEOUT 60)

    # How long the segment stays published after SIGTERM with a download stalled
    # behind a quiet https_proxy (entry 134). Seconds printed;
    # daemon_notification's sandbox.
    vocem_test(daemon_stop_unlinks INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 150 SKIP)

    # The stop during a handshake whose peer never answers the upgrade: the
    # daemon waited out its ten-second deadline with the flag set (entries 72,
    # 77, 102, 112). daemon_ws_bounds's stub answers, so it cannot reach this.
    vocem_test_run(daemon_stop_in_handshake TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=handshake" TIMEOUT 150 SKIP)
    # A handshake answered a byte every tenth of a second: the stop flag must be
    # read between bytes (entry 196).
    vocem_test_run(daemon_stop_in_trickle TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=trickle" TIMEOUT 150 SKIP)

    # A clean stop leaves no words in a Flatpak sandbox: the mirrors were emptied
    # before the note's clear fired the hook that unlinks them (vocem/note.h).
    vocem_test_run(daemon_stop_note_mirror TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=note-mirror" TIMEOUT 150 SKIP)

    # The stop with the token exchange stalled: the flag is consulted from the
    # transfer, as the avatar worker's is (entry 134). 0.1.8-1's daemon waited
    # 10 s. Seconds printed.
    vocem_test(daemon_exchange_stop INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 150 SKIP)
endif()

# Who the daemon believes is on the other end before it sends the token: this
# process's own connection, a dual-stack listener in /proc/net/tcp6, and a row
# owned by somebody else.
vocem_test(peer_identity INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src")

# The daemon's WebSocket client against a hostile peer, compiled directly and
# driven from a stub on an ephemeral port: no daemon, no /dev/shm, no 6463.
vocem_test(daemon_ws_bounds
    SOURCES daemon_ws_bounds.cpp "${CMAKE_SOURCE_DIR}/daemon/src/websocket.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" TIMEOUT 120)

vocem_m32_twin(widths)

# The HDR pipelines on the GPU: the shipped SPIR-V through the real
# hdr_pipeline_create, known sRGB colours drawn to an fp16 target, pixels read
# back against the standard's constants. Skips without a Vulkan device.
vocem_test(hdr_pixels
    SOURCES hdr_pixels.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32 LINK ${CMAKE_DL_LIBS} SKIP)

# The same source against an sRGB target: the stored byte is the one ImGui
# asked for (before mode 3: 128 -> 188).
vocem_test(hdr_srgb_pixels
    SOURCES hdr_pixels.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32 VOCEM_SRGB_TARGET LINK ${CMAKE_DL_LIBS} SKIP)

# The Vulkan present hook on a real swapchain (entries 70, 88, 114): 45 frames
# to an off-screen override-redirect window, one read back, foreign pixels
# required. VOCEM_VK_MANIFEST/_LIBRARY name the build tree's layer; the probe
# reads /proc/self/maps rather than trusting the loader. Skips without a
# display, a loader or a presentable device.
set(VOCEM_VK_MANIFEST_ENV "VOCEM_VK_MANIFEST=${CMAKE_BINARY_DIR}/dev/VkLayer_vocem_overlay.json")
set(VOCEM_VK_PROBE_ENV ${VOCEM_VK_MANIFEST_ENV} "VOCEM_VK_LIBRARY=$<TARGET_FILE:vocem_vk>")
vocem_test(vk_present_draw SOURCES vk_present_draw.cpp vk_file_witness.cpp
    LINK ${CMAKE_DL_LIBS} X11 rt
    LINK_OPTIONS "-Wl,--export-dynamic-symbol=open,--export-dynamic-symbol=open64"
                 "-Wl,--export-dynamic-symbol=openat,--export-dynamic-symbol=openat64"
                 "-Wl,--export-dynamic-symbol=pread,--export-dynamic-symbol=pread64"
                 "-Wl,--export-dynamic-symbol=close"
    ENV ${VOCEM_VK_PROBE_ENV} SKIP)

# Its control: VOCEM_DISABLE=1 and the loader refuses the layer, so the frame
# is exactly the probe's (entry 35 on the Vulkan side). Run on its own, not as
# a child: a forked control inherits the published segment and private_shm.h
# refuses it.
vocem_test_run(vk_present_disabled TARGET vk_present_draw
    ENV ${VOCEM_VK_MANIFEST_ENV} "VOCEM_DISABLE=1" SKIP)

# The witness layer under the overlay (vk_witness_layer.cpp): it records what
# pixels cannot see -- a queue wait inside a present, a signalled fence
# submitted, the stock pipeline in a foreign render pass (entry 131). Its own
# positive control says whether it sat below.
add_library(vocem_vk_witness SHARED vk_witness_layer.cpp)
set_target_properties(vocem_vk_witness PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(vocem_vk_witness PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(vocem_vk_witness PRIVATE VK_NO_PROTOTYPES)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vk_witness.json"
    CONTENT "{
    \"file_format_version\": \"1.2.1\",
    \"layer\": {
        \"name\": \"VK_LAYER_VOCEM_witness\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"$<TARGET_FILE:vocem_vk_witness>\",
        \"api_version\": \"${VOCEM_LAYER_API_VERSION}\",
        \"implementation_version\": \"1\",
        \"description\": \"Vocem Overlay test witness: records what the layer above asks of the chain\",
        \"enable_environment\": { \"VOCEM_WITNESS\": \"1\" },
        \"disable_environment\": { \"VOCEM_WITNESS_OFF\": \"1\" }
    }
}
"
)
set(VOCEM_VK_WITNESSED_ENV ${VOCEM_VK_PROBE_ENV}
    "VOCEM_VK_WITNESS_MANIFEST=${CMAKE_CURRENT_BINARY_DIR}/vk_witness.json")
vocem_test_run(vk_present_witnessed TARGET vk_present_draw ENV ${VOCEM_VK_WITNESSED_ENV} SKIP)

# The swapchain recreated in another format, both ways: a pipeline per render
# pass, which only the witness can tell apart.
vocem_test_run(vk_present_recreate TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=recreate" SKIP)
vocem_test_run(vk_present_recreate_from_srgb TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=recreate" "VOCEM_VK_FORMAT_FIRST=srgb" SKIP)

# Frames chained through semaphores, two in flight, as a game presents: the
# layer's per-image fence wait and its semaphore substitution.
vocem_test_run(vk_present_in_flight TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=in-flight" SKIP)

# A helper device made and destroyed beside the presenting one: "backend
# ready" once, not rebuilt.
vocem_test_run(vk_present_second_device TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=second-device" SKIP)

# Quit means quit on the Vulkan side: with the segment unlinked the layer
# releases everything, counted by its own line, and the frame is the game's.
# Witnessed and chained: a waitIdle after every present made any release legal
# (DESIGN 62), so only the layer's own wait can make it safe here.
vocem_test_run(vk_present_daemon_gone TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=daemon-gone" SKIP TIMEOUT 180)

# A daemon publishing, nobody in a channel: "backend ready" must not appear
# (the renderer used to be built for a panel never shown).
vocem_test_run(vk_present_idle TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=idle" SKIP)

# Six arrivals with a fresh colour emoji, then a message (entry 191): "font
# atlas rebuilt" never, "folded" once per arrival. 0.1.10-5's library reads
# seven rebuilds. The longest present is printed, not asserted.
vocem_test_run(vk_present_arrivals TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=arrivals" SKIP)

# Device, instance and process gone while the first atlas rasterises (entry
# 192): vocem_DestroyDevice and the destructor wait. A guard.
vocem_test_run(vk_present_early_exit TARGET vk_present_draw
    ENV ${VOCEM_VK_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=early-exit" SKIP)

# A Flatpak game with the master switch off (entry 138): the request file
# reads drawing=0 (0.1.8's layer said drawing=1), and zero foreign pixels.
vocem_test_run(vk_present_flatpak_off TARGET vk_present_draw
    ENV ${VOCEM_VK_PROBE_ENV} "VOCEM_VK_SCENARIO=flatpak-off" SKIP)

# The frame at 32 bits against build32 (entries 30/33/34), whose manifest
# and library are looked for when the test runs.
set(VOCEM_VK_PROBE32_ENV
    "VOCEM_VK_MANIFEST=${VOCEM_BUILD32_DIR}/dev/VkLayer_vocem_overlay.x86.json"
    "VOCEM_VK_LIBRARY=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")
vocem_m32_twin(vk_present_draw
    ENV ${VOCEM_VK_PROBE32_ENV}
    BUILD32 "${VOCEM_BUILD32_DIR}/dev/VkLayer_vocem_overlay.x86.json"
            "${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# The scenarios at 32 bits, on the plain chain (the witness is a 64-bit
# layer), with the same BUILD32 files so a missing build32 is a skip.
foreach(scene recreate in_flight second_device daemon_gone idle)
    string(REPLACE "_" "-" scene_value "${scene}")
    vocem_m32_twin_run(vk_present_${scene} TARGET vk_present_${scene}
        ENV ${VOCEM_VK_PROBE32_ENV} "VOCEM_VK_SCENARIO=${scene_value}")
endforeach()
# The arrivals at 32 bits too: the fold writes into atlas pixels by offset and
# registers glyphs by hand, which is exactly the arithmetic a width can break.
vocem_m32_twin_run(vk_present_arrivals TARGET vk_present_arrivals
    ENV ${VOCEM_VK_PROBE32_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=arrivals")
vocem_m32_twin_run(vk_present_early_exit TARGET vk_present_early_exit
    ENV ${VOCEM_VK_PROBE32_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=early-exit")

# Under the Khronos validation layer, below the overlay (entry 192). One test
# per scene, run side by side; vk_present_validated is the default loop and
# the check that the chain order holds. Skips without the layer.
set(VOCEM_VK_VALIDATED_ARGS
    "-DPROBE=$<TARGET_FILE:vocem_vk_present_draw>"
    "-DMANIFEST=${CMAKE_BINARY_DIR}/dev/VkLayer_vocem_overlay.json"
    "-DLIBRARY=$<TARGET_FILE:vocem_vk>"
    "-DWITNESS=${CMAKE_CURRENT_BINARY_DIR}/vk_witness.json"
    "-DEMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba")
vocem_script_test(vk_present_validated ARGS ${VOCEM_VK_VALIDATED_ARGS}
    "-DSCENES=default" "-DORDER=ON")
foreach(scene recreate second-device in-flight daemon-gone idle arrivals deferred
              second-presenter second-queue no-texture-cache alternate-queue)
    string(REPLACE "-" "_" scene_id "${scene}")
    vocem_script_test(vk_present_validated_${scene_id} SCRIPT vk_present_validated.cmake
        ARGS ${VOCEM_VK_VALIDATED_ARGS} "-DSCENES=${scene}")
endforeach()

# The Vulkan half inside gamescope with gamescope's WSI layer in the chain,
# which replaces swapchain creation and present (the script says why the plain
# chain would measure nothing).
if(VOCEM_GAMESCOPE)
    find_file(VOCEM_GAMESCOPE_WSI
        NAMES VkLayer_FROG_gamescope_wsi.x86_64.json VkLayer_FROG_gamescope_wsi.json
        PATHS "/usr/share/vulkan/implicit_layer.d" NO_DEFAULT_PATH)
    vocem_script_test(vk_inside_gamescope LOCK gamescope ARGS
        "-DPROBE=$<TARGET_FILE:vocem_vk_present_draw>"
        "-DMANIFEST=${CMAKE_BINARY_DIR}/dev/VkLayer_vocem_overlay.json"
        "-DLIBRARY=$<TARGET_FILE:vocem_vk>"
        "-DGAMESCOPE=${VOCEM_GAMESCOPE}"
        "-DWSI_MANIFEST=${VOCEM_GAMESCOPE_WSI}")
else()
    vocem_skip(vk_inside_gamescope "gamescope is not installed here")
endif()

# The Appearance presets are four real panels from each preset's theme, and
# Appearance and Spacing carry a live preview. From the geometry dump.
if(TARGET vocem-config)
    vocem_script_test(appearance_previews ARGS ${VOCEM_WINDOW_ARG})
endif()

# Which height sizes the atlas, one spelling for both paths: the display wins
# (entry 39), except a drawable taller than every display (supersampling),
# which the old ternary sized at 0.5x.
vocem_test(sizing_height INCLUDE "${IMGUI_DIR}" DEFINES IMGUI_USE_WCHAR32)

# The tray's five state icons, installed into a scratch DESTDIR and drawn by
# Qt's SVG renderer (tray_icon_pictures.cpp): one circle each, one radius,
# token-true, nothing behind. A missing or broken state icon degrades to the
# generic one with nothing logged. The names Tray.qml asks for stay a reading.
vocem_script_test(tray_icons ARGS "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DPROBE=$<$<TARGET_EXISTS:vocem_tray_icon_pictures>:$<TARGET_FILE:vocem_tray_icon_pictures>>")

if(TARGET vocem-config)
    # A game running right now with the overlay off is said above the history:
    # the Applications page's answer to the question somebody opens it with
    # mid-session, held on the real window against a record for pid 1's name.
    vocem_script_test(running_without ARGS ${VOCEM_WINDOW_ARG})

    # A map of a display the overlay is not sized for shows the boxes at that
    # screen's share, and each map's dropdown picks the display. Fabricated DRM
    # tree; fails against 0.1.0-59's window (ratio 1).
    vocem_script_test(preview_display ARGS ${VOCEM_WINDOW_ARG})

    # One padding in every card on every page, at the default size and the
    # minimum: a card taller than its "cardContent" blocks holds an unpadded item.
    # Against 0.1.0-64's window it measures no cards.
    vocem_script_test(window_padding ARGS ${VOCEM_WINDOW_ARG})
endif()

if(TARGET vocem-config)
    # The window's first frame does not wait on systemctl. Differential, against
    # a stub systemctl that sleeps.
    vocem_script_test(window_startup ARGS ${VOCEM_WINDOW_ARG})

    # What an idle window costs, in counts: stateChanged once, not once per
    # tick; the segment and the display tree asked on the sweep; an icon lookup
    # examining the entries that carry its name and not the whole registry.
    vocem_script_test(window_cost ARGS ${VOCEM_WINDOW_ARG})

    # No Qt_6_PRIVATE_API symbol: the next Qt minor must not stop the window
    # from starting. Fails against 0.1.12-2's window (two AOT references).
    vocem_script_test(window_private_api ARGS ${VOCEM_WINDOW_ARG})

    # The three instant switches, driven where no window test reaches: the
    # bridge built directly (the harness cannot press a Switch). After a switch
    # the window's copy follows an edit made outside, so Apply keeps it.
    # Gui, Qml and fontconfig are what config_bridge.cpp reaches; found here
    # because gui/'s find_package does not reach tests/.
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml)
    find_package(Fontconfig QUIET)
    vocem_test(config_bridge_switches
        SOURCES config_bridge_switches.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_config_bridge_switches PROPERTIES AUTOMOC ON)

    # Two launches 100 ms apart are one window: a lock taken before the engine
    # loads decides it.
    vocem_script_test(single_instance ARGS ${VOCEM_WINDOW_ARG})
endif()

# An empty view says so with a placeholder, on every page with one. Fails on
# section 5 against 0.1.0-64.
if(TARGET vocem-config)
    vocem_script_test(empty_views ARGS ${VOCEM_WINDOW_ARG})
endif()

# Where you are is asked, not remembered, and names arrive without Discord's
# U+2068/U+2069 isolates (the owner's report from 0.1.0-65). The real vocemd
# against a stub, sandboxed like daemon_notification.
if(TARGET vocemd)
    vocem_test(daemon_moved LINK rt ENV ${VOCEM_DAEMON_ENV} SKIP)
endif()

# The System tray section: the panel's picture, and a preview holding six
# pictures that actually loaded (the artwork is carried in the binary).
if(TARGET vocem-config)
    vocem_script_test(tray_icon_choice ARGS ${VOCEM_WINDOW_ARG})
endif()

# The Flatpak bridge: the daemon's writer against the overlay's reader
# across a fake $XDG_RUNTIME_DIR/app; the child reads state, settings and
# avatar after the POSIX segment is unlinked. Private /dev/shm.
vocem_test(flatpak_bridge
    SOURCES flatpak_bridge.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK rt vocem_common SKIP)

# The same directory as a hostile sandbox makes it: every name the daemon
# opens there is a symbolic link.
vocem_test(flatpak_bridge_hostile
    SOURCES flatpak_bridge_hostile.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common SKIP)

# One package, one version: CMakeLists.txt, both PKGBUILDs (each to its own
# rule), .SRCINFO, metainfo, CHANGELOG, tags, and the built window's About
# page. The installed 0.1.4-1 gave three answers.
vocem_script_test(version_agrees ARGS
    "-DCONFIG_BINARY=$<IF:$<TARGET_EXISTS:vocem-config>,$<TARGET_FILE:vocem-config>,>")

# The note's words are retired on the daemon's clock: Discord gone, the text
# stays no longer than its toast (0.1.4-1's daemon: still there after 40 s).
# And a SIGKILLed daemon's note is not inherited by the next one.
if(TARGET vocemd)
    vocem_test(daemon_note_expiry LINK rt ENV ${VOCEM_DAEMON_ENV} TIMEOUT 240 SKIP)

    # With a slow port walk (three silent ports, thirty seconds): the tick runs
    # once per port (entry 112). 11.0 s against 30.0/30.1 s for the installed
    # 0.1.10-2 daemon.
    vocem_test_run(daemon_note_expiry_port_walk TARGET daemon_note_expiry
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_NOTE_EXPIRY_SCENARIO=port-walk" TIMEOUT 120 SKIP)
endif()

# The display map follows the machine while the window is open: a connector
# planted 1.6 s in reshapes both maps. Against 0.1.4-1's window every section
# stays 5:4.
if(TARGET vocem-config)
    vocem_script_test(display_change_while_open ARGS ${VOCEM_WINDOW_ARG})
endif()

# A settings value as long as the user's list: lines past 255 bytes arrived
# in pieces and the window wrote the truncation back. 0.1.4's header: 330
# bytes in, 241 out.
vocem_test(config_long_line)

# What the shim's dispatch hooks hand back when the real dispatcher is a
# private one opened RTLD_GLOBAL: dlsym and the dispatcher answer differently
# (entry 36). A measurement, not a fix: the symmetric change is entry 35's
# defect (see the test's header).
add_library(vocem_stub_private_egl SHARED stub_private_egl.cpp)
set_target_properties(vocem_stub_private_egl PROPERTIES CXX_VISIBILITY_PRESET hidden)
vocem_test(shim_private_dispatch LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} "VOCEM_STUB_PRIVATE_EGL=$<TARGET_FILE:vocem_stub_private_egl>" SKIP)

# The shim and another OpenGL interposer preloaded after it (MangoHud's order
# under its wrapper): each gets every frame, once, and the frame still reaches
# the system's library. Three shapes of interposer, three doors, both widths;
# what the shim handed over is counted by a stub overlay library, so nothing
# needs a display. The other order is asserted as measured (shim_chain.cpp).
function(vocem_chain_stubs suffix)
    set(options ${ARGN})
    foreach(shape OWN_HANDLE NEXT DISPATCHER)
        string(TOLOWER "${shape}" lower)
        add_library(vocem_stub_chain_${lower}${suffix} SHARED stub_chain_overlay.cpp)
        target_compile_definitions(vocem_stub_chain_${lower}${suffix} PRIVATE CHAIN_${shape})
        target_include_directories(vocem_stub_chain_${lower}${suffix} PRIVATE
            "${CMAKE_SOURCE_DIR}/gl/src")
        set_target_properties(vocem_stub_chain_${lower}${suffix} PROPERTIES
            CXX_VISIBILITY_PRESET hidden)
        target_compile_options(vocem_stub_chain_${lower}${suffix} PRIVATE ${options})
        target_link_options(vocem_stub_chain_${lower}${suffix} PRIVATE ${options})
        target_link_libraries(vocem_stub_chain_${lower}${suffix} PRIVATE ${CMAKE_DL_LIBS})
    endforeach()
    # MangoHud's shape once more, bound to itself (see the stub's header).
    add_library(vocem_stub_chain_symbolic${suffix} SHARED stub_chain_overlay.cpp)
    target_compile_definitions(vocem_stub_chain_symbolic${suffix} PRIVATE CHAIN_OWN_HANDLE)
    target_include_directories(vocem_stub_chain_symbolic${suffix} PRIVATE
        "${CMAKE_SOURCE_DIR}/gl/src")
    set_target_properties(vocem_stub_chain_symbolic${suffix} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
    target_compile_options(vocem_stub_chain_symbolic${suffix} PRIVATE ${options})
    target_link_options(vocem_stub_chain_symbolic${suffix} PRIVATE ${options} -Wl,-Bsymbolic)
    target_link_libraries(vocem_stub_chain_symbolic${suffix} PRIVATE ${CMAKE_DL_LIBS})
    # MangoHud's shape asking the global dispatcher inside its own present.
    add_library(vocem_stub_chain_asks${suffix} SHARED stub_chain_overlay.cpp)
    target_compile_definitions(vocem_stub_chain_asks${suffix} PRIVATE CHAIN_OWN_HANDLE CHAIN_ASKS)
    target_include_directories(vocem_stub_chain_asks${suffix} PRIVATE
        "${CMAKE_SOURCE_DIR}/gl/src")
    set_target_properties(vocem_stub_chain_asks${suffix} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
    target_compile_options(vocem_stub_chain_asks${suffix} PRIVATE ${options})
    target_link_options(vocem_stub_chain_asks${suffix} PRIVATE ${options})
    target_link_libraries(vocem_stub_chain_asks${suffix} PRIVATE ${CMAKE_DL_LIBS})
    # SDL's shape: looks the present up for the game, hooks nothing.
    add_library(vocem_stub_chain_lookup${suffix} SHARED stub_chain_lookup.cpp)
    set_target_properties(vocem_stub_chain_lookup${suffix} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
    target_compile_options(vocem_stub_chain_lookup${suffix} PRIVATE ${options})
    target_link_options(vocem_stub_chain_lookup${suffix} PRIVATE ${options})
    target_link_libraries(vocem_stub_chain_lookup${suffix} PRIVATE ${CMAKE_DL_LIBS})
    add_library(vocem_stub_gl_counter${suffix} SHARED stub_gl_counter.cpp)
    set_target_properties(vocem_stub_gl_counter${suffix} PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_compile_options(vocem_stub_gl_counter${suffix} PRIVATE ${options})
    target_link_options(vocem_stub_gl_counter${suffix} PRIVATE ${options})
endfunction()
function(vocem_chain_env out suffix shim linked)
    set(${out}
        "VOCEM_CHAIN_SHIM=$<TARGET_FILE:${shim}>"
        "VOCEM_CHAIN_OWN_HANDLE=$<TARGET_FILE:vocem_stub_chain_own_handle${suffix}>"
        "VOCEM_CHAIN_SYMBOLIC=$<TARGET_FILE:vocem_stub_chain_symbolic${suffix}>"
        "VOCEM_CHAIN_ASKS=$<TARGET_FILE:vocem_stub_chain_asks${suffix}>"
        "VOCEM_CHAIN_LOOKUP=$<TARGET_FILE:vocem_stub_chain_lookup${suffix}>"
        "VOCEM_CHAIN_NEXT=$<TARGET_FILE:vocem_stub_chain_next${suffix}>"
        "VOCEM_CHAIN_DISPATCHER=$<TARGET_FILE:vocem_stub_chain_dispatcher${suffix}>"
        "VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_stub_gl_counter${suffix}>"
        "VOCEM_CHAIN_LINKED=$<TARGET_FILE:${linked}>"
        PARENT_SCOPE)
endfunction()
vocem_chain_stubs("")
vocem_test(shim_chain_linked SOURCES shim_chain.cpp DEFINES CHAIN_LINKED LINK EGL NO_TEST)
vocem_chain_env(VOCEM_CHAIN_ENV "" vocem_gl_shim vocem_shim_chain_linked)
vocem_test(shim_chain LINK ${CMAKE_DL_LIBS} ENV ${VOCEM_CHAIN_ENV} SKIP)
if(VOCEM_HAVE_M32)
    vocem_chain_stubs(32 -m32)
    vocem_m32_twin(shim_chain_linked)
    vocem_chain_env(VOCEM_CHAIN32_ENV 32 vocem_gl_shim32 vocem_shim_chain_linked32)
endif()
vocem_m32_twin(shim_chain ENV ${VOCEM_CHAIN32_ENV})

# vocem-why prints one answer when the daemon is down (`systemctl is-active`
# exits 3; entry 99 is the same fault). Stub systemctl on PATH.
# SERIAL: it asks the script about `cmake` -- this test's own process -- and a
# `cmake` of another test can exit between pgrep and the read (DESIGN 193).
vocem_script_test(vocem_why SERIAL ARGS "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}")

# Two OpenGL interposers in one process, against the real MangoHud: with the
# shim first both draw, with MangoHud first only MangoHud does. The witness is
# our own debug log, not pixels (MangoHud's are foreign pixels too).
find_file(VOCEM_MANGOHUD_SHIM libMangoHud_shim.so
    PATHS /usr/lib/mangohud /usr/lib64/mangohud /usr/local/lib/mangohud
    NO_DEFAULT_PATH)
vocem_script_test(gl_beside_mangohud ARGS ${VOCEM_GL_PROBE_ARGS}
    "-DMANGOHUD_SHIM=${VOCEM_MANGOHUD_SHIM}")

# A bank that has not arrived yet is not missing: a refused open is not
# remembered for the life of the process. The sandbox is built with bwrap.
vocem_test(emoji_bank_arrives LINK rt
    ENV "VOCEM_TEST_BANK_SOURCE=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba" SKIP)

# ---- 0.1.11 common lane ----

# A publish never blanks a game's frame: a writer publishing ~10k states a
# second beside a million polls, private /dev/shm. No nullptr, no torn
# snapshot, a foreign ABI said with its version. Both widths.
vocem_test(state_poll_contention INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt pthread
    TIMEOUT 150 SKIP)
vocem_m32_twin(state_poll_contention)

# Another local user can make /dev/shm/vocem-<uid> first: the trust rule as a
# function, then planted objects at 0666/0640 every reader and writer refuses,
# and at 0600, which they read.
vocem_test(segment_trust INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt SKIP)
vocem_m32_twin(segment_trust)

# The injected code's log: two threads, 100000 lines each, into stderr and
# VOCEM_LOG_FILE at once -- every line whole in both; the file's descriptor
# carries FD_CLOEXEC; a VOCEM_LOG_FILE that cannot be opened is said. Both
# widths: the logger is compiled into both injected libraries.
vocem_test(overlay_log_lines LINK vocem_common pthread)
vocem_m32_twin(overlay_log_lines)

# A journal that cannot be created is not asked for on every frame: 100
# frames of OverlaySession::journal_begin_once() against a read-only journal
# directory, opendir() counted by interposition -- at most one attempt's two
# walks, and the refusal said once.
vocem_test(journal_retry_cadence IMGUI LINK vocem_common ${CMAKE_DL_LIBS} SKIP)

# A journal's suffix is the end of its name: a cache path carrying
# ".running" or ".done" in a directory name (a Flatpak id, a backup) must not
# be cut there. The mapping for the review's cases, then a whole journal life
# under such a path.
vocem_test(journal_suffix)

# A daemon that reopens a segment it did not unlink clears it under the
# seqlock: a reader's sequence before and after the reopening differ (the
# deterministic half, on a segment left at sequence 0), and a reader thread
# beside a writer that reopens the same object in a loop never accepts a
# half-cleared snapshot. Private /dev/shm, both widths.
vocem_test(shm_reopen_clear INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt pthread SKIP)
vocem_m32_twin(shm_reopen_clear)

# With no HOME the settings and the token are nowhere, never in the working
# directory: a config.ini there that switches the overlay off is not
# honoured, a token there is not read, and a save puts nothing there. Both
# widths: Config::path() runs inside games.
vocem_test(no_home)
vocem_m32_twin(no_home)

# The display's mode is the one scanned out: every connector's mode and rate
# match an independent walk of the CRTCs. A witness here, where running and
# preferred heights agree (display.h says why).
vocem_test(display_running_mode SKIP)

# ---- 0.1.11 gui lane ----
# The window's save rewrites the file in place: comments, unknown keys, a
# symlink and the file's mode all survive a click on a switch. The fresh
# file it used to rename over config.ini lost every one of them.
vocem_test(config_rewrite_in_place)

# A name holding the lists' separator (or edge whitespace) is refused, where
# it used to be stored as two other names. The overlay reads these lists
# inside games at both widths, so both are run.
vocem_test(apps_list_separator)
vocem_m32_twin(apps_list_separator)

# Nothing installed carries a RUNPATH: Qt's deployment defaults gave the
# packaged vocem-config $ORIGIN:$ORIGIN/../lib. Measured on a scratch install.
vocem_script_test(installed_runpath ARGS "-DBUILD_DIR=${CMAKE_BINARY_DIR}")

# A Vulkan loader in depends=() only if something shipped NEEDs libvulkan --
# nothing does -- and the two PKGBUILDs and .SRCINFO list the same depends and
# optdepends.
vocem_script_test(package_depends_needed
    ARGS "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DBUILD32_DIR=${VOCEM_BUILD32_DIR}")

if(TARGET vocem-config)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml)
    find_package(Fontconfig QUIET)

    # The window never signals a daemon it did not ask about: under the harness
    # every stop fell to `pkill -x vocemd`, the live daemon included. Stubs on
    # PATH log what would have run.
    vocem_test(window_daemon_lifecycle NO_TEST
        SOURCES window_daemon_lifecycle.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_window_daemon_lifecycle PROPERTIES AUTOMOC ON)
    vocem_test_run(window_daemon_harness TARGET window_daemon_lifecycle
        ENV "VOCEM_WINDOW_DAEMON_SCENARIO=harness" SKIP)
    vocem_test_run(window_daemon_early_quit TARGET window_daemon_lifecycle
        ENV "VOCEM_WINDOW_DAEMON_SCENARIO=early-quit" SKIP)
    # The font box follows the setting after its list has been opened: the
    # assignment that grew the list removed the binding, and a family written
    # outside the window changed the box's text and not its row or its face.
    vocem_script_test(font_picker_follows ARGS "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>")
    # "Start at login" reads the desktop's own "off" (Hidden=true,
    # X-GNOME-Autostart-enabled=false), Apply of another setting leaves the
    # entry alone, and a write that fails is said.
    vocem_test(window_autostart SKIP
        SOURCES window_autostart.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_window_autostart PROPERTIES AUTOMOC ON)
    # "Active in this session" only when this session's environment has the
    # preload: the user manager carrying it is a different fact, and programs
    # started from the desktop do not get it until the next login.
    vocem_test(window_preload_state SKIP
        SOURCES window_preload_state.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_window_preload_state PROPERTIES AUTOMOC ON)
    vocem_test_run(window_daemon_slow_probe TARGET window_daemon_lifecycle
        ENV "VOCEM_WINDOW_DAEMON_SCENARIO=slow-probe" SKIP)
endif()

# ---- 0.1.11 gl lane ----

# The shim's first lookup of a real function, asked by sixteen threads at once,
# in three hundred fresh processes: "tried" was remembered before the lookup
# had its answer, and a thread arriving in between was told the function did
# not exist (shim_lookup_race.cpp). Both widths, against the shim of each.
vocem_test(shim_lookup_race LINK EGL Threads::Threads ENV ${VOCEM_SHIM_ENV} SKIP TIMEOUT 150)
vocem_m32_twin(shim_lookup_race ENV ${VOCEM_SHIM32_ENV})

# The owner context destroyed while not current on the destroying thread: the
# teardown's glXMakeCurrent dance was an X error, and Xlib's default handler
# exits the game (gl_destroy_owner.cpp). `visual` on the machine's own GLX;
# `thread` under Mesa's software GLX, where the measured BadAccess is.
vocem_test(gl_destroy_owner LINK ${CMAKE_DL_LIBS} X11 rt Threads::Threads LINK_OPTIONS -rdynamic
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=visual" SKIP)
vocem_test_run(gl_destroy_owner_thread TARGET gl_destroy_owner
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=thread"
        "__GLX_VENDOR_LIBRARY_NAME=mesa" "LIBGL_ALWAYS_SOFTWARE=1" SKIP)
vocem_m32_twin(gl_destroy_owner
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=visual"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_m32_twin_run(gl_destroy_owner_thread TARGET gl_destroy_owner_thread
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=thread"
        "__GLX_VENDOR_LIBRARY_NAME=mesa" "LIBGL_ALWAYS_SOFTWARE=1")

# A game whose runtime puts an older libstdc++ first: the heavy libraries
# carry their C++ runtime, so the overlay loads (old-runtime), and a failed
# load is said by the shim with nothing loaded in its place (unloadable). The
# stub is libstdc++ 6.0.21's version nodes, in a directory of its own.
function(vocem_old_libstdcxx_stub target)
    add_library(${target} SHARED stub_old_libstdcxx.cpp)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "stdc++" SOVERSION 6
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}"
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stub_old_libstdcxx.map")
    target_compile_options(${target} PRIVATE ${ARGN})
    target_link_options(${target} PRIVATE ${ARGN} -nostdlib++
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/stub_old_libstdcxx.map")
endfunction()
vocem_old_libstdcxx_stub(vocem_stub_old_libstdcxx)
vocem_test(gl_old_libstdcxx LINK GL X11 LINK_OPTIONS -static-libstdc++ -static-libgcc
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=old-runtime"
        "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/vocem_stub_old_libstdcxx" SKIP)
add_dependencies(vocem_gl_old_libstdcxx vocem_stub_old_libstdcxx)
vocem_test_run(gl_unloadable_library TARGET gl_old_libstdcxx
    ENV ${VOCEM_SHIM_ENV} "VOCEM_GL_SCENARIO=unloadable"
        "VOCEM_GL_LIBRARY=${CMAKE_CURRENT_BINARY_DIR}/no-such-dir/libvocem_gl.so" SKIP)
if(VOCEM_HAVE_M32)
    vocem_old_libstdcxx_stub(vocem_stub_old_libstdcxx32 -m32)
endif()
vocem_m32_twin(gl_old_libstdcxx
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=old-runtime"
        "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/vocem_stub_old_libstdcxx32"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
if(TARGET vocem_gl_old_libstdcxx32)
    add_dependencies(vocem_gl_old_libstdcxx32 vocem_stub_old_libstdcxx32)
endif()
vocem_m32_twin_run(gl_unloadable_library TARGET gl_unloadable_library
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_SCENARIO=unloadable"
        "VOCEM_GL_LIBRARY=${CMAKE_CURRENT_BINARY_DIR}/no-such-dir/libvocem_gl.so")

# The artefact half of the same: neither injected heavy library NEEDS the C++
# runtime or binds a symbol of its versions, both widths (injected_runtime.cmake).
vocem_script_test(injected_runtime ARGS
    "-DGL=$<TARGET_FILE:vocem_gl>" "-DVK=$<TARGET_FILE:vocem_vk>"
    "-DGL32=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    "-DVK32=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# The shim as built -- libc alone, its twelve hooks, no guard, nothing
# undefined outside libc -- on this tree's, the -m32 twin's and build32's; and
# its link, which has to refuse an undefined C++ runtime symbol rather than
# leave it for ld.so to fail on in every process (shim_artifact.cmake).
set(VOCEM_SHIM_ARTIFACT_ARGS
    "-DCXX=${CMAKE_CXX_COMPILER}"
    "-DSHIM=$<TARGET_FILE:vocem_gl_shim>"
    "-DSHIM_BUILD32=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so"
    "-DFLAGS=$<JOIN:$<TARGET_PROPERTY:vocem_gl_shim,COMPILE_OPTIONS>,|>"
    "-DLINK_FLAGS=$<JOIN:$<TARGET_PROPERTY:vocem_gl_shim,LINK_OPTIONS>,|>")
if(TARGET vocem_gl_shim32)
    list(APPEND VOCEM_SHIM_ARTIFACT_ARGS
        "-DSHIM_TWIN32=$<TARGET_FILE:vocem_gl_shim32>"
        "-DFLAGS32=$<JOIN:$<TARGET_PROPERTY:vocem_gl_shim32,COMPILE_OPTIONS>,|>"
        "-DLINK_FLAGS32=$<JOIN:$<TARGET_PROPERTY:vocem_gl_shim32,LINK_OPTIONS>,|>")
endif()
vocem_script_test(shim_artifact ARGS ${VOCEM_SHIM_ARTIFACT_ARGS})

# The backend moving between two unshared contexts (entry 210's hand-over) and
# coming back: what it built in a context it left is deleted when that context
# is current again, so each holds one atlas however often the overlay returns
# (gl_handover.cpp). About eleven seconds: two round trips past the library's
# own two-second hand-over.
vocem_test(gl_handover LINK EGL GLESv2 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP TIMEOUT 120)
vocem_m32_twin(gl_handover
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# eglTerminate with another, unshared context current: the backend's names are
# that context's own objects there, and the teardown deleted them
# (gl_egl_terminate.cpp).
vocem_test(gl_egl_terminate LINK EGL GLESv2 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)
vocem_m32_twin(gl_egl_terminate
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# A glvnd dispatcher asked for the other family's name answers with a GL
# dispatch stub, and the shim remembered that stub as the real present
# function for good (shim_dispatch_family.cpp). Both widths, against the shim
# of each.
vocem_test(shim_dispatch_family LINK ${CMAKE_DL_LIBS} ENV ${VOCEM_SHIM_ENV} SKIP)
vocem_m32_twin(shim_dispatch_family ENV ${VOCEM_SHIM32_ENV})

# An ES game whose GL cannot be reached until after the backend has chosen its
# shader language: the backend took desktop GLSL, the ES context refused it,
# and the program that never linked was logged as ready (gl_es_glsl.cpp).
vocem_test(gl_es_glsl LINK ${CMAKE_DL_LIBS} rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)
vocem_m32_twin(gl_es_glsl
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# VOCEM_CAPTURE_FRAME read the frame back through the game's read framebuffer,
# its GL_PACK_* state and its pixel-pack buffer (gl_capture_state.cpp).
set(VOCEM_GL_PROBE32_ENV
    "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
    "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_test(gl_capture_state LINK ${CMAKE_DL_LIBS} X11 rt
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=read-framebuffer" SKIP)
vocem_test_run(gl_capture_pack_buffer TARGET gl_capture_state
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=pack-buffer" SKIP)
vocem_m32_twin(gl_capture_state
    ENV ${VOCEM_GL_PROBE32_ENV} "VOCEM_GL_SCENARIO=read-framebuffer"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_m32_twin_run(gl_capture_pack_buffer TARGET gl_capture_pack_buffer
    ENV ${VOCEM_GL_PROBE32_ENV} "VOCEM_GL_SCENARIO=pack-buffer")

# scripts/dev-run-gl.sh under the session's own LD_PRELOAD: the installed shim
# was first and took every hook, and the build tree's shim was never called
# (dev_run_gl.cmake).
vocem_test(which_shim LINK ${CMAKE_DL_LIBS} NO_TEST)
vocem_script_test(dev_run_gl ARGS "-DPROBE=$<TARGET_FILE:vocem_which_shim>"
    "-DPROBE32=$<$<TARGET_EXISTS:vocem_which_shim32>:$<TARGET_FILE:vocem_which_shim32>>")

# ---- 0.1.11 daemon lane ----
# Who the Flatpak bridge serves the voice channel to: a sandbox's own
# `drawing=1` is its word, and the host decides by application id -- an
# exported desktop entry that says Game, or the id listed in flatpak_apps.
vocem_test(flatpak_consent
    SOURCES flatpak_consent.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common SKIP)
# A listener in a Flatpak sandbox that is not Discord's is not sent the token:
# the peer socket is mapped to its process and that process's /.flatpak-info
# read. The listener really is sandboxed -- a nested bwrap with its own user
# and pid namespace, the network shared -- inside daemon_notification's sandbox.
if(TARGET vocemd)
    vocem_test(daemon_peer_sandbox INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 120 SKIP)
    # The same question where the daemon runs: the real vocemd as a transient
    # unit with every [Service] property of the unit file (private network,
    # /dev/shm, app directory, measured first); a listener in a scope named like
    # Discord's is sent the token, like a Flatpak's is refused, and a Flatpak
    # game's scope is seen by the bridge. Fails against 0.1.11-1 (entry 285).
    # VOCEM_UNIT_FILE can name the packaged unit.
    vocem_test(daemon_unit_peer INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}"
        ENV ${VOCEM_DAEMON_ENV}
            "VOCEM_UNIT_FILE=${CMAKE_SOURCE_DIR}/packaging/systemd/vocemd.service.in"
        TIMEOUT 180 SKIP)
endif()
# Links and FIFOs at the avatar cache's names: the writer's `.part` and the
# bridge's copy source. Nothing is written through a link, nothing waits on a
# FIFO, and only a regular file of the format's one size is a face.
vocem_test(avatar_cache_hostile
    SOURCES avatar_cache_hostile.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common SKIP)
# A SIGTERM ends a daemon blocked inside a system call: the handler is installed
# without SA_RESTART, so a blocked open() returns EINTR and the flag is read.
if(TARGET vocemd)
    vocem_test(daemon_stop_blocked INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 90 SKIP)
endif()
# One daemon per user: the second takes no lock, says so and exits 0, and the
# first keeps its segment named. Two real daemons in one private /dev/shm.
if(TARGET vocemd)
    vocem_test(daemon_single_instance INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 90 SKIP)
endif()
# One record per Flatpak sandbox, under a file name no host record can have:
# a sandbox renaming itself cannot fill the registry or overwrite a host row.
vocem_test(flatpak_records
    SOURCES flatpak_records.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# A settings copy into a sandbox that failed is tried again on the next sweep,
# rather than only after the host's config.ini next changes.
vocem_test(flatpak_config_retry
    SOURCES flatpak_config_retry.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# The emoji bank crosses into a sandbox once: a sandbox that stops asking and
# asks again is not given sixteen megabytes again, on the daemon's main thread.
vocem_test(flatpak_emoji_once
    SOURCES flatpak_emoji_once.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common SKIP)
# What the peer names reaches the daemon's log as one line of text: a refused
# RPC command is logged through sanitise_text, like every other string from it.
if(TARGET vocemd)
    vocem_test(daemon_log_lines INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 90 SKIP)
endif()

# ---- 0.1.11 vk lane ----

# A swapchain whose images get memory at first acquire (DEFERRED_MEMORY_
# ALLOCATION): nothing is made from an image before then (every submit failed,
# device lost under validation). Witnessed, and validated.
vocem_test_run(vk_present_deferred TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=deferred" SKIP)
vocem_m32_twin_run(vk_present_deferred TARGET vk_present_deferred
    ENV ${VOCEM_VK_PROBE32_ENV} "VOCEM_VK_SCENARIO=deferred")

# A second device presenting beside the first: passed through while both
# present, moved after the hand-over interval (entry 210), moved back when it
# is destroyed.
vocem_test_run(vk_present_second_presenter TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=second-presenter" SKIP TIMEOUT 120)
vocem_m32_twin_run(vk_present_second_presenter TARGET vk_present_second_presenter
    ENV ${VOCEM_VK_PROBE32_ENV} "VOCEM_VK_SCENARIO=second-presenter")

# A sequence is rewritten into its key only once the atlas has the key's glyph.
# The Vulkan layer notes and draws in one draw() and folds after the present,
# so a sequence nobody had shown yet drew its key as '?' for a frame: after
# fonts_prepare_text and no ensure_fonts, every key left in the text must have
# a glyph, and after the fold the text must collapse (the positive control).
vocem_test(fonts_key_drawable IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)
vocem_m32_twin(fonts_key_drawable)

# One emoji sequence must not grow ImGui's glyph index to its on-disk key's
# number (U+F0000 up): 1,520 kB -> 11,530 kB for the two weights, +11.4 MB of
# RSS, after one flag. The keys are numbered in the BMP's private use area at
# run time, and a name spelling a key's codepoint is neutralised.
vocem_test(fonts_key_index IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)
vocem_m32_twin(fonts_key_index)

# The texture cache refused (the witness fails its sampler): the renderer fell
# back to ImGui's stock font upload, whose command buffer the loader never
# registered (rule 5). It declines to draw now, said once. Also a scene of
# vk_present_validated, with the validation layer between overlay and witness.
vocem_test_run(vk_present_no_texture_cache TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=no-texture-cache" SKIP)

# The same on one device: a second window presented from the family's second
# queue passed the family check and was drawn with the first queue's uploads
# behind it. Passed through, moved after the hand-over interval, moved back.
vocem_test_run(vk_present_second_queue TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=second-queue" SKIP TIMEOUT 120)
vocem_m32_twin_run(vk_present_second_queue TARGET vk_present_second_queue
    ENV ${VOCEM_VK_PROBE32_ENV} "VOCEM_VK_SCENARIO=second-queue")

# ---- 0.1.11 round 2 daemon lane ----
# A segment the reader refuses (not this user's 0600 regular file) is not "no
# daemon": the CLI says it was refused and why, once and under --watch.
# Private /dev/shm; skips where bwrap is missing.
if(TARGET vocem)
    vocem_test(cli_refused_segment INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV "VOCEM_CLI=$<TARGET_FILE:vocem>" TIMEOUT 60 SKIP)
endif()

# ---- 0.1.11 round 2 gui lane ----
if(TARGET vocem-config)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml)
    find_package(Fontconfig QUIET)

    # Apply writes the keys the window changed on top of the file as it stands.
    # It saved the window's whole copy, which does not follow the file while an
    # edit is pending, so a hand-written flatpak_apps -- consent the window has
    # no control for -- went back to its startup value on the next Apply.
    vocem_test(config_apply_keeps_outside
        SOURCES config_apply_keeps_outside.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_config_apply_keeps_outside PROPERTIES AUTOMOC ON)

    # The switch beside an application whose process name the kernel cut
    # after a space ("Slay the Spire 2" runs as "Slay the Spire "): no list
    # entry can hold that name, so the rule goes on the executable's name,
    # which the overlay also matches -- refused only when neither fits.
    vocem_test(config_bridge_comm_name
        SOURCES config_bridge_comm_name.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_config_bridge_comm_name PROPERTIES AUTOMOC ON)
endif()

# save() on a config.ini that exists and cannot be read fails, where it took
# the empty read for "no file" and renamed a fresh file of the window's
# values over it -- contents gone, the 0200 mode kept.
vocem_test(config_save_unreadable SKIP)

# The 32-bit configure's compiler check links a plain `c++ -m32` executable,
# which opens lib32-gcc-libs' files; since the injected libraries carry their
# C++ runtime inside, that package left depends=() and makepkg -s no longer
# installed it. And package() refuses to replace an exemplar, as build()
# does: makepkg -R runs package() alone.
vocem_script_test(package_m32_makedepends)
vocem_script_test(package_refuses_exemplar)

# A config.ini linked to a file that does not exist yet stays a link: the
# realpath() the save relied on fails there, and the rename put a regular
# file where the link was.
vocem_test(config_save_dangling_link)

# A line load() refused (past 64 KiB) survives the window's save, where one
# click on a switch rewrote an 8000-entry hidden_apps as `hidden_apps = `.
vocem_test(config_save_long_line)

# ---- 0.1.11 round 2 inj lane ----

# An owner sharing its objects with a loader context, destroyed SDL's and
# GLFW's way -- NULL made current first: the first round's "not current, drop
# without GL" left the backend's program, buffers and font texture in the share
# group, one set per owner (gl_destroy_owner.cpp, `shared`). Both widths.
vocem_test_run(gl_destroy_owner_shared TARGET gl_destroy_owner
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=shared" SKIP)
vocem_m32_twin_run(gl_destroy_owner_shared TARGET gl_destroy_owner_shared
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=shared")
# The same scene in a game that dlopens libX11 RTLD_LOCAL, as SDL and GLFW do:
# the overlay's X error trap has to find Xlib among the loaded objects.
vocem_test(gl_destroy_private_x11 SOURCES gl_destroy_owner.cpp DEFINES VOCEM_PROBE_PRIVATE_X11
    LINK ${CMAKE_DL_LIBS} rt Threads::Threads LINK_OPTIONS -rdynamic
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=shared" SKIP)
vocem_m32_twin(gl_destroy_private_x11
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=shared"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# The two heavy libraries loaded and unloaded forty times each: the libstdc++
# they carry inside them since 0.1.11 left its ~73 KB exception emergency pool
# on the heap at every unload, which is every vkDestroyInstance for the layer,
# and 8c6db47's ELF destructors an atlas worker (injected_unload.cpp). Both
# widths, both libraries.
vocem_test(injected_unload LINK ${CMAKE_DL_LIBS}
    ENV "LD_PRELOAD=" "VOCEM_GL_LIBRARY=$<TARGET_FILE:vocem_gl>"
        "VOCEM_VK_LIBRARY=$<TARGET_FILE:vocem_vk>")
vocem_m32_twin(injected_unload
    ENV "LD_PRELOAD=" "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
        "VOCEM_VK_LIBRARY=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# A backend left in a context, torn down while the atlas worker rasterises for
# the context the overlay moved to: reclaim_left() (a present) and forget_left()
# (a destroy) destroyed the left ImGui context without joining the worker, whose
# allocations count themselves through the same global context pointer
# (gl_handover.cpp, `worker`, `worker-destroy`). Both widths.
vocem_test_run(gl_handover_worker TARGET gl_handover
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=worker" SKIP TIMEOUT 120)
vocem_m32_twin_run(gl_handover_worker TARGET gl_handover_worker
    ENV ${VOCEM_GL_PROBE32_ENV} "VOCEM_GL_SCENARIO=worker")
vocem_test_run(gl_handover_worker_destroy TARGET gl_handover
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=worker-destroy" SKIP TIMEOUT 120)
vocem_m32_twin_run(gl_handover_worker_destroy TARGET gl_handover_worker_destroy
    ENV ${VOCEM_GL_PROBE32_ENV} "VOCEM_GL_SCENARIO=worker-destroy")

# A context whose backend program does not link (Mesa, a desktop context held
# at 2.1 by MESA_GL_VERSION_OVERRIDE, so no GLSL 1.30), and then a context the
# backend can live in (ES 2 on the same display): the failure used to be
# remembered for the whole process (gl_failed_context.cpp). `silent`: F stops
# presenting and G gets the overlay at the hand-over; `destroyed`: F is
# destroyed and G gets it at once. Both widths.
set(VOCEM_GL_MESA_EGL_ENV
    "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    "LIBGL_ALWAYS_SOFTWARE=1" "MESA_GL_VERSION_OVERRIDE=2.1" "MESA_GLSL_VERSION_OVERRIDE=120")
vocem_test(gl_failed_context LINK EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_GL_MESA_EGL_ENV} "VOCEM_GL_SCENARIO=silent" SKIP)
vocem_m32_twin(gl_failed_context
    ENV ${VOCEM_GL_PROBE32_ENV} ${VOCEM_GL_MESA_EGL_ENV} "VOCEM_GL_SCENARIO=silent"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_test_run(gl_failed_context_destroyed TARGET gl_failed_context
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_GL_MESA_EGL_ENV} "VOCEM_GL_SCENARIO=destroyed" SKIP)
vocem_m32_twin_run(gl_failed_context_destroyed TARGET gl_failed_context_destroyed
    ENV ${VOCEM_GL_PROBE32_ENV} ${VOCEM_GL_MESA_EGL_ENV} "VOCEM_GL_SCENARIO=destroyed")

# One swapchain presented by two queues of the same family in turn, frames in
# flight and colour emoji arriving meanwhile (vk_present_draw.cpp,
# alternate-queue). Both widths.
vocem_test_run(vk_present_alternate_queue TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=alternate-queue"
    SKIP TIMEOUT 120)
vocem_m32_twin_run(vk_present_alternate_queue TARGET vk_present_alternate_queue
    ENV ${VOCEM_VK_PROBE32_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=alternate-queue")

# scripts/dev-run-gl.sh for a 32-bit child: which_shim built -m32, run through
# the script, must be answered by a 32-bit shim (dev_run_gl.cmake, PROBE32).
vocem_m32_twin(which_shim)

# ---- 0.1.11 clean-up tests lane ----

# The daemon's end-to-end tests under its unit's own [Service] properties
# (entry 285): unit_confinement.h re-executes the probe as a transient unit
# with the file's properties plus a private network, /dev/shm and app
# directory, measured, and the probe undumpable so its stub is closed to the
# daemon as Discord is. daemon_notification_unit fails against 0.1.11-1.
# daemon_peer_sandbox has no twin: its listener is a bwrap, which the unit's
# RestrictNamespaces refuses (daemon_unit_peer is its unit form).
if(TARGET vocemd)
    set(VOCEM_UNIT_ENV
        "VOCEM_UNIT_FILE=${CMAKE_SOURCE_DIR}/packaging/systemd/vocemd.service.in")
    foreach(probe daemon_notification daemon_moved daemon_log_lines daemon_exchange_stop
                  daemon_stop_blocked daemon_single_instance)
        vocem_test_run(${probe}_unit TARGET ${probe}
            ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} TIMEOUT 150 SKIP)
    endforeach()
    vocem_test_run(daemon_reconnect_unit TARGET daemon_reconnect
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} TIMEOUT 120 SKIP)
    vocem_test_run(daemon_reconnect_authenticated_unit TARGET daemon_reconnect
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV}
            "VOCEM_DAEMON_RECONNECT_SCENARIO=authenticated" TIMEOUT 120 SKIP)
    vocem_test_run(daemon_note_expiry_unit TARGET daemon_note_expiry
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} TIMEOUT 240 SKIP)
    vocem_test_run(daemon_note_expiry_port_walk_unit TARGET daemon_note_expiry
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV}
            "VOCEM_NOTE_EXPIRY_SCENARIO=port-walk" TIMEOUT 120 SKIP)
    vocem_test_run(daemon_stop_unlinks_unit TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} TIMEOUT 150 SKIP)
    vocem_test_run(daemon_stop_in_handshake_unit TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} "VOCEM_DAEMON_STOP_SCENARIO=handshake"
        TIMEOUT 150 SKIP)
    vocem_test_run(daemon_stop_in_trickle_unit TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} "VOCEM_DAEMON_STOP_SCENARIO=trickle"
        TIMEOUT 150 SKIP)
    # Its fake Flatpak is a process in an app-flatpak-<id> scope here, not a
    # bwrap (fake_flatpak.h): what the bridge sees of a Flatpak from the unit.
    vocem_test_run(daemon_stop_note_mirror_unit TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} ${VOCEM_UNIT_ENV} "VOCEM_DAEMON_STOP_SCENARIO=note-mirror"
        TIMEOUT 150 SKIP)
endif()

# The picture probe tray_icons runs on the installed icons (Qt's own SVG
# renderer, so it needs Qt6 Svg; without it tray_icons skips).
find_package(Qt6 6.5 QUIET COMPONENTS Gui Svg)
if(TARGET Qt6::Svg)
    vocem_test(tray_icon_pictures NO_TEST LINK Qt6::Gui Qt6::Svg)
endif()

# ---- 0.1.11 clean-up measure lane ----

# What the OpenGL overlay costs the process it draws in, as a budget: the same
# binary three times as its own child in one private /dev/shm -- a prime run
# that warms this run's own shader cache, VOCEM_DISABLE=1, the overlay on --
# and on minus off held under a resident-memory and a CPU-per-frame budget
# (overlay_budget.cpp carries the numbers and the margins). The comparison of
# 28 Sep 2026 found +130 MB per process that nobody had ever chosen; this is
# where it is chosen. Serial: a CPU figure taken beside the rest of the suite
# measures the suite.
vocem_test(overlay_budget LINK ${CMAKE_DL_LIBS} GL X11 vulkan rt
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} SKIP SERIAL TIMEOUT 180)
vocem_m32_twin(overlay_budget
    ENV ${VOCEM_GL_PROBE32_ENV} ${VOCEM_EMOJI_BANK_ENV}
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
# The same budget for the Vulkan layer: the child is a Vulkan game presenting
# to an off-screen window, the layer is the build tree's alone in a scratch
# implicit set, and the control is the loader refusing it on VOCEM_DISABLE=1.
vocem_test_run(overlay_budget_vk TARGET overlay_budget
    ENV ${VOCEM_VK_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_BUDGET_API=vk" SKIP SERIAL TIMEOUT 180)
vocem_m32_twin_run(overlay_budget_vk TARGET overlay_budget_vk
    ENV ${VOCEM_VK_PROBE32_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_BUDGET_API=vk")

# ---- 0.1.11 clean-up atlas lane ----

# The hand-over and the atlas worker (vocem/atlas_owner.h), driven without a
# GPU over every sequence of up to six events -- presents from two contexts,
# silence, a context's death, the worker finishing, a failed build, a failed
# upload, the switch -- against eight invariants, with a real pthread per
# build. Both widths, and once more under AddressSanitizer and UBSan, with the
# session's preload cleared so the sanitizer's runtime comes first.
vocem_test(atlas_owner_states IMGUI LINK vocem_common Threads::Threads TIMEOUT 300)
vocem_m32_twin(atlas_owner_states)
vocem_test(atlas_owner_states_asan SOURCES atlas_owner_states.cpp
    "${PROJECT_SOURCE_DIR}/common/src/atlas_owner.cpp" IMGUI LINK vocem_common Threads::Threads
    COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
    LINK_OPTIONS -fsanitize=address,undefined
    ENV "LD_PRELOAD=" "ASAN_OPTIONS=detect_leaks=1" "UBSAN_OPTIONS=print_stacktrace=1"
    ARGS 5 TIMEOUT 600)

# The switch turned off, or the daemon stopping, noticed in a present of an
# unshared context the backend does not live in: the backend's names are that
# context's own objects there, and the release used to delete them
# (gl_switch_foreign.cpp). Both causes, both widths.
vocem_test(gl_switch_foreign LINK EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=switch" SKIP TIMEOUT 120)
vocem_m32_twin(gl_switch_foreign
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=switch"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_test_run(gl_switch_foreign_daemon TARGET gl_switch_foreign
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=daemon" SKIP TIMEOUT 120)
vocem_m32_twin_run(gl_switch_foreign_daemon TARGET gl_switch_foreign_daemon
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=daemon")

# The renderer refused on the first of two presenting devices (the witness fails
# its texture cache's sampler): the failure belongs to that device, which holds
# the overlay as a working renderer would until it falls silent or goes, and the
# second device gets it then (vk_present_draw.cpp, failed-presenter). 64-bit
# only: the refusal needs the witness layer, which is built at one width.
vocem_test_run(vk_present_failed_presenter TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=failed-presenter" SKIP TIMEOUT 120)

# ---- 0.1.11 clean-up config lane ----

# The settings file under generated hostile shapes -- long lines, NUL, CR/LF,
# bytes that are not UTF-8, links, dangling links, loops, mode 000, read-only,
# a directory, a FIFO, /dev/zero -- through the one writer the window's Apply
# and switches use: a file not read whole is never written, nothing blocks,
# and every line the change did not concern stays byte for byte. Both widths:
# the reader is compiled into every game.
vocem_test(config_hostile)
vocem_m32_twin(config_hostile)

# The settings' copy into a Flatpak sandbox against hostile shapes at both
# ends: links and FIFOs planted by the sandbox, a host file that is a link, a
# FIFO, unreadable, dangling, too large or removed. Nothing blocks, nothing is
# written through a link, a failed copy is retried, a removed file is removed.
vocem_test(flatpak_config_hostile
    SOURCES flatpak_config_hostile.cpp ${VOCEM_FLATPAK_BRIDGE_SOURCES}
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common SKIP)

# A slider's ends are config.h's clamps (Config::numbers(), compiled): every
# Slider and SpinBox in the running window is moved to both ends and the
# setting it changed must land on the clamp's ends ("Show for" once offered
# 1-20 over 1-30). And every Switch writes config.ini on the click while every
# CheckBox waits for Apply (KDE HIG), measured by clicking each one
# (VOCEM_CONFIG_DRIVE, window_controls.cmake). They replace two readings of
# the QML source.
vocem_test(config_numbers NO_TEST)
if(TARGET vocem-config)
    vocem_script_test(slider_bounds
        ARGS ${VOCEM_WINDOW_ARG} "-DNUMBERS_BINARY=$<TARGET_FILE:vocem_config_numbers>")
    vocem_script_test(instant_switches ARGS ${VOCEM_WINDOW_ARG})
endif()

# A window that cannot create its single-instance socket (a runtime directory
# past the 107 bytes a socket address holds) says so and gives the lock back,
# so a later launch opens a window rather than exiting with nothing shown.
if(TARGET vocem-config)
    vocem_script_test(single_instance_long_path ARGS ${VOCEM_WINDOW_ARG})
endif()

# ---- 0.1.11 clean-up refix lane ----

# failed-presenter with both devices presenting from the first frame, each on
# a thread of its own and the process on one CPU: the refused renderer's
# failure is held by the device the witness refused, not by whichever thread
# took the layer's lock first, and the other device gets the overlay once that
# one falls silent (vk_present_draw.cpp, failed-threads). 64-bit only, as
# failed-presenter.
vocem_test_run(vk_present_failed_threads TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=failed-threads" SKIP TIMEOUT 120)

# A release asked while the first atlas is still being rasterised -- the switch
# turned off, the daemon stopping, the hand-over -- waits for the build without
# holding the game's swap: the process on one CPU, the worker at SCHED_IDLE and
# a spinner beside it make the build last seconds (gl_release_mid_build.cpp).
# Both widths.
vocem_test(gl_release_mid_build LINK EGL GLESv2 rt Threads::Threads
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=switch" SKIP TIMEOUT 120)
vocem_m32_twin(gl_release_mid_build
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so" "VOCEM_GL_SCENARIO=switch"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
foreach(scene daemon handover)
    vocem_test_run(gl_release_mid_build_${scene} TARGET gl_release_mid_build
        ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=${scene}" SKIP TIMEOUT 120)
    vocem_m32_twin_run(gl_release_mid_build_${scene} TARGET gl_release_mid_build_${scene}
        ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
            "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
            "VOCEM_GL_SCENARIO=${scene}")
endforeach()

# The systemd-run command of the `_unit` tests keeps the test's environment off
# its own command line, where every local user reads it (unit_command_env.cpp):
# --setenv=NAME, the values in systemd-run's environment.
vocem_test(unit_command_env)

# Two processes writing config.ini at once, a key each, 300 writes apiece
# (config_two_writers.cpp): none refused, every write that answered true on
# the file at the end, no temporary left.
vocem_test(config_two_writers)
