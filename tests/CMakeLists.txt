# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Every test in this suite is one of the shapes VocemTest.cmake defines: a
# compiled probe (vocem_test), a second run of one (vocem_test_run), its -m32
# twin (vocem_m32_twin), a cmake -P script (vocem_script_test), or a test that
# only says why it could not run here (vocem_skip). The comment above each is
# the defect it holds; the defaults are the header's.

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

# The fonts across the death of the ImGui context they were built in -- the
# lifecycle both injected paths actually have (the GL toggle, a second Vulkan
# device), which used to leave fonts() pointing into a freed atlas and crash the
# game on the first frame after the overlay came back.
vocem_test(fonts_lifecycle IMGUI LINK vocem_common)

# The font the settings name, drawn -- with Inter merged underneath it for what
# it does not carry, and a spoken reason whenever the file cannot be used.
# Measured against this machine's own installed fonts, so it skips (77) on a
# machine with none.
vocem_test(system_font IMGUI LINK vocem_common SKIP)

# The panel's geometry, measured out of the vertices ImGui produces rather than
# judged from a screenshot. Same sources as above plus the panel itself, which is
# the point: this runs the code the layer runs.
vocem_test(panel_geometry IMGUI LINK vocem_common)

# Not a test: a channel that is not there, for looking at the configuration window
# with a full one in it. scripts/compare-preview.py runs it against a private
# /dev/shm so it cannot be mistaken for the daemon.
vocem_test(fake_state NO_TEST
    SOURCES fake_state.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src"
    LINK rt vocem_common)

# The list of applications the overlay stays out of, and the record each process
# writes of itself. Both are read from inside other people's games, so both are
# held to the cases that come out of a game rather than out of a test: a name
# truncated by the kernel, a name with a space in it, a list edited by hand.
# One of its cases is a FIFO left where the record's temporary file goes: the
# defective version blocks in open() forever rather than failing, so the timeout
# is how that case reports itself.
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

# The clean-exit rename through the destructor's REAL door: a probe library
# dlopened the way the shim loads the heavy one, ended by its ELF destructor
# after static destruction -- the exit order in which the first version's
# rename read a freed std::string and silently did nothing, leaving every
# clean session as a false crash report. journal.cpp calls journal_end()
# directly and could never see it: the wrong witness, in the test built to
# prevent exactly that.
add_library(vocem_journal_exitprobe MODULE journal_exitprobe.cpp)
target_include_directories(vocem_journal_exitprobe PRIVATE "${CMAKE_SOURCE_DIR}/include")
vocem_test(journal_exit LINK ${CMAKE_DL_LIBS}
    ENV "VOCEM_JOURNAL_EXITPROBE=$<TARGET_FILE:vocem_journal_exitprobe>" SKIP)

# The Vulkan path's two-step handoff of a message's words: draw() records
# which toast it wants, the post-present phase fetches the words for the next
# frame, and the toast's end forgets them. The GL side has a frame-level
# witness (gl_toast_alone); Vulkan has none, and this holds the half that is
# ours rather than letting the suite imply otherwise.
vocem_test(note_handoff INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt SKIP)

# Which swapchains get the overlay's colours re-encoded: exactly the two
# implemented HDR spaces, and nothing else -- HLG above all, whose encode is
# not implemented and must not inherit PQ's on the grounds that both are HDR.
vocem_test(hdr_mode
    SOURCES hdr_mode.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32)

# The shim's dlsym, at both the architectures the install ships.
#
# The shim interposes dlsym for the whole session, so the symbol version it looks
# the real one up by has to be right on every architecture built -- and the 32-bit
# libraries are configured in a tree of their own with tests switched off, so a
# version that resolved on x86-64 and not on i386 shipped and took the Steam client
# down with it. Hence a test built twice, out of one source, and run against the
# preloaded shim rather than against a copy of its logic.
# SKIP, because the ANGLE half of this file cannot be walked without
# libEGL.so.1 and a half-run is not a pass: the probe exits 77 and says so
# (entry 123's rule, which this file was the last escape from).
vocem_test(shim_dlsym INCLUDE "${CMAKE_SOURCE_DIR}/gl/src" LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} SKIP)

# The 32-bit half, which is the one that failed. Built here with -m32 rather than
# taken from the separate 32-bit tree, so `ctest` in an ordinary build covers it;
# where the toolchain cannot produce a 32-bit binary the twins report themselves
# skipped instead of passing without having run (VOCEM_HAVE_M32 is asked once, in
# the top-level CMakeLists.txt, because common/ builds the 32-bit twin of the
# shared objects on the same answer).
if(VOCEM_HAVE_M32)
    # The same flags gl/CMakeLists.txt gives the shipped shim, or this twin is
    # not the shipped shim: without -fno-threadsafe-statics a function-local
    # static added to the shim passed here and was caught only by the
    # package-time readelf.
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

# The *drawing* at 32 bits, which until 0.1.3 nothing ran. Both of these were
# compiled at -m32 in build32 and exercised only at 64, which entries 30, 33
# and 34 all say is not the same claim: the layout arithmetic is floats and
# ints whose widths differ, the font file read turns an off_t into a size_t,
# and a game under Proton is very often the 32-bit binary. Same sources, same
# definitions, one flag apart -- so a difference that appears here is a
# difference the architecture made and nothing else.
vocem_m32_twin(panel_geometry)
vocem_m32_twin(system_font)

# Two dispatch functions with the same name, one process -- the Electron topology
# that -42 froze on. The stub plays ANGLE; the system's libEGL plays itself. See
# the test's own header for the sequence and the two ways it failed before.
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

# One place says which symbol version dlsym has. The first time that version was
# wrong it was fixed in the shim and left alone in libvocem_gl.so, and the second
# copy went on quietly blanking every 32-bit game -- so the duplication itself is
# what this checks, since getting the value wrong is invisible from outside.
vocem_script_test(one_dlsym_version)

