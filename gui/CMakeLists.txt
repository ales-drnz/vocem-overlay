# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

# Widgets is not used directly: it is what the platform theme needs behind the
# system tray icon on Plasma. See the note in main.cpp.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Widgets)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(vocem-config
    src/main.cpp
    src/config_bridge.cpp
)

# Theme is a singleton: one set of spacings and colours, reachable from every
# component without threading it through as a property.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

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
# icon is a name over D-Bus and the desktop resolves it. Inside this process it
# is a different question, and the answer was no: measured, with the desktop
# style loaded, QIcon::fromTheme resolves every Breeze name the window asks for
# and none of these six, so the About page has been drawing an empty square
# where its icon goes and the tray preview would have drawn five of them. What
# a program draws of itself should not depend on the desktop agreeing to find
# it, so IconProvider falls back to these.
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

target_link_libraries(vocem-config PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)

target_compile_options(vocem-config PRIVATE -Wall -Wextra)
target_compile_definitions(vocem-config PRIVATE VOCEM_VERSION="${PROJECT_VERSION}")

install(TARGETS vocem-config RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
