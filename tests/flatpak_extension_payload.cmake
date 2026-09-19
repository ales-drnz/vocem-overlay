# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The Flatpak extension is a third artefact, and nothing had ever read its
# manifest.
#
# `grep -rln 'VulkanLayer.VocemOverlay' tests/ scripts/` returned nothing: the
# two PKGBUILDs are held to each other (tests/pkgbuild_agree.cmake, entry 76)
# and the extension was outside that comparison entirely, although it installs
# the same libraries into other people's sandboxes and is already published and
# signed. What it shipped instead of a test was a hand-written `post-install`
# list, which is the whole of its payload: the module passes
# -DVOCEM_BUILD_HOST_TOOLS=OFF, so every `install()` rule in CMakeLists.txt
# guarded by that option contributes nothing there.
#
# Measured on the installed, signed 0.1.9 extension before this existed: its
# share/vocem held `emoji_bank.rgba` and `OFL-NotoColorEmoji.txt` and NOT
# `emoji_sequences.bin`, so 0.1.9's single announced fix -- emoji sequences
# drawing as one glyph -- was absent in every Flatpak game, intermittently:
# the daemon's bridge leaves its copy of the table in the sandbox's runtime
# directory, so the first game of a session drew the parts and a later one drew
# the glyph, which is entry 117's "shape of a per-process answer". And its six
# OFL fonts and Dear ImGui travelled in binary form with none of their licence
# texts, where both PKGBUILDs ship ten.
#
# Both requirements below are derived from somewhere else in the tree rather
# than listed here, because a list in a test is a second place to forget: the
# data comes from the install rule the host package uses, and the licences from
# the files that exist beside the assets. Add a file to either and this test
# asks the manifest for it.

set(manifest
    "${SOURCE_DIR}/flatpak/vulkanlayer/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "the extension manifest is missing: ${manifest}")
endif()
file(READ "${manifest}" manifest_text)

# The manifest's payload: every `install -Dm644 <source> <destination>` in its
# post-install lists. YAML folds those over two lines, so the whitespace in the
# middle is any run of it.
string(REGEX MATCHALL "install -Dm644[ \t\r\n]+[^ \t\r\n]+[ \t\r\n]+[^ \t\r\n]+" installs
       "${manifest_text}")
set(sources "")
set(destinations "")
foreach(one IN LISTS installs)
    string(REGEX REPLACE "[ \t\r\n]+" ";" parts "${one}")
    list(GET parts 2 source)
    list(GET parts 3 destination)
    list(APPEND sources "${source}")
    list(APPEND destinations "${destination}")
endforeach()
list(LENGTH sources install_count)
if(install_count LESS 3)
    message(FATAL_ERROR
        "read ${install_count} install lines out of the manifest, which cannot be right: "
        "the parsing is what is broken, not the manifest")
endif()
message(STATUS "     the manifest installs ${install_count} file(s) by hand")

# --- the data the injected code reads ---------------------------------------
# One source of truth: the FILES list the host package installs into
# <datadir>/vocem. The extension's libraries read exactly those files, at a
# path compiled in from its own prefix, so the two payloads have to carry the
# same set -- and the bank and its sequence table are one artefact written by
# one run of the generator (entry 142), which is why a rule that ships one
# without the other is a defect and not a choice.
file(READ "${SOURCE_DIR}/CMakeLists.txt" top)
if(NOT top MATCHES
   "install\\(FILES([^)]*)DESTINATION \"\\$\\{CMAKE_INSTALL_DATADIR\\}/vocem\"")
    message(FATAL_ERROR
        "could not find the install rule for <datadir>/vocem in CMakeLists.txt, so this test "
        "does not know what the extension is supposed to carry")
endif()
string(REGEX MATCHALL "[A-Za-z0-9_/.-]+/[A-Za-z0-9_.-]+" data_files "${CMAKE_MATCH_1}")
list(LENGTH data_files data_count)
if(data_count LESS 2)
    message(FATAL_ERROR "the <datadir>/vocem install rule parsed to ${data_count} file(s)")
endif()

set(missing "")
foreach(file IN LISTS data_files)
    get_filename_component(name "${file}" NAME)
    set(found FALSE)
    foreach(destination IN LISTS destinations)
        if(destination MATCHES "/share/vocem/${name}$")
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found)
        list(APPEND missing "${name}")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR
        "the extension does not install, beside its libraries, what those libraries read: "
        "${missing}. The host package installs ${data_files} into <datadir>/vocem and the "
        "manifest's post-install list is the extension's entire payload, because "
        "-DVOCEM_BUILD_HOST_TOOLS=OFF switches that rule off.")
endif()
message(STATUS "     share/vocem carries all ${data_count} file(s) the host package installs")

# --- the licences the binaries oblige ---------------------------------------
# Every OFL text beside the font subsets is for a face compiled into
# vocem_common, which both injected libraries link, and Dear ImGui is compiled
# into both as well: six fonts and one MIT notice travel with the binaries
# whoever ships them. Not nlohmann and not Wuffs -- those are the daemon's, and
# the daemon is deliberately not in the extension.
file(GLOB font_licences "${SOURCE_DIR}/third_party/fonts/OFL*.txt")
list(LENGTH font_licences font_licence_count)
if(font_licence_count LESS 6)
    message(FATAL_ERROR
        "found ${font_licence_count} OFL texts beside the font subsets, expected at least the "
        "six entry 128 left there: the glob is what is broken")
endif()
set(required_licences ${font_licences} "${SOURCE_DIR}/third_party/imgui/LICENSE.txt")

set(missing "")
foreach(licence IN LISTS required_licences)
    get_filename_component(name "${licence}" NAME)
    if(NOT EXISTS "${licence}")
        message(FATAL_ERROR "the tree has no ${licence} to ship")
    endif()
    set(found FALSE)
    foreach(source IN LISTS sources)
        if(source MATCHES "/${name}$" OR source STREQUAL "${name}")
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found)
        list(APPEND missing "${name}")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR
        "the extension ships the fonts and Dear ImGui in binary form without their licence "
        "texts: ${missing}. Both PKGBUILDs install ten texts for the same assets (entry 50: a "
        "licence is part of what a file is, and a derived artefact ships without one by "
        "default); an extension is a binary distribution like any other.")
endif()
list(LENGTH required_licences required_count)
message(STATUS "     the ${required_count} licence texts the injected libraries oblige travel with them")

# --- the licences go somewhere a reader looks -------------------------------
# share/licenses, which is where the host package puts them and where anybody
# looking for them looks. The bank's own OFL travels in share/vocem beside the
# bank instead, deliberately and from the first version of this manifest.
foreach(destination IN LISTS destinations)
    if(destination MATCHES "(OFL|LICENSE)[^/]*$"
       AND NOT destination MATCHES "/share/vocem/"
       AND NOT destination MATCHES "/share/licenses/")
        message(FATAL_ERROR "a licence text is installed outside share/licenses: ${destination}")
    endif()
endforeach()

message("ok   the extension carries the data its libraries read and the licences they oblige")
