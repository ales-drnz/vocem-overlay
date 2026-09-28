// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocem-config: the configuration window.
//
// The Qt Quick style is set to org.kde.desktop when it is available, so the window
// follows the system theme instead of looking like a foreign application. On other
// desktops Qt falls back to its own style, which is still consistent with the rest
// of the session.

#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QPalette>
#include <QStyleHints>
#include <QColor>

#include <unistd.h>

#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QQuickImageProvider>
#include <QHash>
#include <QMetaProperty>
#include <QSet>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>
#include <QQuickItem>
#include <QTextStream>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config_bridge.h"

namespace {

// One window per user, not one per launch.
//
// Closing the window leaves the process running behind its tray icon, so starting
// the application again -- from the menu, from the tray's own entry, from anything
// -- must not add a second process and a second icon to the tray. The first
// instance listens on a socket in the runtime directory; later ones find it, ask
// it to show itself, and exit.
//
// $XDG_RUNTIME_DIR is the right place for the socket: it is per-user, mode 0700,
// and cleared when the session ends, so a stale socket cannot outlive a reboot.
// The path is absolute because a bare name given to QLocalServer is resolved
// against QDir::tempPath(), where any local user could connect and make this
// window appear on somebody's screen. Without a runtime directory there is
// nowhere better than the bare name, and this falls back to it rather than
// inventing one.
QString instance_socket_name() {
    const QString name = QStringLiteral("vocem-config-%1").arg(::getuid());
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        return name;
    }
    return QString::fromLocal8Bit(runtime) + '/' + name;
}

// The lock that decides which instance is THE instance, taken before anything
// else is built. The socket alone cannot decide it: the listen happens after the
// window is built, which takes a good fraction of a second, so two launches
// inside that time (the autostart at login and a menu click, say) would each
// find nobody listening (entry 140). A QLockFile is taken in microseconds,
// before the engine loads; the loser asks the winner to show itself, waiting for
// the winner's socket to appear if it has not yet. Beside the socket, for the
// socket's reasons.
QString instance_lock_name() { return instance_socket_name() + QStringLiteral(".lock"); }

// What an item is, by the QML type it was declared as: a control by the Qt
// Quick Controls type it derives from (the desktop style's Switch is a
// QQuickSwitch underneath), anything else by its own type, the "_QMLTYPE_n"
// suffix Qt gives a QML-declared type taken off.
QString type_name(const QObject* object) {
    static const char* const kControls[][2] = {
        {"QQuickSwitch", "Switch"},     {"QQuickCheckBox", "CheckBox"},
        {"QQuickRadioButton", "RadioButton"}, {"QQuickSlider", "Slider"},
        {"QQuickSpinBox", "SpinBox"},   {"QQuickComboBox", "ComboBox"},
        {"QQuickTextField", "TextField"}, {"QQuickButton", "Button"},
    };
    for (const auto& control : kControls) {
        if (object->inherits(control[0])) {
            return QString::fromLatin1(control[1]);
        }
    }
    QString name = QString::fromLatin1(object->metaObject()->className());
    const int cut = name.indexOf(QStringLiteral("_QMLTYPE_"));
    if (cut > 0) {
        name.truncate(cut);
    }
    return name;
}

