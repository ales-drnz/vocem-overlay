# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

# Widgets is not used directly: it is what the platform theme needs behind the
# system tray icon on Plasma. See the note in main.cpp.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Widgets)

# The window resolves a font family to the files the overlay opens. It is the
# only half of this project that may ask: fontconfig inside a game would be a
# library, a cache and a pile of file syscalls in somebody else's renderer.
find_package(Fontconfig REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(vocem-config
    src/main.cpp
    src/config_bridge.cpp
)

# qt_standard_project_setup() above also sets an install RPATH of
# $ORIGIN:$ORIGIN/../lib for this directory -- an application-bundle
# convention that would put the packaged /usr/bin/vocem-config's own directory
# and /usr/lib ahead of the loader's search path
# (tests/installed_runpath.cmake). The build tree keeps its own RUNPATH; the
# installed file carries none.
set_target_properties(vocem-config PROPERTIES INSTALL_RPATH "")

# Theme is a singleton: one set of spacings and colours, reachable from every
# component without threading it through as a property.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# Bytecode only, no QML compiled to C++. The C++ that qmlcachegen writes calls
# Qt's private API (QQmlPrivate::AOTCompiledContext, symbol version
# Qt_6_PRIVATE_API), which Qt keeps only within one minor release: Arch rebuilds
# its own packages at every Qt minor, and nobody rebuilds this one, so the next
# Qt could leave the window unable to start. Bytecode is data -- a Qt that does
# not recognise it loads the embedded .qml sources instead -- and costs no
# measurable startup here (tests/window_private_api.cmake).
set_target_properties(vocem-config PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")

qt_add_qml_module(vocem-config
    URI Vocem
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/DebugPage.qml
        qml/InlineMessage.qml
        qml/PlaceholderMessage.qml
        qml/AnchorMark.qml
        qml/AvatarPlaceholder.qml
        qml/Tray.qml
        qml/TrayPage.qml
        qml/TaskbarPreview.qml
        qml/Theme.qml
        qml/Card.qml
        qml/SettingRow.qml
        qml/StateBadge.qml
        qml/ColourButton.qml
        qml/OverlayStage.qml
        qml/SidebarItem.qml
        qml/SectionPage.qml
        qml/ScrollablePage.qml
        qml/PanelPage.qml
        qml/NotificationsPage.qml
        qml/AppearancePage.qml
        qml/SpacingPage.qml
        qml/PeoplePage.qml
        qml/GamesPage.qml
        qml/ApplicationPage.qml
        qml/AboutPage.qml
        qml/SliderRow.qml
        qml/SpinRow.qml
        qml/DisplayPicker.qml
        qml/PanelPreview.qml
        qml/LivePreview.qml
        qml/ScreenMap.qml
        qml/NotificationPreview.qml
        qml/NotificationScreen.qml
    SOURCES src/config_bridge.h
)

# The overlay's own typefaces, in the window that draws a picture of the overlay.
# A preview whose text is a different font is a preview whose boxes end in a
# different place: the panel is as wide as its longest name, so the families and
# the metrics have to be the ones the game will use -- including the emoji, which
# the desktop would otherwise draw from its own colour font, at its own width.
# Same files the atlas is built from, through scripts/make-fonts.py.
qt_add_resources(vocem-config "vocem_overlay_fonts"
    PREFIX "/vocem"
    BASE "${PROJECT_SOURCE_DIR}/third_party/fonts"
    FILES
        "${PROJECT_SOURCE_DIR}/third_party/fonts/Inter-Regular.ttf"
        "${PROJECT_SOURCE_DIR}/third_party/fonts/Inter-SemiBold.ttf"
        "${PROJECT_SOURCE_DIR}/third_party/fonts/NotoEmoji.ttf"
        "${PROJECT_SOURCE_DIR}/third_party/fonts/NotoSansJP.ttf"
)

# The application's own artwork, carried rather than looked up.
#
# These are installed into hicolor and the panel finds them there -- the tray
# icon is a name over D-Bus and the desktop resolves it. Inside this process,
# with the desktop style loaded, QIcon::fromTheme may not find them. What a
# program draws of itself should not depend on the desktop agreeing to find it,
# so IconProvider falls back to these (see main.cpp).
qt_add_resources(vocem-config "vocem_application_icons"
    PREFIX "/vocem/icons"
    BASE "${PROJECT_SOURCE_DIR}/packaging/icons"
    FILES
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay.svg"
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay-offline.svg"
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay-idle.svg"
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay-speaking.svg"
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay-muted.svg"
        "${PROJECT_SOURCE_DIR}/packaging/icons/io.github.ales_drnz.vocem_overlay-deafened.svg"
)

target_include_directories(vocem-config PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    src
)

# vocem_common: the application registry's reader and the palette (theme_for),
# which the window reads through the same objects the overlay draws with.
target_link_libraries(vocem-config PRIVATE vocem_common Qt6::Quick Qt6::QuickControls2
                                           Qt6::Widgets Fontconfig::Fontconfig)

target_compile_options(vocem-config PRIVATE -Wall -Wextra)
target_compile_definitions(vocem-config PRIVATE VOCEM_VERSION="${PROJECT_VERSION}")

install(TARGETS vocem-config RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
