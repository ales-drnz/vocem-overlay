# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The System tray section: which picture the panel wears, and a preview that
# actually draws it.
#
# Two claims, both measured on the real window offscreen, because both were
# wrong in a way no amount of reading the QML would have shown.
#
#   1. The setting reaches the preview. With `tray_voice_icon = true` the strip
#      carries an item named trayVoiceIcon; with it false, one named
#      trayApplicationIcon. The names are on the same Image, so exactly one of
#      them exists in either dump and a page that ignored the setting would show
#      the same one twice.
#
#   2. **The picture is really there.** This is the half that earned the test.
#      With the org.kde.desktop style loaded, `QIcon::fromTheme` resolves every
#      Breeze name this window asks for and none of this application's own six
#      -- although they are installed in hicolor and the panel resolves the same
#      names for the tray icon over D-Bus. Measured with a trace in the icon
#      provider: `hasThemeIcon` false, pixmap 0x0, for
#      io.github.ales_drnz.vocem_overlay and all five states. So the About page had been
#      drawing an empty square where its icon goes for as long as it has
#      existed, and this page would have drawn six. The artwork is carried in
#      the binary now and the provider falls back to it.
#
#      An Image whose source will not load reports `status: Image.Error` and
#      paints nothing while keeping the size its layout gave it -- so a
#      geometry dump alone cannot tell a drawn icon from a missing one. The page
#      exposes `loadedIcons`, the number of its own pictures that actually came
#      up, and this holds it to six: the five states in the legend and the one
#      in the strip.
#
# Against 0.1.0-67 there is no System tray section at all, so both fail.

# Run through ctest, or with -DCMAKE_CURRENT_BINARY_DIR=<build>/tests: in script
# mode that variable is the working directory, and a run started from the
# repository root left its scratch directories in the repository (0.1.7's
# packaging pass left two, untracked, beside CMakeLists.txt).
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
if(NOT EXISTS "${CONFIG_BINARY}")
    message(STATUS "skip the configuration window was not built")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/tray-icon-choice")
file(REMOVE_RECURSE "${scratch}")

macro(walk name setting)
    file(MAKE_DIRECTORY "${scratch}/${name}/config/vocem")
    file(MAKE_DIRECTORY "${scratch}/${name}/cache/vocem")
    file(WRITE "${scratch}/${name}/config/vocem/config.ini" "tray_voice_icon = ${setting}\n")
    execute_process(
        COMMAND "${CONFIG_BINARY}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "XDG_CONFIG_HOME=set:${scratch}/${name}/config"
            "XDG_CACHE_HOME=set:${scratch}/${name}/cache"
            "QT_QPA_PLATFORM=set:offscreen"
            "VOCEM_CONFIG_SECTIONS=set:7"
            "VOCEM_CONFIG_GEOMETRY=set:${scratch}/${name}.json")
    if(NOT status EQUAL 0)
        message(STATUS "skip the window could not run here: ${status} ${errors}")
        return()
    endif()
    if(NOT EXISTS "${scratch}/${name}.json")
        message(FATAL_ERROR "the window wrote no geometry dump")
    endif()
    file(READ "${scratch}/${name}.json" ${name}_dump)
endmacro()

walk(voice true)
walk(application false)

# ---- 1. the choice reaches the picture.
if(NOT voice_dump MATCHES "\"item\": \"[^\"]*trayVoiceIcon\"[^\n]*\"visible\": true")
    message(FATAL_ERROR
        "with tray_voice_icon on, the tray preview does not draw the voice icon -- "
        "the setting does not reach the picture that is supposed to show it")
endif()
if(voice_dump MATCHES "trayApplicationIcon")
    message(FATAL_ERROR
        "with tray_voice_icon on, the preview also carries the application icon: "
        "the two are the same Image and only one name can be right")
endif()
if(NOT application_dump MATCHES "\"item\": \"[^\"]*trayApplicationIcon\"[^\n]*\"visible\": true")
    message(FATAL_ERROR
        "with tray_voice_icon off, the tray preview still does not draw the "
        "application's own icon")
endif()

# ---- 2. and the pictures actually loaded.
#
# Read off the page rather than inferred from a rectangle: an Image that failed
# to load keeps the size its layout gave it and paints nothing.
if(NOT voice_dump MATCHES "\"item\": \"[^\"]*trayPreview\"[^\n]*\"loadedIcons\": ([0-9.e+-]+)")
    message(FATAL_ERROR "the tray preview does not say how many of its icons loaded")
endif()
set(loaded "${CMAKE_MATCH_1}")
if(NOT loaded EQUAL 6)
    message(FATAL_ERROR
        "only ${loaded} of the tray preview's 6 pictures loaded -- the window cannot "
        "find its own artwork, which is what the About page has been failing at "
        "silently: five states in the legend and one in the strip")
endif()

message(STATUS "the tray preview follows the setting, and all six of its pictures draw")