// The geometry of everything the previews draw, as numbers.
//
// The companion of tests/vocem_panel_geometry, which measures the real overlay the
// same way, so the two geometries can be compared as numbers rather than judged
// from screenshots. Every item that stands for something the overlay draws
// carries an objectName, and this walks the scene and prints where each one
// actually ended up.
//
// A development aid, like the screenshot path below it, and inert without the
// environment variable.
void dump_item(QQuickItem* item, const QString& path, QHash<QString, int>& seen, QTextStream& out) {
    QString here = path;
    if (!item->objectName().isEmpty()) {
        here = path.isEmpty() ? item->objectName() : path + '/' + item->objectName();
        // Several rows carry the same name; the suffix keeps them apart in the
        // order the layout put them in.
        const int index = seen[here]++;
        const QString key = index == 0 ? here : QStringLiteral("%1#%2").arg(here).arg(index);
        // In the window's own pixels, transforms included: the previews are drawn
        // at the overlay's reference size and scaled as a whole, so an item's own
        // width is in a unit that depends on where it sits.
        const QRectF box = item->mapRectToItem(nullptr, QRectF(0.0, 0.0, item->width(),
                                                              item->height()));

        out << "{\"item\": \"" << key << "\", \"x\": " << box.x() << ", \"y\": " << box.y()
            << ", \"w\": " << box.width() << ", \"h\": " << box.height()
            << ", \"visible\": " << (item->isVisible() ? "true" : "false");
        // The figures the preview computed for itself, so a divergence can be
        // traced to the formula rather than guessed at from a rectangle.
        for (const char* name : {"uiScale", "fontPixels", "textPixels", "rowHeight", "rowSize",
                                 "paddingX", "paddingY", "avatarGap", "avatarDiameter",
                                 "pictureSize", "firstRowY", "overlayScale", "inset",
                                 "implicitWidth", "implicitHeight", "contentWidth", "factor",
                                 // What the preset previews derived, so a check can
                                 // hold each one to a distinct surface -- and to a
                                 // distinct picture, which is not the same claim:
                                 // two presets can share a surface and an opacity
                                 // and draw them in two different places.
                                 "surfaceRgb", "presetOpacity", "presetBox",
                                 // The strength a preview's picture is drawn at,
                                 // which the overlay quiets for whoever is not
                                 // talking: an opacity is invisible to a rectangle.
                                 "pictureOpacity",
                                 // Whether the keyboard can reach it: a bool
                                 // converts to a number, so "every control is
                                 // reachable without a mouse" can be checked.
                                 "activeFocusOnTab",
                                 // Which display the "Map shows" dropdown is
                                 // pointing at, and how many it offers: an
                                 // index is invisible to a rectangle.
                                 "pickerIndex", "pickerCount",
                                 // The same for the font box: the row it is
                                 // on, and the row of the family the settings
                                 // name, which must stay together.
                                 "fontIndex", "fontSetIndex",
                                 // How many of a preview's own pictures actually
                                 // came up. An Image that failed to load keeps the
                                 // size its layout gave it and paints nothing, so
                                 // a rectangle cannot tell a drawn icon from a
                                 // missing one.
                                 "loadedIcons",
                                 // Every named item's strength: a switch that
                                 // takes the overlay off the screen shows in a
                                 // map only as an opacity.
                                 "opacity"}) {
            const QVariant value = item->property(name);
            if (value.isValid() && value.canConvert<qreal>()) {
                out << ", \"" << name << "\": " << value.toReal();
            }
        }
        // Last on the line: the checks match "item", "x" ... "visible" in
        // that order.
        out << ", \"type\": \"" << type_name(item) << "\"}\n";
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        dump_item(child, here, seen, out);
    }
}

// What each control DOES, for the tests that hold a control to its promise
// rather than to its spelling in the QML (VOCEM_CONFIG_DRIVE=1, beside the
// geometry dump; inert without it).
//
// Every visible Switch and CheckBox on the page is clicked as a user clicks
// it -- toggle(), then the toggled() signal the page's handler listens to --
// and clicked back. Every Slider and SpinBox is moved to its `to` and to its
// `from` -- the value set, then moved() or valueModified() -- and back to
// where it was. For each, one line says which of ConfigBridge's writable
// settings the control changed ("drives"), the setting's value at either end
// of a slider ("atFrom", "atTo", after the bridge's clamp), and whether
// config.ini changed on disk before anything else ran ("writesAtOnce"): a
// switch is for a setting written on the click, anything under an Apply bar
// waits for it. Each control once per run, on the first section it is
// visible on; the header's switches are visible on all of them.
QVariantMap writable_settings(const QObject* bridge) {
    QVariantMap settings;
    const QMetaObject* meta = bridge->metaObject();
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (property.isWritable()) {
            settings.insert(QString::fromLatin1(property.name()), property.read(bridge));
        }
    }
    return settings;
}

QStringList changed_settings(const QVariantMap& before, const QVariantMap& after) {
    QStringList names;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (before.value(it.key()) != it.value()) {
            names.append(it.key());
        }
    }
    return names;
}

QByteArray settings_file_bytes() {
    QFile file(QString::fromStdString(vocem::Config::path()));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<none>");
}

// Where a control sits, by the names of the items around it, and what it is
// called: its own text, a SliderRow's accessibleName, or the label of the
// SettingRow it sits in.
QString control_path(QQuickItem* item) {
    QStringList names;
    for (QQuickItem* at = item; at; at = at->parentItem()) {
        if (!at->objectName().isEmpty()) {
            names.prepend(at->objectName());
        }
    }
    return names.join(QLatin1Char('/'));
}

