// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the window can find out about the machine it is running on.
//
// None of this is a setting and none of it is state the daemon publishes: it is
// the typeface the previews have to draw with, the icons the desktop happens to
// carry, the resolution of the display, and whether the two halves of the overlay
// are actually installed. It sat in the middle of ConfigBridge, between the
// setters, which made a file about settings twice as long and half as clear.
//
// ConfigBridge keeps the QML-facing properties; each one is a line that calls in
// here.

#ifndef VOCEM_ENVIRONMENT_H
#define VOCEM_ENVIRONMENT_H

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QTextStream>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>
#include <QScreen>
#include <QString>
#include <QStringList>

#include <algorithm>

#include "vocem/display.h"

namespace vocem {

// The overlay's typeface, registered once so the previews can draw with it.
//
// The panel is as wide as its longest name, so a preview using the desktop's font
// puts every box it draws in the wrong place. These are the same two files
// scripts/make-fonts.py compiles into the in-game atlas.
struct OverlayFonts {
    QString body;
    QString strong;
    // The emoji and the punctuation Inter has none of, as families Qt can fall
    // through to. The atlas merges the same two into each weight; here they are a
    // list, because that is how Qt is told about a fallback.
    QString emoji;
    QString punctuation;
    // What to multiply the overlay's font size by to get the pixel size Qt has to
    // be given for the same glyphs.
    //
    // The two measure a font size differently. ImGui asks stb_truetype to scale the
    // face so that ascent minus descent comes to the size it asked for, while Qt's
    // pixelSize is the em square. For Inter those differ by about a fifth, so a
    // preview asking Qt for "16 pixels" drew noticeably larger text than the game
    // did at 16 -- which then made every box that ends where its text ends too
    // wide. Measured here rather than written down: it is a property of the font
    // file, and the file can be replaced.
    qreal ratio = 1.0;
};

inline const OverlayFonts& overlay_fonts() {
    static const OverlayFonts fonts = [] {
        OverlayFonts loaded;
        const int body = QFontDatabase::addApplicationFont(QStringLiteral(":/vocem/Inter-Regular.ttf"));
        const int strong =
            QFontDatabase::addApplicationFont(QStringLiteral(":/vocem/Inter-SemiBold.ttf"));
        const int emoji = QFontDatabase::addApplicationFont(QStringLiteral(":/vocem/NotoEmoji.ttf"));
        const int punctuation =
            QFontDatabase::addApplicationFont(QStringLiteral(":/vocem/NotoSansJP.ttf"));
        const QStringList body_families = QFontDatabase::applicationFontFamilies(body);
        const QStringList strong_families = QFontDatabase::applicationFontFamilies(strong);
        const QStringList emoji_families = QFontDatabase::applicationFontFamilies(emoji);
        const QStringList punctuation_families =
            QFontDatabase::applicationFontFamilies(punctuation);
        loaded.body = body_families.isEmpty() ? QString() : body_families.first();
        loaded.strong = strong_families.isEmpty() ? loaded.body : strong_families.first();
        loaded.emoji = emoji_families.isEmpty() ? QString() : emoji_families.first();
        loaded.punctuation =
            punctuation_families.isEmpty() ? QString() : punctuation_families.first();

        // At a large size, so the integer rounding Qt applies to the two metrics is
        // a thousandth rather than a twentieth.
        QFont font(loaded.body);
        font.setPixelSize(1000);
        font.setHintingPreference(QFont::PreferNoHinting);
        const QFontMetricsF metrics(font);
        const qreal extent = metrics.ascent() + metrics.descent();
        loaded.ratio = extent > 1.0 ? 1000.0 / extent : 1.0;
        return loaded;
    }();
    return fonts;
}

// The first of these names the session's icon theme actually has, so a window
// on a theme without Breeze's names is not left with holes in it. The last is
// returned unconditionally, so the caller still gets a name to fall over on.
inline QString theme_icon(const QStringList& names) {
    for (const QString& name : names) {
        if (QIcon::hasThemeIcon(name)) {
            return name;
        }
    }
    return names.isEmpty() ? QString() : names.constLast();
}

// A generic person from the icon theme, for the previews. Which name a theme
// carries differs between themes, so it is looked up rather than written down.
inline QString generic_avatar() {
    return QStringLiteral("image://icon/") +
           theme_icon({QStringLiteral("user-identity"), QStringLiteral("avatar-default"),
                 QStringLiteral("user")});
}

// The displays, as the maps depict them.
//
// Not from Qt. On a fractionally scaled Wayland session Screen.width is logical --
// 2560 on the 3840-wide panel this was written on -- and the device pixel ratio
// that would put it back is rounded to a whole number by the compositor protocol,
// so multiplying the two overshoots by a third. What the game will actually render
// at is the mode the display is in, and the kernel says what that is.
//
// One enumeration for everything the window says about displays -- the caption,
// the per-map dropdown -- because two /sys walks would be two answers that can
// disagree. The root is /sys/class/drm unless VOCEM_DRM_ROOT points elsewhere,
// the same override the tests fabricate a tree under (display.h's reader takes
// the root as a parameter for the same reason).
struct DisplayMode {
    QString name;  // the connector, card<N>- prefix stripped: "DP-2", "HDMI-A-1"
    int width = 0;
    int height = 0;
};

inline QString drm_root() {
    const QByteArray root = qgetenv("VOCEM_DRM_ROOT");
    return root.isEmpty() ? QStringLiteral("/sys/class/drm") : QString::fromLocal8Bit(root);
}

// Every connected, enabled connector with a readable mode, sorted by name.
inline QList<DisplayMode> read_displays() {
    QList<DisplayMode> found;
    QDir drm(drm_root());
    QStringList connectors = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    connectors.sort();
    for (const QString& connector : connectors) {
        // Connectors are card<N>-<name>; the bare card<N> and renderD* are not --
        // the same shape display.h's reader requires.
        const qsizetype dash = connector.indexOf(QLatin1Char('-'));
        if (!connector.startsWith(QStringLiteral("card")) || dash <= 0) {
            continue;
        }
        const QString base = drm.filePath(connector);
        QFile status(base + QStringLiteral("/status"));
        QFile enabled(base + QStringLiteral("/enabled"));
        QFile modes(base + QStringLiteral("/modes"));
        if (!status.open(QIODevice::ReadOnly) || !enabled.open(QIODevice::ReadOnly) ||
            !modes.open(QIODevice::ReadOnly)) {
            continue;
        }
        if (status.readAll().trimmed() != "connected" ||
            enabled.readAll().trimmed() != "enabled") {
            continue;
        }
        // The first line is the mode in use; the rest are the other ones the
        // display would accept.
        const QString mode = QString::fromLatin1(modes.readLine()).trimmed();
        const QStringList parts = mode.split('x');
        if (parts.size() == 2 && parts[0].toInt() > 0 && parts[1].toInt() > 0) {
            found.append({connector.mid(dash + 1), parts[0].toInt(), parts[1].toInt()});
        }
    }
    std::sort(found.begin(), found.end(),
              [](const DisplayMode& a, const DisplayMode& b) { return a.name < b.name; });
    return found;
}

inline const QList<DisplayMode>& displays() {
    // Answered once and remembered. A display that is asleep reports itself
    // disconnected with no modes at all -- which is what a run with the monitor
    // blanked found, and what a caption of "0 × 0" came from -- so an empty answer
    // is asked again on the next tick rather than kept.
    static QList<DisplayMode> connected;
    if (connected.isEmpty()) {
        connected = read_displays();
    }
    return connected;
}

// The height the overlay is actually sized for: the largest connected mode,
// through the same reader the daemon publishes from (vocem/display.h), under
// the same overridable root as the enumeration above. Zero -- a VM, a headless
// run -- is asked again on the next tick, like the caption.
inline uint32_t overlay_display_height() {
    static uint32_t height = 0;
    if (height == 0) {
        height = vocem::display_height_under(drm_root().toLocal8Bit().constData());
    }
    return height;
}

// The resolution caption on the maps: the first display of the enumeration,
// which is the display Qt calls primary on every single-monitor machine. With
// several, the dropdown beside the map is what names the others. Falls back to
// Qt's logical size where sysfs is not readable.
inline QString screen_resolution() {
    static QString resolution;
    if (resolution.isEmpty()) {
        const QList<DisplayMode>& connected = displays();
        if (!connected.isEmpty()) {
            resolution = QObject::tr("%1 × %2")
                             .arg(connected.first().width)
                             .arg(connected.first().height);
        } else {
            // Qt's logical size is the wrong number -- see the note above -- but
            // it is the right order of magnitude, and better than no caption.
            const QScreen* screen = QGuiApplication::primaryScreen();
            if (screen && screen->geometry().width() > 0) {
                resolution = QObject::tr("%1 × %2")
                                 .arg(screen->geometry().width())
                                 .arg(screen->geometry().height());
            }
        }
    }
    return resolution;
}

// The loader only reads implicit layer manifests from a fixed set of directories,
// so looking for ours in them answers "is the Vulkan half of this actually
// installed" without running a game to find out. XDG_DATA_DIRS is honoured because
// a --prefix=~/.local install is a supported way to have this.
inline bool vulkan_layer_installed() {
    QStringList roots;
    const QByteArray home = qgetenv("XDG_DATA_HOME");
    roots << (home.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share")
                             : QString::fromLocal8Bit(home));
    const QByteArray dirs = qgetenv("XDG_DATA_DIRS");
    const QString list = dirs.isEmpty() ? QStringLiteral("/usr/local/share:/usr/share")
                                        : QString::fromLocal8Bit(dirs);
    roots << list.split(QLatin1Char(':'), Qt::SkipEmptyParts);

    for (const QString& root : std::as_const(roots)) {
        const QString manifest =
            root + QStringLiteral("/vulkan/implicit_layer.d/VkLayer_vocem_overlay.json");
        if (QFileInfo::exists(manifest)) {
            return true;
        }
    }
    return false;
}

// The OpenGL side has no loader to ask: it is there only if something preloaded
// it, which is what the environment.d file arranges. Two places have to be
// checked, and the difference between them is real -- measured on this machine,
// the user manager had the preload while plasmashell, started before the package
// was installed, did not:
//
//   * our own environment, which is what a game started from this window or from
//     the same shell would inherit;
//   * the systemd user manager's environment, which is what anything launched by
//     the session from now on will get.
//
// Either one means OpenGL games are covered; neither means the session has not
// picked up the file yet, and only logging out fixes that.
inline bool opengl_preload_active() {
    if (qgetenv("LD_PRELOAD").contains("vocem_gl_shim")) {
        return true;
    }
    QProcess environment;
    environment.start(QStringLiteral("systemctl"),
                      {QStringLiteral("--user"), QStringLiteral("show-environment")});
    if (!environment.waitForFinished(3000)) {
        return false;
    }
    return environment.readAllStandardOutput().contains("vocem_gl_shim");
}

// The desktop entry that starts this window with the session.
//
// The freedesktop Autostart specification: an entry under
// $XDG_CONFIG_HOME/autostart, run by the desktop at login. Written rather than
// installed, because it is a per-user answer and not part of the package -- and
// removed rather than emptied, since an entry with Hidden=true is a file that
// stays behind arguing with the next one somebody writes.
//
// It starts hidden: the point is to have the tray icon there from login, not a
// settings window in everybody's face.
inline QString autostart_entry_path() {
    const QByteArray home = qgetenv("XDG_CONFIG_HOME");
    const QString root = home.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                        : QString::fromLocal8Bit(home);
    return root + QStringLiteral("/autostart/io.github.ales_drnz.vocem_overlay.desktop");
}

inline bool autostart_enabled() { return QFile::exists(autostart_entry_path()); }

inline void set_autostart(bool enabled) {
    const QString path = autostart_entry_path();
    if (!enabled) {
        QFile::remove(path);
        return;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    // The executable by absolute path: a session's PATH is not this shell's, and
    // an entry that names a program the desktop cannot find fails silently.
    const QString executable = QCoreApplication::applicationFilePath();
    QTextStream out(&file);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Vocem Overlay\n"
        << "Comment=Discord voice overlay, in the tray\n"
        << "Exec=" << executable << " --hidden\n"
        << "Icon=io.github.ales_drnz.vocem_overlay\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
}

}  // namespace vocem

#endif  // VOCEM_ENVIRONMENT_H
