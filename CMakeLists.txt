# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
cmake_minimum_required(VERSION 3.20)

project(vocem-overlay
    # The release number, stated once, here. Everything else that carries a
    # version derives from this or is checked against it: gui/CMakeLists.txt
    # compiles it into the window as VOCEM_VERSION, and tests/version_agrees.cmake
    # holds both PKGBUILDs, .SRCINFO and the metainfo's release list to it.
    #
    # It sat at 0.1.0 while four releases went out, because neither PKGBUILD
    # passes a version to CMake and nothing compared the two: the installed
    # 0.1.4 package's About page read "Version 0.1.0", the metainfo's newest
    # release was 0.1.2, and .SRCINFO said 0.1.2 as well. Four copies of one
    # fact, none of them told when another moved.
    VERSION 0.1.10
    DESCRIPTION "Discord voice overlay drawn inside the game, on Linux"
    LANGUAGES CXX
)

# Dear ImGui is a submodule, and a plain `git clone` leaves it empty. Without this
# the first thing a stranger sees is "No SOURCES given to target:
# vocem_fonts_frame_quiet", which names the wrong problem entirely.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp")
    message(FATAL_ERROR
        "third_party/imgui is empty: Dear ImGui is a git submodule.\n"
        "  git submodule update --init --recursive\n"
        "or clone with --recurse-submodules.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The Vulkan API version advertised in the layer manifest. Keep in step with the
# headers we build against.
set(VOCEM_LAYER_API_VERSION "1.4.350")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Where the colour emoji bank lands, told to every consumer of
# vocem/emoji_bank.h at compile time. The bank itself is generated offline by
# scripts/make-emoji-bank.py and checked in (no fonttools at build time);
# VOCEM_EMOJI_BANK overrides the path at run time for tests and dev trees.
add_compile_definitions(
    VOCEM_EMOJI_BANK_PATH="${CMAKE_INSTALL_FULL_DATADIR}/vocem/emoji_bank.rgba")

# 32-bit libraries belong next to the other 32-bit libraries, not in the 64-bit
# directory, or the loader will refuse them for 32-bit games.
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT DEFINED CACHE{VOCEM_LIBDIR_OVERRIDDEN})
    set(CMAKE_INSTALL_LIBDIR "lib32" CACHE PATH "Object code libraries" FORCE)
    set(VOCEM_LIBDIR_OVERRIDDEN ON CACHE INTERNAL "")
endif()

# The daemon and CLI are 64-bit only: they never enter a game process, so there is
# no reason to build them twice. A 32-bit build produces just the injected
# libraries, for 32-bit titles running under Proton or native.
option(VOCEM_BUILD_HOST_TOOLS "Build vocemd, the CLI and the configuration GUI" ON)

# A packaged install is expected to work with nothing to switch on:
#   * the Vulkan layer manifest carries only disable_environment, so the layer is
#     active in every Vulkan application;
#   * the OpenGL shim is preloaded session-wide through environment.d, because
#     OpenGL has no layer mechanism at all.
# A development build does neither, so a half-finished overlay cannot destabilise
# unrelated applications on the machine it is written on.
option(VOCEM_ENABLE_BY_DEFAULT
    "Cover every game with no user action: implicit layer plus session preload" OFF)

# Off in a package build: makepkg has no reason to run them, and a missing
# locale on a build server is not a packaging failure.
option(VOCEM_BUILD_TESTS "Build the test programs" ON)

# Whether this toolchain can produce a 32-bit binary. Asked once, here, because
# two directories need the answer: common/ builds the 32-bit twin of the shared
# objects when it can, and tests/ builds the -m32 test executables against it.
# The 32-bit injected libraries themselves come from a tree of their own
# (build32) that ctest does not cover, which is where entries 30/33/34 hid.
if(VOCEM_BUILD_HOST_TOOLS AND VOCEM_BUILD_TESTS)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-m32")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-m32")
    check_cxx_source_compiles("#include <dlfcn.h>\nint main(){return dlvsym(0,\"\",\"\")!=0;}"
        VOCEM_HAVE_M32)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

# The C++ runtime of the two injected heavy libraries (libvocem_gl.so and
# libvocem_vk.so) is linked INTO them, not NEEDED. They are loaded into games,
# and a game's runtime can put an older libstdc++.so.6 first on the search path
# -- the Steam scout runtime's 6.0.21, tried by the 0.1.10 review: the build
# NEEDS GLIBCXX_3.4.29, ld.so refuses the whole library, and the game has no
# overlay. Both libraries' version scripts keep every symbol but their entry
# points local, so the copy they carry is invisible to the game's own (entry
# 195; tests/injected_runtime.cmake holds the NEEDED list at both widths). The
# shim has its own, stricter answer in gl/CMakeLists.txt: no C++ runtime at all.
set(VOCEM_INJECTED_RUNTIME_LINK_OPTIONS -static-libstdc++ -static-libgcc)

# What both injected paths, the daemon, the window and the tests share, built
# once per width; the two injected libraries link it, so it comes first.
add_subdirectory(common)
add_subdirectory(layer)
add_subdirectory(gl)