QString control_label(QQuickItem* item) {
    for (QQuickItem* at = item; at; at = at->parentItem()) {
        const QVariant name = at->property("accessibleName");
        if (name.isValid() && !name.toString().isEmpty()) {
            return name.toString();
        }
        const QVariant text = at->property("text");
        if (at == item && text.isValid() && !text.toString().isEmpty()) {
            return text.toString();
        }
        const QVariant label = at->property("label");
        if (label.isValid() && label.userType() == QMetaType::QString &&
            !label.toString().isEmpty()) {
            return label.toString();
        }
    }
    return {};
}

void collect_controls(QQuickItem* item, QList<QQuickItem*>& controls) {
    if (!item->isVisible()) {
        return;
    }
    const QString type = type_name(item);
    if (type == QLatin1String("Switch") || type == QLatin1String("CheckBox") ||
        type == QLatin1String("Slider") || type == QLatin1String("SpinBox")) {
        controls.append(item);
        return;
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        collect_controls(child, controls);
    }
}

QString json_text(const QString& text) {
    QString out = text;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return out;
}

void drive_controls(QQuickWindow* window, int section, QSet<QQuickItem*>& done, QTextStream& out) {
    auto* bridge = window->findChild<ConfigBridge*>();
    if (!bridge || !window->contentItem()) {
        return;
    }
    QList<QQuickItem*> controls;
    collect_controls(window->contentItem(), controls);
    for (QQuickItem* control : controls) {
        if (done.contains(control)) {
            continue;
        }
        done.insert(control);
        const QString type = type_name(control);
        out << "{\"control\": \"" << type << "\", \"section\": " << section << ", \"path\": \""
            << json_text(control_path(control)) << "\", \"label\": \""
            << json_text(control_label(control)) << '"';
        const QVariantMap before = writable_settings(bridge);
        const QByteArray file_before = settings_file_bytes();
        QStringList drives;
        bool writes = false;
        if (type == QLatin1String("Switch") || type == QLatin1String("CheckBox")) {
            QMetaObject::invokeMethod(control, "toggle");
            QMetaObject::invokeMethod(control, "toggled");
            writes = settings_file_bytes() != file_before;
            drives = changed_settings(before, writable_settings(bridge));
            QMetaObject::invokeMethod(control, "toggle");
            QMetaObject::invokeMethod(control, "toggled");
        } else {
            const bool spin = type == QLatin1String("SpinBox");
            const char* moved = spin ? "valueModified" : "moved";
            const QVariant from = control->property("from");
            const QVariant to = control->property("to");
            const QVariant was = control->property("value");
            out << ", \"from\": " << from.toReal() << ", \"to\": " << to.toReal();
            if (!spin) {
                out << ", \"stepSize\": " << control->property("stepSize").toReal();
            }
            control->setProperty("value", to);
            QMetaObject::invokeMethod(control, moved);
            writes = settings_file_bytes() != file_before;
            const QVariantMap at_to = writable_settings(bridge);
            control->setProperty("value", from);
            QMetaObject::invokeMethod(control, moved);
            const QVariantMap at_from = writable_settings(bridge);
            drives = changed_settings(before, at_to);
            for (const QString& name : changed_settings(before, at_from)) {
                if (!drives.contains(name)) {
                    drives.append(name);
                }
            }
            if (drives.size() == 1) {
                out << ", \"atFrom\": " << at_from.value(drives.first()).toReal()
                    << ", \"atTo\": " << at_to.value(drives.first()).toReal();
            }
            control->setProperty("value", was);
            QMetaObject::invokeMethod(control, moved);
        }
        out << ", \"drives\": \"" << drives.join(QLatin1Char(',')) << "\", \"writesAtOnce\": "
            << (writes ? "true" : "false") << "}\n";
    }
}

