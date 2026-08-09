# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# One installed package, one answer to "which version is this?".
#
# It used to give three. Measured on the installed 0.1.4-1, twice:
# `pacman -Q` said 0.1.4, the window's About page said **Version 0.1.0**, the
# installed metainfo's newest <release> said 0.1.2, and packaging/.SRCINFO said
# 0.1.2 too. The mechanism is entry 33's exact shape one directory further out:
# CMakeLists.txt's project(VERSION) is compiled into the window as VOCEM_VERSION,
# neither PKGBUILD passes a version to CMake at all (grepped: no -DVOCEM_VERSION,
# no -DPROJECT_VERSION, no sed), the metainfo's release list is written by hand
# and .SRCINFO is generated once and forgotten. Four copies of one fact, free to
# drift, and the drift is invisible: `appstreamcli validate` passes on a release
# list two releases out of date, because being out of date is not something
# AppStream can see.
#
# ---- what this holds, and why it is not equality
#
# The four numbers are NOT all meant to be equal while a release is being
# written, and a test asserting that they are would fail for a reason that is
# not the one it names -- entry 77's shape, committed inside the test written to
# answer it. The rule the project actually keeps (CLAUDE.md, and PKGBUILD.local's
# own comment) is:
#
#   * CMakeLists.txt states the release being written. One statement, everything
#     else derives from it or is checked against it.
#   * PKGBUILD.local builds the working tree, so it carries that same number.
#   * PKGBUILD builds `#tag=v$pkgver`, so it names either the newest tag that
#     exists or the release this commit is about to be tagged with. A PKGBUILD
#     naming a tag nobody can fetch is broken for the stranger it exists for --
#     and a tag whose own PKGBUILD names the PREVIOUS release is broken for the
#     same stranger, which is why the second state has to be allowed: a tag
#     cannot exist before the commit it is put on. Ahead of both numbers is a
#     fault; behind is mid-cycle.
#   * .SRCINFO is generated from PKGBUILD, so it says whatever PKGBUILD says.
#   * the metainfo lists every release that has a tag, newest first. It may name
#     the release being written, because the entry and the tag are made in one
#     pass and neither order is wrong.
#
# So this test is true on this branch today (0.1.5 being written, newest tag
# v0.1.4) and true the day after v0.1.5 lands, without anybody editing it to make
# it green. It prints which of the two states it matched, so a reader can tell
# mid-cycle from released without going and looking.
#
# Against the tree as it stands at 9f47bb6 it fails three times over:
# CMakeLists.txt 0.1.0 against PKGBUILD.local's 0.1.4, .SRCINFO's 0.1.2 against
# PKGBUILD's 0.1.4, and a metainfo carrying no entry for either 0.1.3 or 0.1.4.
#
# ---- and the artefact, not only the sources
#
# The last section runs the built window offscreen and asks what version it
# carries, because "true of the intention, false of the artefact" is how this
# defect lived: every source-level claim about the version was fine, and the
# binary said 0.1.0. The window prints it into the geometry dump
# (VOCEM_CONFIG_GEOMETRY, one line, before any section). What that line is NOT is
# the rendered label: the dump holds rectangles and numbers, never text, so this
# checks the string ConfigBridge::version() answers with -- one QML binding
# (`AboutPage.qml`: qsTr("Version %1").arg(root.config.version)) short of the
# pixels. Closing that last step would mean teaching dump_item to print text,
# which is a wider change to the instrument than this defect asks for.

set(problems "")

# --- what each file says -----------------------------------------------------

# The one line of CMakeLists.txt's project() block that is a bare version.
set(project_version "")
file(STRINGS "${SOURCE_DIR}/CMakeLists.txt" cmakelists_lines)
foreach(line IN LISTS cmakelists_lines)
    if(line MATCHES "^[ \t]*VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)[ \t]*$")
        set(project_version "${CMAKE_MATCH_1}")
        break()
    endif()
