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

#include <fontconfig/fontconfig.h>

#include <algorithm>

#include "vocem/apps.h"
#include "vocem/display.h"

namespace vocem {

// --- the machine's own fonts -------------------------------------------------
//
// The window resolves a family to files; the game only ever opens a path. That
// split is deliberate and it is the whole reason this lives here: fontconfig is
// a library, a cache, a configuration parse and a handful of file syscalls, and
// none of that may happen inside somebody else's renderer -- while this process
// already has Qt, which has fontconfig loaded before it draws its first label.
//
// Only TrueType outlines are offered, and the reason is narrower than it looks.
// ImGui rasterises with stb_truetype, which *does* implement Type 2 charstrings
// (imstb_truetype.h, stbtt__run_charstring) and so reads plain CFF OpenType --
// but not CFF2, which is what a variable .otf carries: Cantarell-VF.otf was
// measured producing an atlas of 0x0 pixels and no glyphs at all. The window
// cannot tell those two apart without rasterising, and fontconfig's format
// string can, so the filter is the conservative one: what is offered is what the
// overlay is measured drawing. A family kept out this way is a family the user
// cannot pick; a family let in wrongly is a setting that appears to do nothing.
inline bool font_file_for(const QString& family, bool bold, QString* file) {
    if (family.isEmpty() || !FcInit()) {
        return false;
    }
    FcPattern* pattern = FcPatternCreate();
    if (!pattern) {
        return false;
    }
    FcPatternAddString(pattern, FC_FAMILY,
                       reinterpret_cast<const FcChar8*>(family.toUtf8().constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddString(pattern, FC_FONTFORMAT, reinterpret_cast<const FcChar8*>("TrueType"));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match || result != FcResultMatch) {
        if (match) {
            FcPatternDestroy(match);
        }
        return false;
    }

    // fontconfig always answers with its best match, so a family that is not
    // installed comes back as whatever is: the answer is only accepted when it
    // is the family that was asked for. Without this check, picking a font that
    // has since been uninstalled would silently draw a different one.
    //
    // Every FC_FAMILY value, not the first: a pattern holds a list, one entry
    // per name the face answers to, and nothing in fontconfig's documentation
    // promises that index 0 of a *matched* pattern is the same string FcFontList
    // reported for the same font. Asking all of them costs a loop over two or
    // three strings and takes the promise out of it. (Measured on this machine
    // before the change: all 267 offered families resolved through index 0
    // alone, so this is a promise made rather than a defect fixed.)
    FcChar8* matched_family = nullptr;
    FcChar8* path = nullptr;
    FcChar8* format = nullptr;
    bool named = false;
    for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &matched_family) == FcResultMatch;
         ++i) {
        if (QString::fromUtf8(reinterpret_cast<const char*>(matched_family))
                .compare(family, Qt::CaseInsensitive) == 0) {
            named = true;
            break;
        }
    }
    const bool filed = FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch;
    const bool formatted =
        FcPatternGetString(match, FC_FONTFORMAT, 0, &format) == FcResultMatch;

    // Which face inside the file, which the overlay has no way to ask for: it
    // opens what is there and stb_truetype reads face 0. fontconfig packs two
    // things into FC_INDEX -- the face number in the low half, the named
    // instance of a variable font in the high half -- and only the low half is a
    // difference the overlay would draw wrong. A collection whose second face
    // carries the chosen family would come back as file X, face 1, and the game
    // would draw face 0 of X: the window naming one family while the overlay
    // draws another, which is the defect this whole resolution exists to avoid.
    // Refused rather than shipped.
    //
    // A named instance is accepted, and what gets drawn is the file's default
    // instance. Measured on this machine: of 534 resolutions (267 families,
    // both weights) exactly one lands anywhere but face 0 -- Adwaita Sans bold,
    // instance 7 of AdwaitaSans-Regular.ttf, which is why the bold of a variable
    // family comes out at the regular weight and why the comment on
    // ConfigBridge::setFontFamily says what it says.
    int face = 0;
    FcPatternGetInteger(match, FC_INDEX, 0, &face);
    const bool whole_file = (face & 0xFFFF) == 0;