// Theme icons for the things Qt Quick does not draw itself.
//
// A Button takes an icon.name and the style finds the artwork; a plain Image has
// no such route, and the window needs one for the pictures that are not on a
// control -- the application's own icon, a warning beside a state, the generic
// person the previews use for an avatar. Everything under image://icon/ is a
// freedesktop icon name.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requested) override {
        const int width = requested.width() > 0 ? requested.width() : 22;
        const int height = requested.height() > 0 ? requested.height() : width;
        QPixmap pixmap = QIcon::fromTheme(id).pixmap(width, height);
        // The application's own artwork, carried in the binary, for when the
        // desktop will not find it: with the org.kde.desktop style loaded the
        // theme lookup can miss this application's own names although they are
        // installed in hicolor. A program's picture of itself should not depend
        // on the desktop agreeing to find it (entry 67).
        if (pixmap.isNull() && id.startsWith(QStringLiteral("io.github.ales_drnz.vocem_overlay"))) {
            QIcon carried(QStringLiteral(":/vocem/icons/%1.svg").arg(id));
            pixmap = carried.pixmap(width, height);
        }
        if (size) {
            *size = pixmap.size();
        }
        return pixmap;
    }
};

// A popup is in no screenshot and in no geometry dump unless something clicks it
// open, and the font picker's list -- its width, its height, where it opens and
// which face each row is drawn in -- has to be measured like everything else.
//
// VOCEM_CONFIG_OPEN is a comma-separated list of objectNames. Anything named
// that has a `popup` (a ComboBox does) has it opened after the section switch
// and before the grab; an item that is a Popup itself is opened directly. An
// open popup's contents are parented into the window's overlay, which is a
// child of the window's content item, so the walk below finds them with no
// further help. Inert without the variable, like the screenshot path.
void open_named_popups(QQuickWindow* window, const QString& names) {
    for (const QString& name : names.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        auto* found = window->findChild<QQuickItem*>(name.trimmed());
        // Only where it is: the named control lives on one section, and every
        // other section would otherwise get that section's menu hanging over it.
        if (!found || !found->isVisible()) {
            continue;
        }
        QObject* popup = found->property("popup").value<QObject*>();
        QMetaObject::invokeMethod(popup ? popup : found, "open");
    }
}