endforeach()
if(project_version STREQUAL "")
    message(FATAL_ERROR
        "CMakeLists.txt has no project(VERSION x.y.z) line -- the one place the "
        "release number is stated")
endif()

function(pkgbuild_version file out)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "no ${file}")
    endif()
    set(found "")
    file(STRINGS "${file}" lines)
    foreach(line IN LISTS lines)
        if(line MATCHES "^pkgver=(.+)$")
            set(found "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "no pkgver= in ${file}")
    endif()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

pkgbuild_version("${SOURCE_DIR}/packaging/PKGBUILD" tagged_pkgver)
pkgbuild_version("${SOURCE_DIR}/packaging/PKGBUILD.local" local_pkgver)

# .SRCINFO is generated: `makepkg -p PKGBUILD --printsrcinfo > .SRCINFO`.
set(srcinfo_pkgver "")
if(EXISTS "${SOURCE_DIR}/packaging/.SRCINFO")
    file(STRINGS "${SOURCE_DIR}/packaging/.SRCINFO" srcinfo_lines)
    foreach(line IN LISTS srcinfo_lines)
        if(line MATCHES "^[ \t]*pkgver = (.+)$")
            set(srcinfo_pkgver "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
endif()

# Every <release version="..."> in the metainfo, in the order they are written.
set(metainfo "${SOURCE_DIR}/packaging/io.github.ales_drnz.vocem_overlay.metainfo.xml")
if(NOT EXISTS "${metainfo}")
    message(FATAL_ERROR "no ${metainfo}")
endif()
file(READ "${metainfo}" metainfo_text)
string(REGEX MATCHALL "<release version=\"[0-9.]+\"" release_tags "${metainfo_text}")
set(metainfo_versions "")
foreach(tag IN LISTS release_tags)
    string(REGEX REPLACE "^<release version=\"([0-9.]+)\"$" "\\1" version "${tag}")
    list(APPEND metainfo_versions "${version}")
endforeach()
if(metainfo_versions STREQUAL "")
    message(FATAL_ERROR "the metainfo carries no <release> entries at all")
endif()
list(GET metainfo_versions 0 metainfo_newest)

# The tags, newest first. A source tree with no git (a tarball build, a shallow
# clone) cannot answer this; that half then says so instead of passing quietly.
set(tags "")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" tag --sort=-v:refname
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE tag_output
        RESULT_VARIABLE tag_status
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(tag_status EQUAL 0 AND NOT tag_output STREQUAL "")
        string(REPLACE "\n" ";" tags "${tag_output}")
    endif()
endif()
set(tag_versions "")
foreach(tag IN LISTS tags)
    if(tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
        list(APPEND tag_versions "${CMAKE_MATCH_1}")
    endif()
endforeach()
set(newest_tag "")
if(NOT tag_versions STREQUAL "")
    list(GET tag_versions 0 newest_tag)
endif()

# --- the rules ---------------------------------------------------------------

# 1. The working-tree PKGBUILD builds the working tree, so it carries the number
#    the working tree states.
if(NOT local_pkgver VERSION_EQUAL project_version)
    list(APPEND problems
        "packaging/PKGBUILD.local says pkgver=${local_pkgver} and CMakeLists.txt states VERSION ${project_version}. That file builds this working tree, so it is the same release: whichever of the two is behind is the one to move.")
endif()

# 2. The tag-built PKGBUILD names a tag that exists, and the newest one -- the
#    release a stranger gets when they build this file.
set(state "")
if(newest_tag STREQUAL "")
    if(tagged_pkgver VERSION_GREATER project_version)
        list(APPEND problems
            "packaging/PKGBUILD says pkgver=${tagged_pkgver}, which is ahead of the ${project_version} being written, and no tags are visible here to check it against.")
    endif()
    set(state "no tags visible: only the ordering of PKGBUILD's ${tagged_pkgver} against ${project_version} was checked")
elseif(tagged_pkgver VERSION_EQUAL project_version AND NOT newest_tag VERSION_EQUAL project_version)
    # The release being cut. This state was missing and the rule above forbade
    # it, which made the test impossible to satisfy at the one moment it
    # matters: a tag cannot exist before the commit it is put on, so the commit
    # that moves PKGBUILD to the new number is always one where that tag is not
    # there yet. Forbidding it would leave two bad choices -- tag first and ship
    # a v0.1.5 whose own PKGBUILD fetches v0.1.4, which is what a stranger
    # checking out that tag would build, or edit the test to make it green. What
    # the project actually does, and what v0.1.4 shows in its own tree, is move
    # the number and then tag that commit.
    #
    # This branch is the narrow one: PKGBUILD may be ahead of every tag only
    # when it names exactly the release CMakeLists.txt states. Ahead of both is
    # still a fault, and behind is still the mid-cycle state below.
    set(state "being cut: PKGBUILD carries ${project_version} and v${project_version} is not tagged yet")
elseif(NOT tagged_pkgver VERSION_EQUAL newest_tag)
    list(APPEND problems
        "packaging/PKGBUILD says pkgver=${tagged_pkgver}, which is neither the newest tag (v${newest_tag}) nor the ${project_version} being released. That file fetches #tag=v\$pkgver, so it has to name a tag that exists or the one this commit is about to be tagged with.")
elseif(newest_tag VERSION_EQUAL project_version)
    set(state "released: v${newest_tag} exists, so all three files carry ${project_version}")
else()
    set(state "mid-cycle: ${project_version} is being written, newest tag v${newest_tag}")
endif()

# 3. .SRCINFO is generated from PKGBUILD and says nothing of its own.
if(srcinfo_pkgver STREQUAL "")
    list(APPEND problems
        "packaging/.SRCINFO has no pkgver line, or is missing. It is generated: cd packaging && makepkg -p PKGBUILD --printsrcinfo > .SRCINFO")
elseif(NOT srcinfo_pkgver VERSION_EQUAL tagged_pkgver)
    list(APPEND problems
        "packaging/.SRCINFO says pkgver = ${srcinfo_pkgver} and packaging/PKGBUILD says ${tagged_pkgver}. .SRCINFO is generated from that file and was generated once, at 0.1.2: regenerate it with `cd packaging && makepkg -p PKGBUILD --printsrcinfo > .SRCINFO`.")
endif()

# 4. Every release that has a tag is in the metainfo. This is the half that was
#    actually wrong: Discover and GNOME Software show this list as "what's new",
#    and it stopped two releases back on a package four releases in.
set(missing "")
foreach(version IN LISTS tag_versions)
    list(FIND metainfo_versions "${version}" at)
    if(at EQUAL -1)
        list(APPEND missing "${version}")
    endif()
endforeach()
if(NOT missing STREQUAL "")
    string(REPLACE ";" ", " missing_text "${missing}")
    list(APPEND problems
        "the metainfo has no <release> for ${missing_text}, and those releases have tags. Discover and GNOME Software read that list, whose words come from CHANGELOG.md and from nowhere else.")
endif()

# 5. Newest first, and the newest is either the last release or the one being
#    written (its entry and its tag are made in one pass).
set(previous "")
foreach(version IN LISTS metainfo_versions)
    if(NOT previous STREQUAL "" AND NOT previous VERSION_GREATER version)
        list(APPEND problems
            "the metainfo lists ${previous} before ${version}: the release list is newest first.")
        break()
    endif()
    set(previous "${version}")
endforeach()
if(NOT newest_tag STREQUAL ""
   AND NOT metainfo_newest VERSION_EQUAL newest_tag
   AND NOT metainfo_newest VERSION_EQUAL project_version)
    list(APPEND problems
        "the metainfo's newest <release> is ${metainfo_newest}, which is neither the newest tag v${newest_tag} nor the ${project_version} being written.")
endif()

if(NOT problems STREQUAL "")
    message("One fact, more than one answer -- which is how the installed 0.1.4")
    message("package's About page came to say Version 0.1.0:")
    message("")
    message("  CMakeLists.txt       VERSION ${project_version}")
    message("  PKGBUILD             pkgver=${tagged_pkgver}")
    message("  PKGBUILD.local       pkgver=${local_pkgver}")
    message("  .SRCINFO             pkgver = ${srcinfo_pkgver}")
    message("  metainfo newest      ${metainfo_newest}")
    message("  newest tag           v${newest_tag}")
    message("")
    foreach(problem IN LISTS problems)
        message("  * ${problem}")
    endforeach()
    message(FATAL_ERROR "the release number is stated in CMakeLists.txt; the rest follow it")
endif()

# --- the artefact ------------------------------------------------------------
#
# What the built window says it is, which is the only claim the four files above
# cannot make on their own.
if(NOT DEFINED CONFIG_BINARY OR NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS
        "the sources agree (${state}); the window was not built, so what the "
        "binary carries was not checked")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/version-agrees")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}/config/vocem")
file(MAKE_DIRECTORY "${scratch}/cache/vocem")
# A fabricated display, so the run does not depend on what is plugged into this
# machine.
file(MAKE_DIRECTORY "${scratch}/drm/card0-DP-1")
file(WRITE "${scratch}/drm/card0-DP-1/status" "connected\n")
file(WRITE "${scratch}/drm/card0-DP-1/enabled" "enabled\n")
file(WRITE "${scratch}/drm/card0-DP-1/modes" "3840x2160\n")
file(WRITE "${scratch}/config/vocem/config.ini" "")

execute_process(
    COMMAND "${CONFIG_BINARY}"
    RESULT_VARIABLE window_status
    ERROR_VARIABLE window_errors
    TIMEOUT 90
    ENVIRONMENT_MODIFICATION
        "XDG_CONFIG_HOME=set:${scratch}/config"
        "XDG_CACHE_HOME=set:${scratch}/cache"
        "VOCEM_DRM_ROOT=set:${scratch}/drm"
        "QT_QPA_PLATFORM=set:offscreen"
        "VOCEM_CONFIG_SECTIONS=set:0"
        "VOCEM_CONFIG_GEOMETRY=set:${scratch}/geometry.json")
if(NOT window_status EQUAL 0)
    message(STATUS "skip the window could not run here: ${window_status} ${window_errors}")
    return()
endif()

file(READ "${scratch}/geometry.json" dump)
# It must be there. A dump without the line is an old binary or an instrument
# that stopped printing it, and either way "no line" must not read as agreement
# -- the reason preview_display.cmake refuses an empty dump, for the same reason.
if(NOT dump MATCHES "\"version\": \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    message(FATAL_ERROR
        "the window printed no version line into its geometry dump. Either this is "
        "a binary from before the line existed, or the instrument stopped writing "
        "it; an absent line is not agreement.")
endif()
set(binary_version "${CMAKE_MATCH_1}")

if(NOT binary_version VERSION_EQUAL project_version)
    message(FATAL_ERROR
        "the built window carries ${binary_version} and CMakeLists.txt states "
        "${project_version}. VOCEM_VERSION comes from PROJECT_VERSION "
        "(gui/CMakeLists.txt), so the two can only differ if that chain was broken "
        "or the build tree is stale -- and a window that names the wrong release is "
        "exactly what shipped from 0.1.0 to 0.1.4.")
endif()

message(STATUS
    "one version: CMakeLists.txt ${project_version}, PKGBUILD.local "
    "${local_pkgver}, PKGBUILD ${tagged_pkgver}, .SRCINFO ${srcinfo_pkgver}, "
    "metainfo newest ${metainfo_newest}, and the built window says "
    "${binary_version} (${state})")
