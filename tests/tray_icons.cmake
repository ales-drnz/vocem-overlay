# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The tray's five state icons: one circle, one diameter, the right colours,
# well-formed, and all installed.
#
# The tray icon is the user's own voice state -- a hollow ring outside a
# channel, a grey disc in one, green while speaking, red with a microphone
# while muted, red with headphones while deafened. A state icon that is
# missing from the install, or that quietly stops carrying its token colour,
# degrades to a generic icon with nothing logged anywhere, which is this
# project's least favourite failure shape. And "all the same size" is the
# owner's requirement rather than a nicety: five discs that drift apart make
# the tray twitch as the state changes, so the radius is asserted equal
# across all five rather than trusted to five hand-written files.

set(icons_dir "${SOURCE_DIR}/packaging/icons")

# Which colour carries each state's meaning. Grey is the overlay's own muted
# text token (measured legible on every scene it is read over, which is also
# what makes it work on a light panel and a dark one); the green is the
# speaking-ring token and the red is the badge's.
# A pipe separator, because a nested cmake list flattens into its parent.
set(pairs
    "io.github.ales_drnz.vocem_overlay-offline.svg|#9aa0ab"
    "io.github.ales_drnz.vocem_overlay-idle.svg|#9aa0ab"
    "io.github.ales_drnz.vocem_overlay-speaking.svg|#23a55a"
    "io.github.ales_drnz.vocem_overlay-muted.svg|#f23f43"
    "io.github.ales_drnz.vocem_overlay-deafened.svg|#f23f43")

set(radii "")

foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 token)
    set(file "${icons_dir}/${name}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${name} is missing -- the tray state falls back to "
                            "the generic icon with nothing logged")
    endif()
    file(READ "${file}" svg)
    if(NOT svg MATCHES "${token}")
        message(FATAL_ERROR "${name} does not carry its state's token colour "
                            "${token} -- the icon has stopped saying what the "
                            "overlay says")
    endif()

    # The state circle: centred on the 48 grid, and the same radius in every
    # icon. Its absence means the icon is not a circle any more, which is the
    # whole shape the owner asked for.
    string(REGEX MATCH "<circle cx=\"24\" cy=\"24\" r=\"([0-9.]+)\"" circle "${svg}")
    if(NOT circle)
        message(FATAL_ERROR "${name} has no centred state circle -- the tray "
                            "icons are one circle each, at one size")
    endif()
    list(APPEND radii "${CMAKE_MATCH_1}")

    # Nothing behind the circle: a box, a frame or a backdrop is exactly what
    # the owner asked to be rid of, and it is what made the 22-px icon a
    # picture of the product instead of a state.
    if(svg MATCHES "<rect[^>]*x=\"3\"" OR svg MATCHES "fill=\"#17181c\"")
        message(FATAL_ERROR "${name} draws a surface behind its circle -- the "
                            "tray icon is the state, not a portrait of the "
                            "application")
    endif()

    # Well-formed enough to render: a comment with a double hyphen inside is
    # not XML, and an icon theme engine that meets one draws nothing. Every
    # legal "--" belongs to a comment delimiter -- the opener carries one and
    # the closer carries one -- so the counts must balance exactly. This has
    # caught its own author twice now, both times before a commit.
    string(REGEX MATCHALL "--" hyphens "${svg}")
    list(LENGTH hyphens hyphen_count)
    string(REGEX MATCHALL "<!--" openers "${svg}")
    list(LENGTH openers opener_count)
    string(REGEX MATCHALL "-->" closers "${svg}")
    list(LENGTH closers closer_count)
    math(EXPR delimiter_count "${opener_count} + ${closer_count}")
    if(NOT hyphen_count EQUAL delimiter_count)
        message(FATAL_ERROR "${name} carries a double hyphen outside a comment "
                            "delimiter -- not well-formed XML, and the theme "
                            "engine will draw nothing")
    endif()
endforeach()

list(REMOVE_DUPLICATES radii)
list(LENGTH radii distinct_radii)
if(NOT distinct_radii EQUAL 1)
    message(FATAL_ERROR
        "the tray icons are drawn at ${distinct_radii} different radii "
        "(${radii}) -- the icon would change size as the voice state changes")
endif()

# And the install rule ships all five beside the application's own icon: an
# icon that exists only in the repository is a tray that works only here.
# Matched inside an install(FILES ...) call with the comments stripped first,
# not as a bare substring of the whole file -- a commented-out install line
# satisfied unit_hardening's substring match once (entry 77), and this check
# had the same shape.
file(READ "${SOURCE_DIR}/CMakeLists.txt" cmake_lists)
string(REGEX REPLACE "#[^\n]*" "" cmake_lists "${cmake_lists}")
foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    if(NOT cmake_lists MATCHES "install\\(FILES[^)]*${name}")
        message(FATAL_ERROR "${name} is not in the install rule -- the packaged "
                            "tray falls back to the generic icon")
    endif()
endforeach()

# The window must ask for exactly these names. A state whose icon name is
# misspelled draws the theme's fallback with nothing logged. Comments stripped
# for the same reason as above.
file(READ "${SOURCE_DIR}/gui/qml/Tray.qml" tray)
string(REGEX REPLACE "//[^\n]*" "" tray "${tray}")
foreach(pair IN LISTS pairs)
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    string(REPLACE ".svg" "" icon_name "${name}")
    if(NOT tray MATCHES "\"${icon_name}\"")
        message(FATAL_ERROR "Tray.qml never asks for ${icon_name} -- an "
                            "installed icon no state can reach")
    endif()
endforeach()

message(STATUS "five tray icons, one radius, token-true, installed and asked for")