// Asks the instance holding the lock to show itself, waiting up to `wait_ms`
// for its socket: the holder may still be building its window.
bool ask_running_instance_to_show(int wait_ms) {
    const int step_ms = 100;
    for (int waited = 0;; waited += step_ms) {
        QLocalSocket socket;
        socket.connectToServer(instance_socket_name());
        if (socket.waitForConnected(300)) {
            socket.write("show");
            socket.waitForBytesWritten(300);
            socket.disconnectFromServer();
            return true;
        }
        if (waited >= wait_ms) {
            return false;
        }
        QThread::msleep(step_ms);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // QApplication rather than QGuiApplication, for one reason: the system tray.
    // On Plasma the platform theme implements Qt.labs.platform's tray icon with
    // KStatusNotifierItem, which builds a QMenu -- a QtWidgets class, and QtWidgets
    // calls qFatal() when there is no QApplication. It is not a graceful failure to
    // work around: the process aborts as the tray icon is created.
    QApplication application(argc, argv);
    // Hiding the window must not end the process, since the tray icon is what the
    // user gets it back with. The QML side quits explicitly when there is no tray
    // to go back to, so this cannot strand a running process with no interface.
    QApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::setApplicationName(QStringLiteral("vocem-config"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Vocem Overlay"));
    QGuiApplication::setOrganizationName(QStringLiteral("vocem"));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.ales_drnz.vocem_overlay"));
    // Every icon name this window asks for exists in Breeze; not all of them exist
    // in Adwaita. A name the session's own theme does not carry falls through to
    // Breeze rather than leaving a hole in the sidebar. ConfigBridge::icon() picks
    // between alternatives before it comes to this.
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    // A development aid beside the screenshot one below: offscreen there is no
    // platform theme, so the window always renders in the light colours Qt
    // defaults to, whatever the session looks like. VOCEM_CONFIG_COLOURS=dark
    // states the answer instead of asking for one. Theme.qml reads
    // styleHints.colorScheme first and the palette second, so both are set.
    if (const char* colours = std::getenv("VOCEM_CONFIG_COLOURS");
        colours && std::strcmp(colours, "dark") == 0) {
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(0x1b, 0x1e, 0x20));
        dark.setColor(QPalette::WindowText, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::Base, QColor(0x1b, 0x1e, 0x20));
        dark.setColor(QPalette::AlternateBase, QColor(0x23, 0x26, 0x29));
        dark.setColor(QPalette::Text, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::Button, QColor(0x31, 0x36, 0x3b));
        dark.setColor(QPalette::ButtonText, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::ToolTipBase, QColor(0x31, 0x36, 0x3b));
        dark.setColor(QPalette::ToolTipText, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::PlaceholderText, QColor(0xa1, 0xa9, 0xb1));
        dark.setColor(QPalette::Highlight, QColor(0x3d, 0xae, 0xe9));
        dark.setColor(QPalette::HighlightedText, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x77, 0x7d, 0x83));
        dark.setColor(QPalette::Disabled, QPalette::Text, QColor(0x77, 0x7d, 0x83));
        dark.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x77, 0x7d, 0x83));
        QApplication::setPalette(dark);
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    }

    // QQuickStyle has no query for available styles, so the presence of the
    // qqc2-desktop-style QML module is checked directly. Falling back to Qt's own
    // style is fine: it simply looks less at home on Plasma.
    // An explicit choice wins: QQuickStyle::setStyle overrides the environment, so
    // without this check QT_QUICK_CONTROLS_STYLE would be silently ignored.
    const char* requested_style = std::getenv("QT_QUICK_CONTROLS_STYLE");
    const bool style_chosen_by_user = requested_style && *requested_style;

    const bool desktop_style_available = [] {
        for (const QString& root : QGuiApplication::libraryPaths()) {
            if (QDir(root + QStringLiteral("/../qml/org/kde/desktop")).exists() ||
                QDir(root + QStringLiteral("/qml/org/kde/desktop")).exists()) {
                return true;
            }
        }
        return QDir(QStringLiteral("/usr/lib/qt6/qml/org/kde/desktop")).exists();
    }();

    if (desktop_style_available && !style_chosen_by_user) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // Before anything is created: a second instance has nothing to do but wake the
    // first one. Skipped for the screenshot path, which is expected to run
    // alongside whatever else is open.
    const bool taking_screenshots = std::getenv("VOCEM_CONFIG_SCREENSHOT") != nullptr ||
                                    std::getenv("VOCEM_CONFIG_GEOMETRY") != nullptr;
    QLockFile instance_lock(instance_lock_name());
    if (!taking_screenshots) {
        // A lock whose holder is gone -- a crash, a kill -- is stale, and
        // QLockFile knows by the pid written in it; the holder alive means the
        // one window exists, whether or not it is listening yet.
        if (!instance_lock.tryLock(0)) {
            if (instance_lock.removeStaleLockFile()) {
                instance_lock.tryLock(0);
            }
        }
        if (!instance_lock.isLocked()) {
            ask_running_instance_to_show(3000);
            return 0;
        }
    }

    // --hidden is what the autostart entry passes: start behind the tray icon,
    // with no window, which is the only sane thing to do at login.
    bool start_hidden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hidden") == 0) {
            start_hidden = true;
        }
    }

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    engine.setInitialProperties({{QStringLiteral("startHidden"), start_hidden}});
    engine.loadFromModule("Vocem", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto* main_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());

    // This instance holds the lock, so it is the one that listens. removeServer
    // clears a socket left behind by a process that died without cleaning up --
    // without it, a crash would make the application unstartable until reboot.
    // Safe to call because of the lock: the only socket this can unlink is a
    // dead process's, never a living instance's.
    QLocalServer server;
    if (!taking_screenshots) {
        QLocalServer::removeServer(instance_socket_name());
        // The owner, and nobody else. Without this the socket is created
        // world-accessible, and where XDG_RUNTIME_DIR is missing it lands in /tmp
        // -- so any local user could make this window appear on the desktop.
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.listen(instance_socket_name());
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, main_window] {
            QLocalSocket* client = server.nextPendingConnection();
            if (client) {
                client->deleteLater();
            }
            if (main_window) {
                main_window->show();
                main_window->raise();
                main_window->requestActivate();
            }
        });
    }

    // A development aid, not a feature: with VOCEM_CONFIG_SCREENSHOT set, the
    // window renders itself to that file and exits. Judging a layout by describing
    // it does not work, and asking a screenshot tool for "the window" picks the
    // wrong one often enough to be useless. Run it with
    //   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
    // and it needs no display at all.
    const char* screenshot_target = std::getenv("VOCEM_CONFIG_SCREENSHOT");
    const char* geometry_target = std::getenv("VOCEM_CONFIG_GEOMETRY");
    if ((screenshot_target && *screenshot_target) || (geometry_target && *geometry_target)) {
        QQuickWindow* window = main_window;
        if (!window) {
            return 1;
        }
        const QString path = QString::fromLocal8Bit(screenshot_target ? screenshot_target : "");
        const QString geometry_path =
            QString::fromLocal8Bit(geometry_target ? geometry_target : "");
        auto* geometry_file = new QFile(geometry_path, window);
        if (!geometry_path.isEmpty() && !geometry_file->open(QIODevice::WriteOnly)) {
            return 1;
        }

        // The version this binary carries, once, before any section. It is the
        // same string ConfigBridge::version() answers with and AboutPage.qml
        // prints, so a run of the window can be asked what it calls itself.
        // What this line does not carry is the rendered label -- the dump holds
        // rectangles and numbers, not text -- so it answers "which version is
        // compiled in", one QML binding short of "which version the page shows".
        if (geometry_file->isOpen()) {
            QTextStream out(geometry_file);
            out << "{\"version\": \"" << QStringLiteral(VOCEM_VERSION) << "\"}\n";
        }

        // The window at a size of the caller's choosing, so the layouts can be
        // judged at the smallest one the window allows as well as at its default.
        if (const char* size = std::getenv("VOCEM_CONFIG_SIZE"); size && *size) {
            const QStringList parts = QString::fromLocal8Bit(size).split('x');
            if (parts.size() == 2) {
                window->resize(parts[0].toInt(), parts[1].toInt());
            }
        }

        // Every section the sidebar has, About included, numbered, so a change can
        // be judged on all of them. Each grab happens after the section has had a
        // moment to lay out and load its images.
        //
        // VOCEM_CONFIG_SECTIONS=N stops after section N. A comma in it is an
        // explicit walk instead: the sections to visit, in order, repeats allowed,
        // so a page can be grabbed again after something changed underneath it
        // (the slow sweep is four seconds: "0,1,2,3,0").
        const int last_section = 9;
        QList<int> walk;
        if (const char* asked = std::getenv("VOCEM_CONFIG_SECTIONS"); asked && *asked) {
            const QString text = QString::fromLocal8Bit(asked);
            if (text.contains(QLatin1Char(','))) {
                for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    bool number = false;
                    const int section = part.trimmed().toInt(&number);
                    if (number && section >= 0 && section <= last_section) {
                        walk.append(section);
                    }
                }
            } else {
                const int stop = text.toInt();
                for (int section = 0; section <= qBound(0, stop, last_section); ++section) {
                    walk.append(section);
                }
            }
        }
        if (walk.isEmpty()) {
            for (int section = 0; section <= last_section; ++section) {
                walk.append(section);
            }
        }
        // The pause between one grab and the next section, 700 ms unless
        // VOCEM_CONFIG_STEP_MS says otherwise. It lays nothing out -- the 400 ms
        // before each grab is the page's moment, and the grab renders
        // synchronously -- so a test that only reads what each page shows can
        // ask for 0. The tests that use the walk as a CLOCK leave it alone,
        // because shortening it would change what they measure. The first step
        // always waits the full 700: that one is the window coming up.
        // A value that is not a number keeps the 700 and says so: toInt() reads
        // "abc" or "700ms" as 0, which is the fastest walk and not the default.
        int step_ms = 700;
        if (const char* asked = std::getenv("VOCEM_CONFIG_STEP_MS"); asked && *asked) {
            bool number = false;
            const int value = QString::fromLocal8Bit(asked).toInt(&number);
            if (number) {
                step_ms = qBound(0, value, 5000);
            } else {
                std::fprintf(stderr, "VOCEM_CONFIG_STEP_MS=%s is not a number; stepping at 700 ms\n",
                             asked);
            }
        }
        auto* step = new QTimer(window);
        auto* index = new int(0);
        // The controls already driven (drive_controls), for the whole walk.
        const bool drive = std::getenv("VOCEM_CONFIG_DRIVE") != nullptr;
        auto* driven = new QSet<QQuickItem*>();
        step->setInterval(700);
        QObject::connect(step, &QTimer::timeout, window,
                         [window, path, geometry_file, index, step, walk, step_ms, drive, driven] {
            step->setInterval(step_ms);
            if (*index >= walk.size()) {
                if (geometry_file->isOpen()) {
                    // What the run cost, in counts (ConfigBridge::counters):
                    // the last line of the dump, so a test can hold the
                    // window's idle cost to a number rather than to a clock.
                    if (auto* bridge = window->findChild<ConfigBridge*>()) {
                        QTextStream out(geometry_file);
                        const QVariantMap counters = bridge->counters();
                        out << "{\"counters\": {";
                        bool first = true;
                        for (auto it = counters.constBegin(); it != counters.constEnd(); ++it) {
                            out << (first ? "" : ", ") << '"' << it.key() << "\": "
                                << it.value().toLongLong();
                            first = false;
                        }
                        out << "}}\n";
                    }
                    geometry_file->close();
                }
                step->stop();
                QGuiApplication::exit(0);
                return;
            }
            // One section at a time, with the clock stopped while this one is
            // being taken: a step that costs more than the 400 ms before its grab
            // (opening the font menu builds the machine's whole font list) would
            // otherwise let the next timeout in on top of it, and the same section
            // would be set, grabbed and dumped twice (entry 105).
            step->stop();
            // The section this step visits; the step's own number is what names
            // the file and the dump line, because a walk may visit one section
            // more than once and two grabs must not land on one name.
            const int section = walk.at(*index);
            window->setProperty("section", section);
            // Before the grab, so the popup has the same moment to lay itself
            // out that the page has.
            if (const char* open = std::getenv("VOCEM_CONFIG_OPEN"); open && *open) {
                open_named_popups(window, QString::fromLocal8Bit(open));
            }
            // Grab on the next tick: the property change has to reach the scene.
            QTimer::singleShot(400, window, [window, path, geometry_file, index, step, section,
                                             drive, driven] {
                // Always grabbed, even when no screenshot was asked for: the grab is
                // what forces a synchronous render, and without a render the section
                // that has just been switched to is never laid out. Its items then
                // report the size they had when the window opened, which is zero for
                // every page but the first -- a geometry dump that looks like a
                // finished measurement and is nothing of the kind.
                const QImage frame = window->grabWindow();
                if (!path.isEmpty()) {
                    QString file = path;
                    if (*index > 0) {
                        file.insert(file.lastIndexOf('.'), QStringLiteral("-%1").arg(*index));
                    }
                    frame.save(file);
                    // Any other visible window belongs to the run too: a
                    // harness that only ever grabs the main window could never
                    // look at it.
                    int extra = 0;
                    for (QWindow* other : QGuiApplication::allWindows()) {
                        auto* quick_other = qobject_cast<QQuickWindow*>(other);
                        if (!quick_other || quick_other == window || !quick_other->isVisible()) {
                            continue;
                        }
                        QString extra_file = path;
                        extra_file.insert(extra_file.lastIndexOf('.'),
                                          QStringLiteral("-extra%1").arg(extra));
                        quick_other->grabWindow().save(extra_file);
                        ++extra;
                    }
                }
                if (geometry_file->isOpen()) {
                    QTextStream out(geometry_file);
                    QHash<QString, int> seen;
                    out << "{\"section\": " << section << ", \"step\": " << *index
                        << ", \"window\": {\"w\": " << window->width()
                        << ", \"h\": " << window->height() << "}}\n";
                    // Every visible window and whether it hangs off another one.
                    // A window declared inside another becomes a transient child,
                    // which a compositor centres over its parent -- indistinguishable,
                    // from the outside, from a dialog inside the application. A claim
                    // about window arrangement needs something that can read window
                    // arrangement.
                    for (QWindow* other : QGuiApplication::allWindows()) {
                        if (!other->isVisible()) {
                            continue;
                        }
                        out << "{\"topLevel\": \"" << other->objectName() << "\", \"transient\": "
                            << (other->transientParent() ? "true" : "false")
                            << ", \"w\": " << other->width() << ", \"h\": " << other->height()
                            << "}\n";
                    }
                    if (window->contentItem()) {
                        dump_item(window->contentItem(), QString(), seen, out);
                    }
                    // After the page's own dump, so its geometry is the
                    // page as it opened.
                    if (drive) {
                        drive_controls(window, section, *driven, out);
                    }
                }
                ++(*index);
                step->start();
            });
        });
        step->start();
    }

    return QGuiApplication::exec();
}