    bool ok = false;
    if (named && filed && formatted && whole_file &&
        qstrcmp(reinterpret_cast<const char*>(format), "TrueType") == 0) {
        *file = QString::fromUtf8(reinterpret_cast<const char*>(path));
        ok = true;
    }
    FcPatternDestroy(match);
    return ok;
}

// Every family with a TrueType face, sorted, once per run of the window.
inline const QStringList& installed_font_families() {
    static const QStringList families = [] {
        QStringList found;
        if (!FcInit()) {
            return found;
        }
        FcPattern* pattern = FcPatternCreate();
        FcObjectSet* wanted = FcObjectSetBuild(FC_FAMILY, static_cast<char*>(nullptr));
        if (!pattern || !wanted) {
            // One of the two may have been made: fontconfig's own objects are
            // reference-counted and neither destroy accepts null, so each is
            // asked for separately.
            if (pattern) {
                FcPatternDestroy(pattern);
            }
            if (wanted) {
                FcObjectSetDestroy(wanted);
            }
            return found;
        }
        FcPatternAddString(pattern, FC_FONTFORMAT, reinterpret_cast<const FcChar8*>("TrueType"));
        FcPatternAddBool(pattern, FC_OUTLINE, FcTrue);
        FcFontSet* set = FcFontList(nullptr, pattern, wanted);
        if (set) {
            for (int i = 0; i < set->nfont; ++i) {
                FcChar8* name = nullptr;
                if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &name) == FcResultMatch) {
                    const QString family = QString::fromUtf8(reinterpret_cast<const char*>(name));
                    if (!family.isEmpty() && !found.contains(family)) {
                        found.append(family);
                    }
                }
            }
            FcFontSetDestroy(set);
        }
        FcObjectSetDestroy(wanted);
        FcPatternDestroy(pattern);
        found.sort(Qt::CaseInsensitive);
        return found;
    }();
    return families;
}

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

// The same measurement for a family the user chose. It cannot be the constant
// it used to be: ascent-minus-descent against the em square is a property of
// the file, so every preview drawn in somebody else's font would otherwise be
// laid out against Inter's proportion and end up the wrong width -- which is
// exactly the divergence this ratio exists to close. Cached per family, because
// a QML binding asks for it on every evaluation.
inline qreal overlay_font_ratio_for(const QString& family) {
    if (family.isEmpty()) {
        return overlay_fonts().ratio;
    }
    static QHash<QString, qreal> measured;
    const auto found = measured.constFind(family);
    if (found != measured.constEnd()) {
        return found.value();
    }
    QFont font(family);
    font.setPixelSize(1000);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF metrics(font);
    const qreal extent = metrics.ascent() + metrics.descent();
    const qreal ratio = extent > 1.0 ? 1000.0 / extent : overlay_fonts().ratio;
    measured.insert(family, ratio);
    return ratio;
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
    bool operator==(const DisplayMode& other) const {
        return name == other.name && width == other.width && height == other.height;
    }
    bool operator!=(const DisplayMode& other) const { return !(*this == other); }
};

inline QString drm_root() {
    const QByteArray root = qgetenv("VOCEM_DRM_ROOT");
    return root.isEmpty() ? QStringLiteral("/sys/class/drm") : QString::fromLocal8Bit(root);
}

// Every connected, enabled connector with a readable mode, sorted by name --
// through display.h's one reader, which is the daemon's too. This file carried
// a second reader in Qt, and the two had disagreed once about `enabled` (entry
// 135); the shape of the tree is known in one place now.
inline QList<DisplayMode> read_displays() {
    QList<DisplayMode> found;
    vocem::DisplayModeInfo modes[vocem::kMaxDisplayModes];
    const int count =
        vocem::read_display_modes(drm_root().toLocal8Bit().constData(), modes, vocem::kMaxDisplayModes);
    for (int i = 0; i < count; ++i) {
        found.append({QString::fromLocal8Bit(modes[i].name), static_cast<int>(modes[i].width),
                      static_cast<int>(modes[i].height)});
    }
    std::sort(found.begin(), found.end(),
              [](const DisplayMode& a, const DisplayMode& b) { return a.name < b.name; });
    return found;
}