if(VOCEM_BUILD_HOST_TOOLS)
    add_subdirectory(daemon)
    add_subdirectory(cli)
    add_subdirectory(gui)

    if(VOCEM_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()

# --- launcher helper ---------------------------------------------------------
# Uses the linker's $LIB token so a single script serves both architectures. The
# literal must survive CMake's configure step, hence the placeholder.
#
# From the install prefix, not from a hardcoded /usr. Entry 16's rule is that
# vocem-run preloads the same shim the session does, so the two entry paths
# cannot drift -- and this said /usr while packaging/environment.d/50-vocem.conf.in
# took @CMAKE_INSTALL_PREFIX@ (entry 137 made it a template and did not come
# back here). Measured on a configure at another prefix: vocem-run said
# `/usr/$LIB/libvocem_gl_shim.so` and the session's file said
# `<prefix>/$LIB/libvocem_gl_shim.so`, so every program started through
# vocem-run after a ~/.local install printed `cannot be preloaded: ignored` and
# got no OpenGL overlay. tests/one_preload_path.cmake configures at a prefix of
# its own and compares the two generated files, because at the default prefix
# the defect is invisible.
set(VOCEM_LIBDIR_TOKEN "${CMAKE_INSTALL_PREFIX}/$LIB")
configure_file(scripts/vocem-run.in "${CMAKE_BINARY_DIR}/vocem-run" @ONLY)

if(VOCEM_BUILD_HOST_TOOLS)
    install(PROGRAMS "${CMAKE_BINARY_DIR}/vocem-run"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # The tool for the person whose game has no overlay. Two release notes
    # announced it and neither PKGBUILD installed it: it existed in the source
    # tree and nowhere on an installed machine.
    install(PROGRAMS scripts/vocem-why.sh
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME vocem-why)
endif()

# --- packaging ---------------------------------------------------------------
# Only from the 64-bit build: a 32-bit build contributes just its libraries.
if(VOCEM_BUILD_HOST_TOOLS)
    # The bank is a Modified Version of Noto Color Emoji under the OFL 1.1
    # (common/emoji/README.md): its licence travels beside it, and so does its
    # sequence table, which the reader looks for beside the bank by name.
    install(FILES common/emoji/emoji_bank.rgba common/emoji/emoji_sequences.bin
        common/emoji/OFL-NotoColorEmoji.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/vocem")
    install(FILES packaging/io.github.ales_drnz.vocem_overlay.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES packaging/io.github.ales_drnz.vocem_overlay.metainfo.xml
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    # The application's own icon, and the tray's five state variants -- one
    # circle at one diameter each, told apart by name swaps (Tray.qml says why
    # names and not pixmaps). The application icon is not among the tray's:
    # the tray shows a state, the launcher shows the product.
    install(FILES packaging/icons/io.github.ales_drnz.vocem_overlay.svg
        packaging/icons/io.github.ales_drnz.vocem_overlay-offline.svg
        packaging/icons/io.github.ales_drnz.vocem_overlay-idle.svg
        packaging/icons/io.github.ales_drnz.vocem_overlay-speaking.svg
        packaging/icons/io.github.ales_drnz.vocem_overlay-muted.svg
        packaging/icons/io.github.ales_drnz.vocem_overlay-deafened.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

    # The unit and the session's environment file carry the install prefix in
    # them (ExecStart, and the shim's path), so they are generated from the
    # prefix rather than written with /usr in them: the README's install into
    # ~/.local shipped a unit whose ExecStart named a binary that was not there
    # (entry 137). Both land in build/packaging, where that recipe copies them
    # from. The directories below are systemd's own and deliberately not
    # CMAKE_INSTALL_LIBDIR: systemd reads lib/, never lib64/.
    configure_file(packaging/systemd/vocemd.service.in
        "${CMAKE_BINARY_DIR}/packaging/vocemd.service" @ONLY)
    configure_file(packaging/environment.d/50-vocem.conf.in
        "${CMAKE_BINARY_DIR}/packaging/50-vocem.conf" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/packaging/vocemd.service"
        DESTINATION "lib/systemd/user")
    install(FILES packaging/systemd/90-vocem.preset
        DESTINATION "lib/systemd/user-preset")

    # The session preload is what covers OpenGL games without any user action. Only
    # a packaged install ships it: preloading into every process of a session is
    # not something a build tree should do behind the developer's back.
    if(VOCEM_ENABLE_BY_DEFAULT)
        install(FILES "${CMAKE_BINARY_DIR}/packaging/50-vocem.conf"
            DESTINATION "lib/environment.d")
    endif()

    # Enabled for every user without anyone running systemctl: the symlink is what
    # `systemctl --user enable` would create, shipped by the package instead. This
    # is the same mechanism pipewire and friends use.
    install(CODE "
        set(wants_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/systemd/user/default.target.wants\")
        file(MAKE_DIRECTORY \"\${wants_dir}\")
        file(CREATE_LINK ../vocemd.service \"\${wants_dir}/vocemd.service\" SYMBOLIC)
    ")
endif()
