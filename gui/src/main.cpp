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
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickItem>
#include <QTextStream>
#include <QQuickWindow>
#include <QTimer>

#include <cstdlib>
#include <cstring>

namespace {

// One window per user, not one per launch.
//
// Closing the window leaves the process running behind its tray icon, so starting
// the application again -- from the menu, from the tray's own entry, from anything
// -- used to add a second process and a second icon to the tray, and then a third.
// The first instance listens on a socket in the runtime directory; later ones find
// it, ask it to show itself, and exit.
//
// $XDG_RUNTIME_DIR is the right place for the socket: it is per-user, mode 0700,
// and cleared when the session ends, so a stale socket cannot outlive a reboot.
//
// The paragraph above was true about the intention and false about the code for
// as long as it stood there. A bare name given to QLocalServer is resolved
// against QDir::tempPath(), so the socket was `/tmp/vocem-config-1000` at mode
// 0755 -- world-connectable, in a directory anybody on the machine can write to.
// Nothing secret goes over it; what goes over it is "show yourself", so any
// local user could make this window appear on somebody's screen. Found by
// looking for it after a run of my own did exactly that. An absolute path puts
// it where the comment always said it was; without a runtime directory there is
// nowhere better than the old behaviour, and it says so rather than inventing
// one.
QString instance_socket_name() {
    const QString name = QStringLiteral("vocem-config-%1").arg(::getuid());
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        return name;
    }
    return QString::fromLocal8Bit(runtime) + '/' + name;
}

// The geometry of everything the previews draw, as numbers.
//
// The companion of tests/vocem_panel_geometry, which measures the real overlay the
// same way. The previews had been corrected three times by looking at screenshots
// and adjusting figures, and each pass fixed some and broke others; the only way
// out was to put the two geometries side by side as numbers and compare them. Every
// item that stands for something the overlay draws carries an objectName, and this
// walks the scene and prints where each one actually ended up.
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
                                 // hold each one to a distinct surface.
                                 "surfaceRgb", "presetOpacity",
                                 // Whether the keyboard can reach it. A bool converts
                                 // to a number, and "every control is reachable
                                 // without a mouse" is otherwise a claim nobody can
                                 // check without a pair of hands -- the guidelines'
                                 // accessibility page asks for exactly that test.
                                 // The org.kde.desktop ToolButton has its focusPolicy
                                 // commented out with a "KF6 TODO" beside it, which
                                 // reads like the row resets are out of the chain;
                                 // measured here they are in it, and this is what
                                 // will say so if that ever stops being true.
                                 "activeFocusOnTab",
                                 // How many of a preview's own pictures actually
                                 // came up. An Image that failed to load keeps the
                                 // size its layout gave it and paints nothing, so
                                 // a rectangle cannot tell a drawn icon from a
                                 // missing one -- and missing is what six of them
                                 // were until the artwork was carried in the
                                 // binary.
                                 "loadedIcons"}) {
            const QVariant value = item->property(name);
            if (value.isValid() && value.canConvert<qreal>()) {
                out << ", \"" << name << "\": " << value.toReal();
            }
        }
        out << "}\n";
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        dump_item(child, here, seen, out);
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
        // desktop will not find it. Measured with the org.kde.desktop style
        // loaded: every Breeze name the window asks for resolves and not one of
        // this application's six does -- `QIcon::hasThemeIcon` answers false and
        // the pixmap comes back 0x0 -- although they are installed in hicolor
        // and the panel resolves the very same names for the tray icon. So the
        // About page drew an empty square where its icon goes, for as long as
        // that page has existed. A program's picture of itself should not
        // depend on the desktop agreeing to find it.
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

// A popup is in no screenshot and in no geometry dump, because nothing in a
// harness ever clicks one open. The font picker's list is the case that made
// this necessary: its width, its height, where it opens and which face each row
// is drawn in were all reasoned about and none of them had ever been measured,
// so every claim about them rested on reading the source back.
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

bool ask_running_instance_to_show() {
    QLocalSocket socket;
    socket.connectToServer(instance_socket_name());
    if (!socket.waitForConnected(300)) {
        return false;
    }
    socket.write("show");
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
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
    if (!taking_screenshots && ask_running_instance_to_show()) {
        return 0;
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

    // Nobody answered, so this instance is the one that listens. removeServer
    // clears a socket left behind by a process that died without cleaning up --
    // without it, a crash would make the application unstartable until reboot.
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
        //
        // Nothing could ask before, and the number was wrong for four releases:
        // the About page of the installed 0.1.4 package said "Version 0.1.0",
        // because VOCEM_VERSION comes from CMakeLists.txt's project(VERSION) and
        // that line had never moved. What this line does not carry is the
        // rendered label -- the dump holds rectangles and numbers, not text --
        // so it answers "which version is compiled in", one QML binding short of
        // "which version the page shows".
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

        // Every section, numbered, so a change can be judged on all of them rather
        // than on whichever one happens to open first. Each grab happens after the
        // section has had a moment to lay out and load its images.
        // Every section the sidebar has, About included: it is a page like any
        // other and a screenshot run that stops one short of it is a run that
        // never looks at it.
        //
        // VOCEM_CONFIG_SECTIONS stops earlier. A screenshot run wants all of them;
        // the geometry comparison reads two, and walking the other six costs it a
        // second and a half of switching pages for nothing.
        int sections = 9;
        if (const char* last = std::getenv("VOCEM_CONFIG_SECTIONS"); last && *last) {
            const int asked = QString::fromLocal8Bit(last).toInt();
            if (asked >= 0 && asked < sections) {
                sections = asked;
            }
        }
        auto* step = new QTimer(window);
        auto* index = new int(0);
        step->setInterval(700);
        QObject::connect(step, &QTimer::timeout, window,
                         [window, path, geometry_file, index, step, sections] {
            if (*index > sections) {
                if (geometry_file->isOpen()) {
                    geometry_file->close();
                }
                step->stop();
                QGuiApplication::exit(0);
                return;
            }
            // One section at a time, with the clock stopped while this one is
            // being taken. Left running, the walk assumed every step costs less
            // than the 400 ms before its grab -- and the first step that did not
            // (opening the font menu builds the machine's whole font list) let
            // the next timeout in on top of it, so the same section was set
            // twice, grabbed twice and dumped twice while the sections ran out.
            // A dump of three sections that are all section 0 looks exactly like
            // a finished measurement.
            step->stop();
            window->setProperty("section", *index);
            // Before the grab, so the popup has the same moment to lay itself
            // out that the page has.
            if (const char* open = std::getenv("VOCEM_CONFIG_OPEN"); open && *open) {
                open_named_popups(window, QString::fromLocal8Bit(open));
            }
            // Grab on the next tick: the property change has to reach the scene.
            QTimer::singleShot(400, window, [window, path, geometry_file, index, step] {
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
                    // Any other visible window belongs to the run too -- the
                    // crash report window appears on the desktop by itself, and
                    // a harness that only ever grabs the main window could
                    // never look at it.
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
                    out << "{\"section\": " << *index << ", \"window\": {\"w\": " << window->width()
                        << ", \"h\": " << window->height() << "}}\n";
                    // Every visible window and whether it hangs off another one.
                    // The crash report is supposed to be its own window on the
                    // desktop; declared inside the settings window it had silently
                    // become a transient child, which a compositor centres over its
                    // parent -- indistinguishable, from the outside, from a dialog
                    // inside the application. A claim about window arrangement needs
                    // something that can read window arrangement.
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
                }
                ++(*index);
                step->start();
            });
        });
        step->start();
    }

    return QGuiApplication::exec();
}