// The three answers below are remembered between reads, and forgotten on the
// window's slow tick.
//
// Remembered, because a QML binding asks for each of them every time the state
// changes -- twice a second -- and a /sys walk per binding per tick is not what
// this window should spend its time on. NOT answered once, which is what they
// used to be: this is a tray application that starts hidden at login and stays
// up for the session, so "once" meant "at login". Meanwhile the daemon re-reads
// the same tree every sixty seconds and republishes, and running games follow
// it -- so after a monitor was plugged in the overlay resized and the window
// that is supposed to be a picture of it went on drawing the old display,
// captioning the old resolution, and not offering the new one in "Map shows",
// until somebody quit the tray icon and opened it again. Measured against the
// packaged 0.1.4-1 with a 1280x1024 tree and a 3840x2160 connector added 1.6 s
// after the window was up: every map stayed 5:4 for the rest of the run, where
// the same two displays present at startup gave 16:9. The reader was right; the
// remembering was the defect (entry 63's shape, in the instrument the owner uses
// to place the overlay).
//
// All three together, so one call forgets all of them: two facts about one
// display refreshed a tick apart is a window disagreeing with itself.
namespace detail {

struct RememberedDisplays {
    QList<DisplayMode> connected;
    uint32_t sizing_height = 0;
    QString resolution;
};

inline RememberedDisplays& remembered_displays() {
    static RememberedDisplays remembered;
    return remembered;
}

}  // namespace detail

// Ask the kernel again the next time each of them is read.
//
// The cadence belongs to the caller and ConfigBridge puts it on its four-second
// sweep, not on the twice-a-second tick. Measured before choosing: one
// read_displays() against this machine's /sys/class/drm (four connectors, one
// connected) costs 141 us, twice over 200 runs -- so four seconds is thirty-five
// parts in a million of the window's time, sooner than anybody can plug a
// monitor in and look at the settings, and fifteen times more attentive than the
// daemon, which calls the same question every sixty seconds.
inline void forget_displays() {
    detail::remembered_displays() = detail::RememberedDisplays{};
}

inline const QList<DisplayMode>& displays() {
    // A display that is asleep reports itself disconnected with no modes at all
    // -- which is what a run with the monitor blanked found, and what a caption
    // of "0 × 0" came from -- so an empty answer is asked again on the next read
    // rather than kept until the sweep.
    QList<DisplayMode>& connected = detail::remembered_displays().connected;
    if (connected.isEmpty()) {
        connected = read_displays();
    }
    return connected;
}

// The height the overlay is actually sized for: the largest connected mode,
// through the same reader the daemon publishes from (vocem/display.h), under
// the same overridable root as the enumeration above. Zero -- a VM, a headless
// run -- is asked again on the next read, like the caption.
inline uint32_t overlay_display_height() {
    uint32_t& height = detail::remembered_displays().sizing_height;
    if (height == 0) {
        height = vocem::display_height_under(drm_root().toLocal8Bit().constData());
    }
    return height;
}

// The display the overlay is sized for: the tallest connected mode, which is
// what display.h picks and what the daemon publishes. Zero-sized where nothing
// can be read.
//
// The enumeration is sorted by connector name, and the first entry of it was
// what the maps used to caption themselves with -- so on a machine whose
// tallest display is not its alphabetically first, the automatic map named one
// display and drew the boxes at the size they take on another. The number the
// overlay is actually sized from has one source; so should the picture of it.
inline DisplayMode sizing_display() {
    DisplayMode best;
    for (const DisplayMode& display : displays()) {
        if (display.height > best.height) {
            best = display;
        }
    }
    return best;
}