# The vocem_gl_* entry points: the names the shim resolves against the symbols
# the built library exports, both directions, both widths when both are built.
# A rename on one side is entry 38's failure (loads, hooks, never draws, never
# says why) and nothing else relates the two files. The 32-bit library is named
# here and looked for when the test runs (VocemTest.cmake, VOCEM_BUILD32_DIR).
vocem_script_test(gl_entry_points ARGS
    "-DLIBRARY=$<TARGET_FILE:vocem_gl>"
    "-DLIBRARY32=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# The RPC probe's own client across a pause at a frame boundary and one inside
# a frame: both frames, in order, and no close before the peer's (DESIGN 206).
vocem_script_test(rpc_probe_frames)

# Every package a test was refuted against is on the list a clean-up checks
# before deleting anything (packaging/EXEMPLARS.md, DESIGN 205).
vocem_script_test(exemplars_listed)

# The layer's exports, the same rule (entry 16): the three names the loader
# asks for, nothing else, at both widths (layer_exports.cmake says what
# escaped a build without LTO before a version script stated the list).
vocem_script_test(layer_exports ARGS
    "-DLIBRARY=$<TARGET_FILE:vocem_vk>"
    "-DLIBRARY32=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# A crash is reported inside the window's Debug section, and nowhere else: the
# dead process's journal appears in the page, and no separate crashReport
# window exists any more (it was built, fixed to be genuinely top-level -- the
# crash_window_alone test's era -- and then removed on the owner's judgement;
# DESIGN entry 60). Uses the same harness window line that test introduced.
if(TARGET vocem-config)
    set(VOCEM_WINDOW_ARG "-DCONFIG_BINARY=$<TARGET_FILE:vocem-config>")
    vocem_script_test(debug_section ARGS ${VOCEM_WINDOW_ARG} "-DQML_DIR=${CMAKE_SOURCE_DIR}/gui/qml")

    # The Debug section reads journals and only journals: a symlink planted in
    # the journal directory was followed by the unattended four-second scan and
    # its contents shown in the window. Refused at the scanner (lstat: a journal
    # is a regular file) and at the reader (canonical paths, a size cap).
    vocem_script_test(journal_paths ARGS ${VOCEM_WINDOW_ARG})

    # "Drawing now" follows the world while the window is open: the live list
    # was computed once, in the constructor, so a window opened before a game
    # started said nothing was drawing while a game drew 28223 frames.
    vocem_script_test(live_while_open ARGS ${VOCEM_WINDOW_ARG})

    # The message preview may only promise what the daemon will publish: its
    # example body follows notification_show_body. The preview drew a body
    # unconditionally while the switch had no interface, and the owner read the
    # difference between the promise and the game as text failing to draw.
    vocem_script_test(toast_preview_honest ARGS ${VOCEM_WINDOW_ARG})

    # A switch that empties the overlay has to be visible in the window's own
    # picture of the overlay. `notificationsEnabled` dimmed its box from the
    # start and `panelEnabled` was read by the Switch that sets it and by
    # nothing else, so the Panel map and the live preview drew a panel at full
    # strength while the game drew none. Two runs, the dump's own `opacity`.
    vocem_script_test(preview_switch_honest ARGS ${VOCEM_WINDOW_ARG})

    # The preview quiets the picture of whoever is not talking, from the same
    # setting the overlay reads: an opacity no rectangle can see, so the dump
    # prints it. Against 0.1.9's window there is no such figure and this fails.
    vocem_script_test(preview_idle_picture ARGS ${VOCEM_WINDOW_ARG})
endif()

# One place turns pixels back into a position fraction. The wrong line was written
# three times -- the old inverse survived in the drag handler, was fixed there, and
# survived again in the release handler, which shipped. The rule is about the
# duplication, as entry 33's is.
vocem_script_test(one_placement_inverse)

# The emoji bank is a Modified Version of Noto Color Emoji under the OFL 1.1
# and shipped through 0.1.0-52's sources with no licence text anywhere; the
# licence now lives beside the bank and installs with it, and this holds both.
vocem_script_test(emoji_bank_licence)

# The same rule for the project's other one-place facts: the monotonic clock
# (vocem/clock.h), the mkdir -p loop (vocem/paths.h) and the curl write
# callback (daemon/src/curl_sink.h) each existed in several hand-written
# copies before the consolidation sweep; this keeps a new copy from growing
# back quietly.
vocem_script_test(one_spelling)

# A slider's bounds and the settings file's clamp are the same fact spelled
# twice; one pair had drifted ("Show for" offered 1-20 over a clamp of 1-30,
# so a slider touched at 30 silently rewrote the setting). Every SliderRow and
# SpinRow bound to a config value is held to config.h's clamp for the same key.
vocem_script_test(slider_bounds)

# The two PKGBUILDs build one package: build(), package(), depends and
# makedepends held to each other with the two files' legitimate differences
# normalised away. Entry 76 is what a drift cost once; the tagged file
# compiling the whole test suite for every stranger is what it cost again.
vocem_script_test(pkgbuild_agree)

# The third artefact, which nothing had ever read: the Flatpak extension's
# manifest. Its post-install list is the whole of what it carries besides the
# libraries, because both of its modules pass -DVOCEM_BUILD_HOST_TOOLS=OFF and
# that is what every data and licence install rule in CMakeLists.txt is guarded
# by. The published 0.1.9 extension shipped the emoji bank without its sequence
# table -- 0.1.9's one announced fix, absent in every Flatpak game -- and six
# OFL fonts and Dear ImGui with none of their licence texts. Both requirements
# are derived from the tree rather than listed in the test.
vocem_script_test(flatpak_extension_payload)

# The two ways the shim reaches a process, held to one path (entry 16). It
# configures a tree at a prefix of its own on purpose: at the default prefix
# vocem-run's hardcoded /usr and environment.d's @CMAKE_INSTALL_PREFIX@ agree
# by accident, so the comparison anybody would make on this machine passes
# against the defect. About a second; nothing is built.
vocem_script_test(one_preload_path TIMEOUT 300)

# Nothing in the repository root that is neither tracked nor ignored. `git
# status` cannot see a directory with no files in it, and six of those --
# named after this project's own environment variables and Steam runtime path
# fragments, so an ad-hoc command word-split a line -- were carried for three
# days. Entry 137 caught the version that has files in it, because that one
# git status shows.
vocem_script_test(repo_root_clean)

# The daemon's unit: confined, and confined in ways that actually start. An empty
# CapabilityBoundingSet on a user service fails to start (exit 218, measured), so
# its absence is asserted alongside the directives that must be there.
vocem_script_test(unit_hardening)

# Anything that turns a 64-bit value into text has to produce the same text in the
# 32-bit half of the install as in the 64-bit one, because both talk to the same
# 64-bit daemon and look for the same files. Built twice for that reason; the 32-bit
# build is the one that has caught things (its twin is further down, where it was).
vocem_test(widths)

# The DRM reader that decides how large the overlay is, against a /sys tree
# built to order rather than whatever the build machine has plugged in.
vocem_test(display_height)

# The CLI's --watch across a daemon restart: it read its mapping and nothing
# else, so it showed the dead daemon's last state for ever. The first test
# the CLI has had. Private /dev/shm; skips where bwrap is missing.
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

# The overlay must draw for a game whose GL arrives by dlopen(RTLD_LOCAL) --
# Minecraft, GLFW, LWJGL, SDL: the level-3 door. The library shipped in -44
# resolved its own GL lookups through scopes that are empty in exactly those
# games, and the overlay drew nothing while logging that it was drawing. Needs a
# display and bwrap; skips where either is missing. Opens a small window for a
# moment, which is the honest cost of measuring pixels.
# -rdynamic: the probe interposes glXMakeCurrent for the overlay's RTLD_DEFAULT
# lookups (its header says how), and only an exported definition is found.
vocem_test(gl_draw_local LINK ${CMAKE_DL_LIBS} X11 rt LINK_OPTIONS -rdynamic
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# The same game destroying a helper context beside the one the overlay draws
# in. The library shipped 0.1.7 tore its backend down for any context's death
# and made the dying context current to do it (gl_draw_local.cpp's header).
vocem_test_run(gl_second_context TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=second-context" SKIP)

# Somebody joins six times with a fresh colour emoji, then a message arrives:
# the owner's half-second freeze as a scene on the OpenGL door. "font atlas
# built" must read 1 and "folded" once per arrival (entry 191); against the
# 0.1.10-5 package's library it reads 8 builds. The longest swap after each
# arrival is printed, not asserted.
vocem_test_run(gl_arrivals TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=arrivals" SKIP)

# A second window with an unshared context PRESENTS (gl_draw_local.cpp's
# header): the backend's object names belong to the first context, and in the
# second they are that context's own textures. Nothing drawn there, no GL error
# left there, its textures untouched through a fold and a whole upload, and the
# first window drawing again after it.
vocem_test_run(gl_two_windows TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=two-windows" SKIP)

# The game going away while the first atlas is still being rasterised on its
# worker (entry 192): release() and the library's destructor must wait for it.
# A guard on the worker's hazard; a library without one passes trivially.
vocem_test_run(gl_early_exit TARGET gl_draw_local
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_GL_SCENARIO=early-exit" SKIP)

# What a GL CONTEXT costs, which gl_noop_quiet (per frame) and apps_cost (per
# ask) leave uncovered. A game that destroys the context it was drawing in and
# makes another crashed 0.1.10-1 on the second one -- the fonts module's dead
# band could not tell a fresh atlas from its own -- and paid the whole 133 ms
# rasterisation again per context. Four EGL contexts, no window; the log's own
# "font atlas built" lines are the count.
vocem_test(gl_context_cycle LINK ${CMAKE_DL_LIBS} EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP TIMEOUT 120)

# Quit means quit inside the game too. The daemon stops, the overlay leaves the
# screen -- and used to go on holding its atlas, its GL objects and a texture
# per face for the rest of the process's life: 139.7 MB against 22.3 MB without
# the overlay, unchanged 1200 presents after the segment was unlinked. The probe
# reads its own Pss on both sides of the quit.
vocem_test(gl_daemon_gone LINK ${CMAKE_DL_LIBS} EGL GLESv2 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP TIMEOUT 120)

# The same probe in a process the overlay declines, which is what plasmashell
# and every other EGL client of the session is: no backend, no atlas, and one
# verdict however many contexts go by.
vocem_test_run(gl_context_cycle_quiet TARGET gl_context_cycle
    ENV ${VOCEM_GL_PROBE_ENV} "VOCEM_GL_SCENARIO=declining" SKIP TIMEOUT 120)

# Both at 32 bits, against build32's own libraries. Entries 144, 145 and 146
# were all written at one width, in a project whose own law is that the 64-bit
# build passing is not evidence about the 32-bit half (entries 30/33/34) -- and
# the atlas these two are about is the largest thing the injected code holds,
# with a pointer and a size_t in every one of its bookkeeping structures.
vocem_m32_twin(gl_context_cycle
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")
vocem_m32_twin(gl_daemon_gone
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# The overlay's colours in a game that writes sRGB. GL_FRAMEBUFFER_SRGB makes the
# hardware encode whatever the shader writes, and ImGui's colours are encoded
# already: the panel's surface came out lifted, white and saturated colours
# unchanged, which is why it hid. The same measurement the Vulkan half makes
# through the swapchain's format (hdr_srgb_pixels), on the switch this side has.
vocem_test(gl_srgb_write LINK ${CMAKE_DL_LIBS} X11 rt vocem_common
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# A process the overlay declines must be quiet on the X wire: the present hook
# used to pay two glXQueryDrawable X round trips per frame in every GL process
# of the session, before asking whether it would draw at all. Compares X request
# serials between a disabled child and a declining one; the -50 library shows
# +3 requests per frame, the fixed one 0. Needs a display and real GLX.
vocem_test(gl_noop_quiet LINK GL X11 ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# A toast with nobody in a voice channel: the frame guard used to ask only
# about the channel, in both injection paths, which made build_notification
# unreachable in exactly the situation a "somebody wrote to you" toast is for.
# Same miniature dlopen-local game as gl_draw_local, publishing a fresh
# notification and an empty channel; fails against the library shipped in -50.
vocem_test(gl_toast_alone LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# Colour emoji all the way to a real framebuffer, which emoji_atlas cannot claim:
# it proves the bank reaches the atlas, and everything between the atlas and the
# screen is the library. Differential rather than a colour detector -- the same
# scene twice, bank reachable and bank absent -- because the speaking ring is
# green and the muted badge is red, so a colour filter on a capture lies (DESIGN).
# Point VOCEM_GL_LIBRARY at a packaged file and this measures what ships.
vocem_test(gl_emoji_colour LINK ${CMAKE_DL_LIBS} X11 rt
    ENV ${VOCEM_GL_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} SKIP)

# What a drawn frame costs the filesystem on the avatar path -- where entry 49's
# filter stopped. Counting rather than killing, because this path is SUPPOSED to
# touch files: the probe interposes the file syscalls in its own executable (a
# definition in the main program wins over the dlopened library's lookups) and
# reports per-frame numbers across the three phases a game actually goes through:
# nobody's picture arrived yet, the pictures land, everything resolved.
vocem_test(gl_avatar_quiet LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# The upload reads its pixels through the game's GL_UNPACK_* state and its
# pixel-unpack buffer, which the overlay neither neutralised nor restored: a
# miniature game leaves a hostile one bound and asks GL afterwards what it finds.
vocem_test(gl_unpack_state INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK ${CMAKE_DL_LIBS} X11 rt
    ENV ${VOCEM_GL_PROBE_ENV} SKIP)

# The segment's layout at both widths, and crossed: the daemon is 64-bit and every
# 32-bit game reads what it wrote, so the struct's layout IS the ABI -- and it did
# not hold. `User` starts with a uint64_t, whose alignment is 8 on x86-64 and 4 on
# i386, so `users` began at 96 in the daemon and at 92 in every 32-bit game. The
# earlier 32-bit probes could not see it: they published with a writer of their own
# width, so writer and reader were wrong together. Here each width writes a segment
# and the *other* reads it.
vocem_test(shared_state_layout)
vocem_m32_twin(shared_state_layout)
if(VOCEM_HAVE_M32)
    # 64 writes, 32 reads -- the daemon-to-Isaac direction, which is the one that
    # was broken -- and then the other way round for symmetry. Each reader
    # REQUIRES its writer as a fixture, so the file it reads is the one this run
    # wrote: with DEPENDS, which only orders tests that are both selected,
    # `ctest -R cross_read32` read a segment64.bin left by an earlier build and
    # passed against a writer whose layout had moved since.
    vocem_test_run(shared_state_cross_write64 TARGET shared_state_layout
        ARGS write "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin" SETUP segment64)
    vocem_test_run(shared_state_cross_read32 TARGET shared_state_layout32
        ARGS read "${CMAKE_CURRENT_BINARY_DIR}/segment64.bin" REQUIRES segment64 SKIP)
    vocem_test_run(shared_state_cross_write32 TARGET shared_state_layout32
        ARGS write "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin" SETUP segment32)
    vocem_test_run(shared_state_cross_read64 TARGET shared_state_layout
        ARGS read "${CMAKE_CURRENT_BINARY_DIR}/segment32.bin" REQUIRES segment32 SKIP)
endif()

# A face on screen, at both widths, through the GL path -- because the width that
# has failed invisibly three times (entries 30, 33, 34) is the one the owner's
# Isaac runs at: its record says api = opengl and wine-preloader i386-unix. The
# 64-bit build is the control; the 32-bit build is the question. Both use the
# libraries of their own width, so the shim, the heavy library and the probe agree
# about what a pointer is. The 32-bit libraries are build32's, looked for when the
# test runs.
vocem_test(gl_avatar_width LINK ${CMAKE_DL_LIBS} X11 rt ENV ${VOCEM_GL_PROBE_ENV} SKIP)
vocem_m32_twin(gl_avatar_width
    ENV "LD_PRELOAD=${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "VOCEM_SHIM_PRELOADED=1"
        "VOCEM_GL_LIBRARY=${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so"
    BUILD32 "${VOCEM_BUILD32_DIR}/gl/libvocem_gl_shim.so" "${VOCEM_BUILD32_DIR}/gl/libvocem_gl.so")

# Beside Steam's own overlay, in both orders, with the log-line witness. The
# raw-pixel versions of these two tests (gl_beside_steam / gl_under_steam) were
# the trap gl_beside_mangohud.cmake describes: they counted what ANY interposer
# drew, so they passed whether Steam's library drew, was missing, or the cached
# find_file path had gone stale after an uninstall. One script now, both
# orders, asserting exactly what DESIGN's open-risks entry records as measured:
# we draw in both orders, so Steam forwards down the chain. Whether Steam's own
# overlay drew stays unmeasured and said (a probe is not a Steam title).
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

# The OpenGL half inside gamescope, headless. Answers a claim that arrived from
# outside and was believed here for a day -- that gamescope strips LD_PRELOAD and
# therefore takes this half away in every gamescope session. It strips one named
# library (Steam's), not the variable: measured, the shim loads and the same 1058
# pixels come out. Pinned in the direction it was measured, because gamescope's
# own WSI layer sits where our present hook does and this is the neighbourhood a
# gamescope update can reach us through.
find_program(VOCEM_GAMESCOPE gamescope)
if(VOCEM_GAMESCOPE)
    # LOCK gamescope: two nested headless sessions at once is the one
    # contention the parallel suite showed (DESIGN 193) -- the two gamescope
    # tests started in the same second of a -j16 run and one attempt hung to
    # its 180 s cap before the retry went through.
    vocem_script_test(gl_inside_gamescope LOCK gamescope ARGS ${VOCEM_GL_PROBE_ARGS}
        "-DGAMESCOPE=${VOCEM_GAMESCOPE}")
else()
    vocem_skip(gl_inside_gamescope "gamescope is not installed here")
endif()

# The layer manifest's shared name and its bare soname, which hold each other up:
# the loader discards one of the two manifests (measured on the host AND inside
# pressure-vessel, which is where it matters), and the survivor only serves both
# architectures because the library is named by soname. Changing either alone
# kills 32-bit games silently with the 64-bit half still passing -- entries
# 30/33/34's shape. The script's header carries the whole argument, including why
# copying MangoHud's per-architecture names would be the defect here rather than
# the fix. The 32-bit manifest is build32's, looked for when the test runs.
vocem_script_test(layer_manifest_name ARGS
    "-DMANIFEST=${CMAKE_BINARY_DIR}/layer/install/VkLayer_vocem_overlay.json"
    "-DMANIFEST32=${VOCEM_BUILD32_DIR}/layer/install/VkLayer_vocem_overlay.x86.json"
    "-DLAYER_CMAKE=${CMAKE_SOURCE_DIR}/layer/CMakeLists.txt")

# And the same pair inside Steam's container, which is where the collapse is a
# documented defect for other projects and where the reasoning above had never
# been executed. One instance per width, no window, and the witness is the path
# the process mapped -- an enumerated layer NAME says nothing about which ABI
# arrived. Measures the installed layer, because the claim is about the installed
# pair of manifests; skips without the sniper runtime or without an installation.
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

# The avatar upload's command buffer through the loader contract: a dispatchable
# object a layer creates is registered with pfnSetDeviceLoaderData or dispatch
# on it crashes (layer rule 5). The TextureCache allocated one under a comment
# claiming the renderer had registered it, and nothing had. Runs the real
# TextureCache against a stub Vulkan supplied through its own resolver seam.
# vocem_common for the shared logger the cache speaks through (nothing else of
# it is pulled: the logger is an object of its own).
set(VOCEM_TEXTURE_CACHE_INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
vocem_test(texture_loader_data
    SOURCES texture_loader_data.cpp "${PROJECT_SOURCE_DIR}/layer/src/texture_cache.cpp"
    INCLUDE ${VOCEM_TEXTURE_CACHE_INCLUDE} DEFINES IMGUI_USE_WCHAR32 LINK vocem_common)

# The avatar cache against its descriptor budget: AddTexture on an exhausted
# pool updates an uninitialised descriptor set and the driver dies on it --
# the Minecraft crash of 2026-07-29, symbolised out of the core dump. The
# cache must stop at its budget and hand out placeholders beyond it.
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

# And it reaches it WITHOUT rasterising the atlas again -- the measurement
# behind "the game freezes for half a second when somebody joins". A count and
# not a clock (entry 145's reason). Against the module as it stood every new
# emoji is another 125-145 ms build, and the count check fails on the first one.
vocem_test(emoji_incremental IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)

# The decoration symbols a server names its channels with have glyphs, pinned
# one per added block, both weights. "?? ... ??" in a toast is what a miss
# looks like from inside a game; the file's header carries the measured
# corpus numbers.
vocem_test(symbol_coverage IMGUI LINK vocem_common)

# The frame path's silence, held by a seccomp filter rather than by reading:
# fonts_note_emoji runs inside the present on both paths, and what makes that
# legal is that every codepoint costs its bank lookup exactly once -- rejections
# remembered as well as hits. The layer's comment used to claim "no I/O at all",
# which was never quite the property it had.
vocem_test(fonts_frame_quiet IMGUI LINK vocem_common ENV ${VOCEM_EMOJI_BANK_ENV} SKIP)

# The daemon's decode, which is the project's trust boundary: bytes from the
# internet against the only parser in the install. It counts what stb's
# allocator is asked for, because both defects it holds -- a decompression bomb
# inflated in full before the size check refused it, and a migration read that
# followed a symlink to /dev/zero and never returned -- are about cost, and a
# test comparing booleans passes against both.
# The vendored decoder compiles helpers this build does not call; the daemon's
# own CMakeLists.txt silences the same warning for the same reason.
find_package(ZLIB REQUIRED)
set(VOCEM_DAEMON_INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" "${CMAKE_SOURCE_DIR}/third_party")
vocem_test(avatar_decode INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function LINK ZLIB::ZLIB SKIP)

# The cache's memory against the peer that feeds it: request() must stop
# remembering at the ceiling, because the notification path reaches this
# container with no bound of its own and the unit carries MemoryMax=128M.
# Compiles the real avatars.cpp; the worker is stopped before the flood so no
# network is touched and the reading is the same twice.
find_package(CURL REQUIRED)
vocem_test(avatar_bound
    SOURCES avatar_bound.cpp "${CMAKE_SOURCE_DIR}/daemon/src/avatars.cpp"
    INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function
    DEFINES VOCEM_VERSION="${PROJECT_VERSION}"
    LINK CURL::libcurl ZLIB::ZLIB Threads::Threads)

# The faces must not change when the decoder does. The daemon decoded with
# stb_image up to 0.1.0-53 and with Wuffs after it; every other test is blind to
# that swap, because the cache file has the same name and length whichever
# decoder wrote it -- a decoder that swapped red for blue would pass all of them.
# This one holds the new decoder to the picture itself AND to the old decoder,
# byte for byte, across six colour types.
vocem_test(avatar_decoders_agree INCLUDE ${VOCEM_DAEMON_INCLUDE}
    COMPILE_OPTIONS -Wno-unused-function LINK ZLIB::ZLIB SKIP)

# The avatar cache format, from both ends: the daemon's writer (resample and
# rename into place) and the games' reader, whose strictness -- exactly one
# size or nothing -- is the entire format now that no image parser runs inside
# a game process. Built at both widths, beside the paths.
vocem_test(avatar_rgba)
vocem_m32_twin(avatar_rgba)

# What a message becomes by the time a game can read it: the real vocemd, driven
# by a stub Discord RPC, with the segment read back the way a game reads it. The
# body of a message stays out of the segment unconditionally since entry 64 --
# the words travel through the note segment instead, and the test asserts the
# segment's body field empty with no setting in the sentence (the "unless the
# settings say otherwise" that used to stand here described entry 58's switch,
# which 64 removed). The daemon as shipped in 0.1.0-43 published the body
# unconditionally, and this fails against that binary. Sandboxed under bwrap
# (private /dev/shm, private network) and skipped where bwrap is missing.
if(TARGET vocemd)
    set(VOCEM_DAEMON_ENV "VOCEM_DAEMON=$<TARGET_FILE:vocemd>")
    vocem_test(daemon_notification LINK rt ENV ${VOCEM_DAEMON_ENV} SKIP)

    # What the daemon does when the port answers and the session does not. The
    # backoff covered "nothing is listening" and not "something took the
    # connection and dropped it", which is what Discord's own client does to an
    # RPC connection it will not serve: measured against the packaged 0.1.3-7
    # daemon, 2215 connections in five seconds against 3. Same sandbox as above.
    vocem_test(daemon_reconnect ENV ${VOCEM_DAEMON_ENV} TIMEOUT 120 SKIP)

    # The same peer, one step further in: it authenticates the session and then
    # drops it. The pause was made conditional on NOT having authenticated, so
    # this path had no sleep at all -- measured against the installed 0.1.10-2
    # daemon, 5166 and 4381 connections in five seconds, where the pause makes
    # 6; and as in entry 103 the ceiling is the stub's, not the daemon's.
    # Discord restarting is this shape.
    vocem_test_run(daemon_reconnect_authenticated TARGET daemon_reconnect
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_RECONNECT_SCENARIO=authenticated"
        TIMEOUT 120 SKIP)
endif()

# What the daemon takes out of a name (daemon/src/text.h): the directional
# marks of entry 66 and, since entry 133, the control characters -- a newline
# in a nickname was a second row in the panel and a second line in the journal.
# Header-only, so the rule is tested alone rather than through the daemon.
vocem_test(daemon_text INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src")

# The token file, read the way the avatar cache reads its files (entry 47) and
# written through a temporary: a link to /dev/zero was read until memory ran
# out, a FIFO held the daemon at its first line, and a byte that is not UTF-8
# ended it on the first READY (entry 133). Compiles auth.cpp directly; the
# token exchange in it needs libcurl linked and is never called.
if(TARGET vocemd)
    vocem_test(token_file
        SOURCES token_file.cpp "${CMAKE_SOURCE_DIR}/daemon/src/auth.cpp"
        INCLUDE ${VOCEM_DAEMON_INCLUDE}
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}"
        LINK CURL::libcurl TIMEOUT 60)

    # How long the segment stays published after SIGTERM with a download
    # stalled: the join came before the unlink and the transfer had no way
    # out, so systemctl stop ended in SIGKILL with the segment still there
    # (entry 134). Built stall (a quiet https_proxy), printed seconds,
    # daemon_notification's sandbox.
    vocem_test(daemon_stop_unlinks INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 150 SKIP)

    # The same stop arriving while the daemon is inside a handshake rather than
    # inside a transfer. A peer that accepts and never answers the upgrade is
    # one the daemon is right to wait for -- it has a ten-second absolute
    # deadline for it (entry 77) -- and it waited the whole of it with `g_stop`
    # already set, against a unit whose TimeoutStopSec is ten. The class had
    # been repaired four times in four functions (entries 72, 77, 102, 112) and
    # this was the instance left: daemon_ws_bounds cannot reach it, because its
    # stub answers.
    vocem_test_run(daemon_stop_in_handshake TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=handshake" TIMEOUT 150 SKIP)
    # And the handshake answered a byte every tenth of a second: the stop flag
    # was read only when a slice expired empty, which a trickle never lets
    # happen, so the stop waited out the handshake's ten seconds (entry 196).
    vocem_test_run(daemon_stop_in_trickle TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=trickle" TIMEOUT 150 SKIP)

    # And what a CLEAN stop leaves in a Flatpak sandbox. `bridge.stop()` emptied
    # the list of mirrors before `note().clear()` fired the hook that unlinks
    # the mirrored words, so a message's text stayed in the sandbox until
    # logout -- after the tray's Quit, which is the one stop with no later
    # daemon to clean up after it. vocem/note.h promises the opposite; the two
    # halves are each covered in flatpak_bridge.cpp and never in this order.
    vocem_test_run(daemon_stop_note_mirror TARGET daemon_stop_unlinks
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_DAEMON_STOP_SCENARIO=note-mirror" TIMEOUT 150 SKIP)

    # The same stop with the token exchange stalled instead of a download: the
    # one transfer on the daemon's own thread had no way out, so SIGTERM waited
    # for libcurl's connect timeout -- the unit's whole TimeoutStopSec -- with
    # the segment published. The flag is consulted from the transfer now, as
    # the avatar worker's is (entry 134); printed seconds, the same sandbox.
    vocem_test(daemon_exchange_stop INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 150 SKIP)
endif()

# Who the daemon believes is on the other end before it sends a token with
# messages.read scope. Three cases: the real /proc for a connection this process
# owns, a dual-stack listener whose row lives in /proc/net/tcp6 (a case that
# answers Unknown if only /proc/net/tcp is read, which is every ordinary Discord
# client), and a hand-written row owned by somebody else.
vocem_test(peer_identity INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src")

# The one place in this project that reads bytes off a socket, held against a peer
# that is not Discord. Compiles the daemon's WebSocket client directly and talks to
# it from a stub on an ephemeral loopback port: no daemon, no /dev/shm, no 6463,
# so it needs neither bwrap nor a sandbox.
vocem_test(daemon_ws_bounds
    SOURCES daemon_ws_bounds.cpp "${CMAKE_SOURCE_DIR}/daemon/src/websocket.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" TIMEOUT 120)

vocem_m32_twin(widths)

# The HDR pipelines executed on the GPU: the shipped SPIR-V through the real
# hdr_pipeline_create (compiled in, not copied), a quad of known sRGB colours
# drawn to an offscreen fp16 target on a surfaceless device, pixels read back
# against references recomputed from the standard constants -- the half of the
# HDR open risk that the arithmetic replication could not reach. No window, no
# WSI; skips 77 where no Vulkan loader or device exists.
vocem_test(hdr_pixels
    SOURCES hdr_pixels.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32 LINK ${CMAKE_DL_LIBS} SKIP)

# The same source against an sRGB-format target, which is the ordinary swapchain
# and not an HDR one: there the format carries the encoding, the hardware applies
# linear->sRGB to whatever the shader writes, and the claim is a round trip --
# what the attachment stores is the byte ImGui asked for. Against the shader
# before mode 3 existed it stores the sRGB encode of an already-encoded value:
# measured 128 -> 188, and the panel's own 79,84,92 -> 151,155,162.
vocem_test(hdr_srgb_pixels
    SOURCES hdr_pixels.cpp "${PROJECT_SOURCE_DIR}/layer/src/hdr_pipeline.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/layer/src" "${IMGUI_DIR}"
    DEFINES IMGUI_USE_WCHAR32 VOCEM_SRGB_TARGET LINK ${CMAKE_DL_LIBS} SKIP)

# The Vulkan present hook, on a real swapchain. Nothing in here ever created one
# before: hdr_pixels reaches a device and measures the colour arithmetic *inside*
# the draw on a surfaceless device, so vocem_QueuePresentKHR -- the entry point
# the whole Vulkan half hangs from -- had no test at all, which entries 70, 88
# and 114 each say in their own words. The probe presents 45 frames to an
# off-screen override-redirect window, reads one back and requires pixels it did
# not paint, with a control pass (VOCEM_DISABLE=1, no layer in the chain) to say
# what zero looks like. VOCEM_VK_MANIFEST is the build tree's manifest and
# VOCEM_VK_LIBRARY the library it must resolve to: both manifests carry the same
# layer name by design, so a run can silently measure the *packaged* layer, and
# the probe reads /proc/self/maps rather than trusting the loader's choice.
# Skips 77 without a display, a loader, a presentable device or a readable
# surface format.
set(VOCEM_VK_MANIFEST_ENV "VOCEM_VK_MANIFEST=${CMAKE_BINARY_DIR}/dev/VkLayer_vocem_overlay.json")
set(VOCEM_VK_PROBE_ENV ${VOCEM_VK_MANIFEST_ENV} "VOCEM_VK_LIBRARY=$<TARGET_FILE:vocem_vk>")
vocem_test(vk_present_draw SOURCES vk_present_draw.cpp vk_file_witness.cpp
    LINK ${CMAKE_DL_LIBS} X11 rt
    LINK_OPTIONS "-Wl,--export-dynamic-symbol=open,--export-dynamic-symbol=open64"
                 "-Wl,--export-dynamic-symbol=openat,--export-dynamic-symbol=openat64"
                 "-Wl,--export-dynamic-symbol=pread,--export-dynamic-symbol=pread64"
                 "-Wl,--export-dynamic-symbol=close"
    ENV ${VOCEM_VK_PROBE_ENV} SKIP)

# The same binary as its own control: VOCEM_DISABLE=1 and the loader refuses the
# layer on its disable_environment, so the presented frame must be exactly what
# the probe painted -- nothing mapped, zero foreign pixels. Two claims in one
# run: that the count above belongs to this overlay rather than to anything else
# on the machine that draws into a swapchain, and entry 35's promise measured on
# the Vulkan side, which nothing did before. Runs the scene independently rather
# than as a child of the measuring pass: a forked control inherits the private
# /dev/shm, which by then holds `vocem-<uid>` published by its parent, and
# private_shm.h refuses -- correctly -- because that is the name the live daemon
# uses.
vocem_test_run(vk_present_disabled TARGET vk_present_draw
    ENV ${VOCEM_VK_MANIFEST_ENV} "VOCEM_DISABLE=1" SKIP)

# What the layer asks of the chain, watched from BELOW it. This machine has no
# Khronos validation layer, and pixels cannot see a queue wait inside a present
# (rules 8 and 10), a fence submitted while still signalled, or ImGui's stock
# pipeline bound in a render pass it was not built against -- the three faults
# the layer shipped 0.1.7 with, found by reading (DESIGN entry 131). The
# witness is a layer of its own (vk_witness_layer.cpp) that records those
# events to a file; the probe puts it under the overlay through the loader's
# override meta-layer (a file name decides nothing, measured), and the
# report's own positive control -- a pipeline being created, which only the
# overlay does -- says whether it really sat below.
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

# The swapchain recreated in another format, both directions. The stock ImGui
# pipeline is built against the FIRST swapchain's render pass; a later one in
# another format needs a pipeline of its own, and the witness is what tells
# the two apart -- the pixel count is the same either way on this driver.
vocem_test_run(vk_present_recreate TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=recreate" SKIP)
vocem_test_run(vk_present_recreate_from_srgb TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=recreate" "VOCEM_VK_FORMAT_FIRST=srgb" SKIP)

# The frames chained through semaphores with two in flight, the way a game
# presents: exercises the layer's wait on its own per-image fence and its
# substitution of its own semaphore for the application's, which the idle-
# between-frames loop above never reaches. The witness matches each present's
# wait semaphore to the overlay's own submit.
vocem_test_run(vk_present_in_flight TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=in-flight" SKIP)

# A helper device made and destroyed beside the presenting one. The renderer
# used to be shut down for ANY device destroyed in the process, and rebuilt on
# the next present; the layer's log says "backend ready" once now.
vocem_test_run(vk_present_second_device TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=second-device" SKIP)

# Quit means quit on the Vulkan side too: the daemon's segment is unlinked under
# a drawing game and the layer must hand back everything it holds for it. The
# memory is weighed on the OpenGL half (gl_daemon_gone); here the witnesses are
# the layer's own line, counted, and a frame that is the game's own again.
#
# WITNESSED, and driven by the chained loop rather than the idling one. The
# first version of this test used run_frames, which calls vkQueueWaitIdle after
# every present -- so the GPU was provably finished by the time the layer's
# release executed, and destroying the renderer's images, buffers, pipeline and
# descriptor pool was legal whatever the layer did. It asserted three things
# about bookkeeping and could not have failed for any lifetime reason, which is
# the shape DESIGN 62 is about. The release fires on an arbitrary present of a
# live device; only a wait the layer performs itself makes it safe.
vocem_test_run(vk_present_daemon_gone TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=daemon-gone" SKIP TIMEOUT 180)

# A daemon publishing and nobody in a voice channel: the ordinary state of a
# machine with the tray icon up, and the one the layer used to build its whole
# renderer for -- 133 ms of atlas and 80 MB of pixels for a panel that was never
# going to appear, because `needs_init` was decided from the swapchain and the
# application's verdict alone. "backend ready" must not appear at all.
vocem_test_run(vk_present_idle TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=idle" SKIP)

# Somebody joins, six times, each with a colour emoji nobody in the channel had,
# and then a message arrives with one more: the owner's "the game freezes for
# half a second when somebody joins or a message arrives", as a scene on the
# real layer and a real swapchain. The layer's own lines are the count (entry
# 191): "font atlas rebuilt" must not appear, "folded" must, once per arrival.
# Against the 0.1.10-5 package's library it reads seven rebuilds. The longest
# present after each arrival is printed, not asserted (entries 145, 185).
vocem_test_run(vk_present_arrivals TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=arrivals" SKIP)

# The device, the instance and the process gone while the first atlas is still
# being rasterised on its worker (entry 192): vocem_DestroyDevice and the
# layer's destructor must wait for it. A guard, not a refutation.
vocem_test_run(vk_present_early_exit TARGET vk_present_draw
    ENV ${VOCEM_VK_PROBE_ENV} ${VOCEM_EMOJI_BANK_ENV} "VOCEM_VK_SCENARIO=early-exit" SKIP)

# A Flatpak game with the master switch off, as the daemon hears of it. The
# layer enters the bridge and tells the daemon whether this sandbox is drawing;
# it used to say `allowed` where the GL side said `enabled && allowed`, so the
# daemon went on mirroring the channel and every face into a sandbox showing
# nothing (entry 138). The request file must read drawing=0; against the layer
# shipped in 0.1.8 it reads drawing=1. No state segment reaches the sandbox,
# so the frame is also held to zero foreign pixels.
vocem_test_run(vk_present_flatpak_off TARGET vk_present_draw
    ENV ${VOCEM_VK_PROBE_ENV} "VOCEM_VK_SCENARIO=flatpak-off" SKIP)

# The same frame at 32 bits, against the 32-bit tree. Roadmap phase 4 said the
# Vulkan half had "never been seen loading" at this width; it loads (measured on
# the host and inside Steam's container, vk_layer_in_container), and a frame is
# the other half of the claim -- the layout arithmetic is floats and ints whose
# widths differ, and a Proton title is very often the 32-bit binary. Same source,
# one flag apart, so a difference here is the architecture's and nothing else's,
# which is the widths.cpp pattern entries 30/33/34 bought. build32's manifest and
# library are looked for when the test runs.
set(VOCEM_VK_PROBE32_ENV
    "VOCEM_VK_MANIFEST=${VOCEM_BUILD32_DIR}/dev/VkLayer_vocem_overlay.x86.json"
    "VOCEM_VK_LIBRARY=${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")
vocem_m32_twin(vk_present_draw
    ENV ${VOCEM_VK_PROBE32_ENV}
    BUILD32 "${VOCEM_BUILD32_DIR}/dev/VkLayer_vocem_overlay.x86.json"
            "${VOCEM_BUILD32_DIR}/layer/libvocem_vk.so")

# And its scenarios at 32 bits, which nothing could twin until
# vocem_m32_twin_run existed: the probe had eight scenes at 64 bits and one at
# 32, and every one of them is about lifetimes and bookkeeping in a process
# where a pointer and a size_t are a different size. The witness is left out --
# it is a 64-bit layer -- so these run with the plain 32-bit chain, which is
# what `vk_present_draw32` itself uses, and the same BUILD32 files, so that a
# machine without build32 reads a skip for each of them as it does for the probe.
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

# The same present hook under the Khronos validation layer, where the machine
# has one: the witness knows the handful of calls this project's rules name,
# the real layer knows all of them. Resolved when the test runs; skips out
# loud where vulkan-validation-layers is not installed (this machine, today).
vocem_script_test(vk_present_validated ARGS
    "-DPROBE=$<TARGET_FILE:vocem_vk_present_draw>"
    "-DMANIFEST=${CMAKE_BINARY_DIR}/dev/VkLayer_vocem_overlay.json"
    "-DLIBRARY=$<TARGET_FILE:vocem_vk>"
    "-DWITNESS=${CMAKE_CURRENT_BINARY_DIR}/vk_witness.json"
    "-DEMOJI_BANK=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba")

# The Vulkan half inside gamescope, with gamescope's own WSI layer in the chain --
# the one place a gamescope update can reach us, since its WSI layer replaces
# swapchain creation and present and our hook is on the present path. The
# script's header records why its first version was worthless: vk_present_draw
# builds a deliberately clean chain, and run unchanged under gamescope that
# cleanliness excluded gamescope's WSI too, so it reported a confident success
# about a process merely launched by gamescope.
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

# The Appearance page previews its presets as four real panels built from the
# theme each preset would produce (distinct surfaces, one active mark), and
# Appearance and Spacing carry a live preview of both boxes beside their
# controls. Read out of the geometry dump; against the window before these
# previews existed the preset row was colour chips and neither page had a
# preview column, and this fails there.
if(TARGET vocem-config)
    vocem_script_test(appearance_previews ARGS ${VOCEM_WINDOW_ARG})
endif()

# Which height sizes the atlas, in one spelling for both injected paths. The
# display wins over a window (entry 39) -- but a drawable TALLER than every
# display (supersampling/DSR) is headed for the compositor's downscale, and
# sizing it by the display landed the overlay at 0.5x on screen: the
# illegibly-small direction. Fails against the old per-path ternary, which
# answered the display's height for the supersampled case.
vocem_test(sizing_height INCLUDE "${IMGUI_DIR}" DEFINES IMGUI_USE_WCHAR32)

# The tray's four state icons: present, token-true, well-formed, installed.
# A missing or broken state icon degrades to the generic one with nothing
# logged, which is the silence this suite exists to refuse.
vocem_script_test(tray_icons)

if(TARGET vocem-config)
    # A game running right now with the overlay off is said above the history:
    # the Applications page's answer to the question somebody opens it with
    # mid-session, held on the real window against a record for pid 1's name.
    vocem_script_test(running_without ARGS ${VOCEM_WINDOW_ARG})

    # A map depicting a display the overlay is not sized for shows the boxes at
    # the share of THAT screen they will really cover, and each map's dropdown
    # chooses the display. Fabricated two-display DRM tree via VOCEM_DRM_ROOT,
    # real window offscreen; fails against the window before the dropdown
    # existed (the packaged 0.1.0-59 exemplar), where the key is ignored and the
    # ratio is 1.
    vocem_script_test(preview_display ARGS ${VOCEM_WINDOW_ARG})

    # One padding, in every card, on every page of the settings window, at the
    # default size and at the minimum. Every card carries objectName "card" and
    # every padded block inside one "cardContent", so a card that is taller than
    # the blocks in it is an item sitting in a card without taking the card's
    # padding -- the seam the owner spotted by eye in the Debug section, and
    # which was still on the Applications page in 0.1.0-64. Against that
    # packaged window nothing is named and the check reports it measured no
    # cards at all.
    vocem_script_test(window_padding ARGS ${VOCEM_WINDOW_ARG})
endif()

if(TARGET vocem-config)
    # The window's first frame does not wait on systemctl: the preload probe
    # was a synchronous spawn behind a CONSTANT property the Debug page reads
    # at construction, so a slow service manager was a slow first frame (up to
    # the three-second cap). Differential, against a stub systemctl that sleeps.
    vocem_script_test(window_startup ARGS ${VOCEM_WINDOW_ARG})

    # What an idle window costs, in counts: stateChanged once, not once per
    # tick; the segment and the display tree asked on the sweep; an icon lookup
    # examining the entries that carry its name and not the whole registry.
    vocem_script_test(window_cost ARGS ${VOCEM_WINDOW_ARG})

    # The three instant switches, driven where no window test can reach them.
    # The offscreen harness walks sections and opens popups; it cannot press a
    # Switch, and persistNow() has no other interface -- so the bridge is built
    # directly here, the way daemon_ws_bounds.cpp compiles websocket.cpp and
    # talks to it. It writes on top of the file as it stands, and used to leave
    # the window's own copy at the value it had that morning: the next Apply
    # then wrote over an edit made outside, with the button grey and nothing
    # saying the two differed.
    # The GUI's own dependencies, found here because tests/ is a directory of
    # its own and gui/'s find_package does not reach it -- QUIET and inside
    # `if(TARGET vocem-config)`, so a build without the window never asks. Gui,
    # Qml and fontconfig are what config_bridge.cpp actually reaches; it was
    # linking Quick and Widgets as well, which the probe's own header measures.
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml)
    find_package(Fontconfig QUIET)
    vocem_test(config_bridge_switches
        SOURCES config_bridge_switches.cpp "${CMAKE_SOURCE_DIR}/gui/src/config_bridge.cpp"
        INCLUDE "${CMAKE_SOURCE_DIR}/gui/src"
        LINK vocem_common Qt6::Gui Qt6::Qml Fontconfig::Fontconfig
        DEFINES VOCEM_VERSION="${PROJECT_VERSION}")
    set_target_properties(vocem_config_bridge_switches PROPERTIES AUTOMOC ON)

    # Two launches 100 ms apart are one window: the socket guard asked before
    # the window was built and listened after, so two launches inside that gap
    # both proceeded and the second unlinked the first's socket. A lock file
    # taken before the engine loads decides it.
    vocem_script_test(single_instance ARGS ${VOCEM_WINDOW_ARG})
endif()

# A switch means the click did something. Three settings in this window are
# written the moment they are clicked and reach a running game a couple of
# seconds later; every other boolean waits for Apply, and the KDE guidelines
# call that a checkbox. Ten of them were switches through 0.1.0-64, and this
# fails against those sources.
vocem_script_test(instant_switches)

# An empty view says so with a placeholder message, on every page that has one.
# Measured on the real window with nothing on disk; against 0.1.0-64 the Debug
# section has its placeholders and the Applications page answers the same
# situation with a card holding one row, so this fails on section 5.
if(TARGET vocem-config)
    vocem_script_test(empty_views ARGS ${VOCEM_WINDOW_ARG})
endif()

# Where you are is asked, not remembered, and the names arrive without the
# invisible characters Discord wraps them in. Both are the owner's report from
# 0.1.0-65: moved into another channel by somebody else, the panel went on
# naming the old one; and question marks in the message box, which turned out to
# be U+2068/U+2069 with no glyph in the atlas. The real vocemd against a stub
# Discord RPC, sandboxed like daemon_notification.
if(TARGET vocemd)
    vocem_test(daemon_moved LINK rt ENV ${VOCEM_DAEMON_ENV} SKIP)
endif()

# The System tray section: which picture the panel wears, and a preview that
# really draws it. The second half is the one that earned the test -- with the
# desktop style loaded, QIcon::fromTheme resolves every Breeze name this window
# asks for and none of this application's own six, so the About page had been
# drawing an empty square where its icon goes. The artwork is carried in the
# binary now; this holds the preview to six pictures that actually loaded.
if(TARGET vocem-config)
    vocem_script_test(tray_icon_choice ARGS ${VOCEM_WINDOW_ARG})
endif()

# The Flatpak bridge, both directions.
#
# flatpak_bridge holds the daemon's writer and the injected code's reader against
# each other across a fake $XDG_RUNTIME_DIR/app: the daemon publishes, a forked
# child with FLATPAK_ID set enters the bridge exactly as the overlay does and
# reads back the state, the settings and the avatar -- after unlinking the POSIX
# segment, so nothing it gets right can have come from /dev/shm. A private
# /dev/shm, because a StateWriter is opened.
vocem_test(flatpak_bridge
    SOURCES flatpak_bridge.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK rt vocem_common SKIP)

# The same directory, seen as what it is: writable by a sandbox that the daemon
# has no reason to trust, while the daemon itself runs as the user with the
# user's home reachable. Every name it opens down there is a symbolic link here.
vocem_test(flatpak_bridge_hostile
    SOURCES flatpak_bridge_hostile.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)

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
vocem_script_test(version_agrees ARGS
    "-DCONFIG_BINARY=$<IF:$<TARGET_EXISTS:vocem-config>,$<TARGET_FILE:vocem-config>,>")

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
    vocem_test(daemon_note_expiry LINK rt ENV ${VOCEM_DAEMON_ENV} TIMEOUT 240 SKIP)

    # The same words, with the port walk made slow: three ports that accept and
    # say nothing are thirty seconds of walking, and the tick that retires a
    # message's words sat AFTER the walk while its own comment and DESIGN 112
    # both said "once per port". Measured: 11.0 s with the tick inside the loop
    # -- one handshake deadline -- against 30.0 and 30.1 s against the installed
    # 0.1.10-2 daemon.
    vocem_test_run(daemon_note_expiry_port_walk TARGET daemon_note_expiry
        ENV ${VOCEM_DAEMON_ENV} "VOCEM_NOTE_EXPIRY_SCENARIO=port-walk" TIMEOUT 120 SKIP)
    # Inside the block, which it sat outside of with the indentation of the
    # inside (DESIGN 193): a build without the daemon registered a test whose
    # command does not exist.
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
    vocem_script_test(display_change_while_open ARGS ${VOCEM_WINDOW_ARG})
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
vocem_test(config_long_line)

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
vocem_test(shim_private_dispatch LINK ${CMAKE_DL_LIBS}
    ENV ${VOCEM_SHIM_ENV} "VOCEM_STUB_PRIVATE_EGL=$<TARGET_FILE:vocem_stub_private_egl>" SKIP)

# The tool for the person having the problem, held to printing one answer.
#
# `systemctl is-active` reports a STATE, so its exit status is that state: it
# prints "inactive" and exits 3. `$(... || echo unknown)` therefore ran both
# branches and put two lines in the report, exactly when the daemon is down --
# which is when somebody runs this. Entry 99 is the same fault with `grep -c`,
# five lines further down in the same file, found and fixed while this one stood.
# Hermetic: a stub systemctl earlier on PATH, so the test never asks the owner's
# live daemon anything and needs no skip.
#
# SERIAL: its per-process leg asks the script about `cmake` -- the process
# running this test -- and the script reads every `cmake` it finds with
# pgrep. Beside other script tests under -j, one of those can exit between the
# pgrep and the read of its maps, and the leg then reports "cannot read" and
# fails. Not seen in eight parallel runs; read from the script, and 0.4 s
# alone costs nothing (DESIGN 193).
vocem_script_test(vocem_why SERIAL ARGS "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}")

# Two OpenGL interposers in one process, and which of them draws.
#
# The companion of gl_beside_steam, asking the question that pair cannot: those
# two count OUR pixels, so they pass whether the other overlay survived us or
# not. Against MangoHud -- live on this machine, and the shim it preloads
# exports the same ten names ours does -- the two exclude each other and
# whoever is first in LD_PRELOAD wins. The witness is our own debug log and not
# the pixel count, because with MangoHud first the frame is still full of
# foreign pixels and they are MangoHud's. See the file's header.
find_file(VOCEM_MANGOHUD_SHIM libMangoHud_shim.so
    PATHS /usr/lib/mangohud /usr/lib64/mangohud /usr/local/lib/mangohud
    NO_DEFAULT_PATH)
vocem_script_test(gl_beside_mangohud ARGS ${VOCEM_GL_PROBE_ARGS}
    "-DMANGOHUD_SHIM=${VOCEM_MANGOHUD_SHIM}")

# A bank that has not arrived yet is not a bank that is missing.
#
# The Flatpak candidate added with the /run/host one did not work: the daemon
# copies the bank in on its own one-second tick, the game's first frame beats it,
# and one refused open was remembered for the life of the process. The sandbox's
# shape is built with bwrap rather than described; see the file's header.
vocem_test(emoji_bank_arrives LINK rt
    ENV "VOCEM_TEST_BANK_SOURCE=${CMAKE_SOURCE_DIR}/common/emoji/emoji_bank.rgba" SKIP)

# ---- 0.1.11 common lane ----

# A publish must not blank a game's frame: a writer thread publishes about ten
# thousand self-checking states a second while StatePoll::poll runs a million
# times beside it, in a private /dev/shm. No nullptr after the first read, no
# torn snapshot, and a foreign ABI said with its version even after contention
# -- and said again after a reattachment. Both widths: the segment is read by
# both.
vocem_test(state_poll_contention INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt pthread
    TIMEOUT 150 SKIP)
vocem_m32_twin(state_poll_contention)

# Another local user can make /dev/shm/vocem-<uid> first. The trust rule
# (owner is this uid, a regular file, nothing for group or others) as a
# function over struct stat, every case; then planted objects at 0666 and
# 0640 in a private /dev/shm, which the state reader, the poll, the version
# peek, the daemon's writer, the note reader and the note writer all refuse,
# and the same objects at 0600, which they read.
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

# The display's mode is the one being scanned out: every connector the
# machine's tree reports carries the mode and rate an independent walk of the
# kernel's CRTCs finds for its id, and says it is current; a fabricated tree
# says it is not. A witness on this machine, where the running and preferred
# heights agree (3840x2160 at 143.99 Hz against 60); display.h's header says
# why the difference in height cannot be shown here.
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

    # The window never signals the daemon it did not ask about. Under the
    # harness the unit question was never asked, so every stop -- Quit, a
    # close with no tray, Re-authorise -- fell to `pkill -TERM -x vocemd`, the
    # owner's live daemon included; a real launch did the same for the
    # question's first seconds, and a question past its cap exec'd a second
    # vocemd. Stubs on PATH log what would have run.
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

# A game whose runtime ships an older libstdc++ first on its search path: the
# heavy libraries carry their C++ runtime inside them now, so the overlay loads
# (gl_old_libstdcxx.cpp, old-runtime), and a load that fails is said by the
# shim under VOCEM_DEBUG, with no other library loaded in its place
# (unloadable). The stub is libstdc++ 6.0.21's version nodes and nothing else,
# in a directory of its own so no other test's RUNPATH can reach it. The probe
# carries its own runtime, or it would not start under the stub.
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
vocem_script_test(dev_run_gl ARGS "-DPROBE=$<TARGET_FILE:vocem_which_shim>")

# ---- 0.1.11 daemon lane ----
# Who the Flatpak bridge serves the voice channel to: a sandbox's own
# `drawing=1` is its word, and the host decides by application id -- an
# exported desktop entry that says Game, or the id listed in flatpak_apps.
vocem_test(flatpak_consent
    SOURCES flatpak_consent.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# A listener in a Flatpak sandbox that is not Discord's is not sent the token:
# the peer socket is mapped to its process and that process's /.flatpak-info
# read. The listener really is sandboxed -- a nested bwrap with its own user
# and pid namespace, the network shared -- inside daemon_notification's sandbox.
if(TARGET vocemd)
    vocem_test(daemon_peer_sandbox INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 120 SKIP)
endif()
# Links and FIFOs at the avatar cache's names: the writer's `.part` and the
# bridge's copy source. Nothing is written through a link, nothing waits on a
# FIFO, and only a regular file of the format's one size is a face.
vocem_test(avatar_cache_hostile
    SOURCES avatar_cache_hostile.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
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
    SOURCES flatpak_records.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# A settings copy into a sandbox that failed is tried again on the next sweep,
# rather than only after the host's config.ini next changes.
vocem_test(flatpak_config_retry
    SOURCES flatpak_config_retry.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# The emoji bank crosses into a sandbox once: a sandbox that stops asking and
# asks again is not given sixteen megabytes again, on the daemon's main thread.
vocem_test(flatpak_emoji_once
    SOURCES flatpak_emoji_once.cpp "${CMAKE_SOURCE_DIR}/daemon/src/flatpak_bridge.cpp"
    INCLUDE "${CMAKE_SOURCE_DIR}/daemon/src" LINK vocem_common)
# What the peer names reaches the daemon's log as one line of text: a refused
# RPC command is logged through sanitise_text, like every other string from it.
if(TARGET vocemd)
    vocem_test(daemon_log_lines INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}" LINK rt
        ENV ${VOCEM_DAEMON_ENV} TIMEOUT 90 SKIP)
endif()

# ---- 0.1.11 vk lane ----

# A swapchain whose images get their memory at first acquire
# (VK_EXT_swapchain_maintenance1's DEFERRED_MEMORY_ALLOCATION): nothing may be
# made from an image before then. The layer made a view and a framebuffer of
# every image at the first present, and every one of its submits failed after
# it -- zero foreign pixels, the device lost under the validation layer.
# Witnessed, and validated in vk_present_validated.
vocem_test_run(vk_present_deferred TARGET vk_present_draw
    ENV ${VOCEM_VK_WITNESSED_ENV} "VOCEM_VK_SCENARIO=deferred" SKIP)
vocem_m32_twin_run(vk_present_deferred TARGET vk_present_deferred
    ENV ${VOCEM_VK_PROBE32_ENV} "VOCEM_VK_SCENARIO=deferred")

# A second device PRESENTING beside the first, which stays alive. The renderer
# belongs to one device; the other was drawn with its buffers (commonparent
# under the validation layer, then SIGSEGV). Passed through while both present,
# moved once the owner has been silent for the hand-over interval (entry 210's
# rule on this side), and moved back when the second device is destroyed.
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