// The shape of that display, or 0 where no mode can be read -- the automatic
// map's aspect, which used to come from `Screen`: the screen this window
// happens to be on, which on a laptop beside a 16:9 monitor is neither the
// display the overlay is sized for nor the one the caption names.
inline qreal sizing_display_aspect() {
    const DisplayMode sizing = sizing_display();
    return sizing.height > 0 ? static_cast<qreal>(sizing.width) / sizing.height : 0.0;
}

// The resolution caption on the maps: the display the overlay is sized for,
// which is the one the automatic map stands for. Falls back to Qt's logical
// size where sysfs is not readable.
inline QString screen_resolution() {
    QString& resolution = detail::remembered_displays().resolution;
    if (resolution.isEmpty()) {
        const DisplayMode sizing = sizing_display();
        if (sizing.height > 0) {
            resolution = QObject::tr("%1 × %2").arg(sizing.width).arg(sizing.height);
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
    // The XDG data roots, spelled once for the project (apps.h): this was the
    // third copy of the enumeration.
    for (const std::string& root : vocem::detail::desktop_roots()) {
        const QString manifest = QString::fromStdString(root) +
                                 QStringLiteral("/vulkan/implicit_layer.d/VkLayer_vocem_overlay.json");
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
//
// Two halves, asked differently. This process's own environment is a string
// compare and is answered here at once. The user manager's is a `systemctl`
// spawn, and that is NOT asked here: it used to be, synchronously with a
// three-second cap, behind a CONSTANT property the Debug page reads while the
// window is being built -- so the first frame waited on systemctl. ConfigBridge
// starts that spawn asynchronously and publishes the answer when it arrives
// (openglPreloadKnown); the arguments it runs are these, so the two stay one
// question.
inline bool opengl_preload_in_own_environment() {
    return qgetenv("LD_PRELOAD").contains("vocem_gl_shim");
}
inline QStringList opengl_preload_probe_arguments() {
    return {QStringLiteral("--user"), QStringLiteral("show-environment")};
}
inline bool opengl_preload_in_manager_output(const QByteArray& show_environment) {
    return show_environment.contains("vocem_gl_shim");
}

// Whether systemd would find a vocemd.service for the user manager, read from
// the directories systemd.unit(5) lists for user units rather than asked of
// systemctl. It is the answer ConfigBridge falls back on when
// `systemctl --user cat` does not answer inside its cap -- a busy login, a
// manager still starting -- because "no answer" read as "no unit" made the
// window exec a vocemd of its own beside the one the unit was about to start.
// The runtime directories (transient units, generators) are left out: nothing
// puts this unit there.
inline bool daemon_unit_on_disk() {
    const auto env_or = [](const char* name, const QString& fallback) {
        const QByteArray value = qgetenv(name);
        return value.isEmpty() ? fallback : QString::fromLocal8Bit(value);
    };
    const QString home = QDir::homePath();
    QStringList roots;
    roots << env_or("XDG_CONFIG_HOME", home + QStringLiteral("/.config")) + QStringLiteral("/systemd/user");
    for (const QString& dir : env_or("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg"))
                                  .split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        roots << dir + QStringLiteral("/systemd/user");
    }
    roots << QStringLiteral("/etc/systemd/user");
    roots << env_or("XDG_DATA_HOME", home + QStringLiteral("/.local/share")) + QStringLiteral("/systemd/user");
    for (const QString& dir : env_or("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share"))
                                  .split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        roots << dir + QStringLiteral("/systemd/user");
    }
    roots << QStringLiteral("/usr/local/lib/systemd/user") << QStringLiteral("/usr/lib/systemd/user");
    for (const QString& root : roots) {
        if (QFileInfo::exists(root + QStringLiteral("/vocemd.service"))) {
            return true;
        }
    }
    return false;
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
    // Quoted the way the specification's Exec key wants an argument quoted
    // (apps.h, desktop_exec_quoted): written bare, a path with a space in it
    // was two arguments and a path with a `%` in it a field code.
    const QString executable = QString::fromStdString(
        vocem::detail::desktop_exec_quoted(QCoreApplication::applicationFilePath().toStdString()));
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
