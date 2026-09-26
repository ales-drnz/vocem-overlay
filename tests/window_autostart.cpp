// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The "Start at login" switch says what the desktop will do, and Apply leaves
// the desktop's own answer alone.
//
// The switch read "is there a file at $XDG_CONFIG_HOME/autostart/<id>.desktop"
// and nothing else -- while the desktops switch an entry off without deleting
// it: the freedesktop Autostart specification's Hidden=true (XFCE's settings,
// and what the spec says a user's copy should do), GNOME's
// X-GNOME-Autostart-enabled=false. Either one and the switch said "on" for a
// window that will not start. And every Apply -- of ANY setting -- called
// set_autostart(true), which truncated the entry and wrote this program's
// own: a click on a colour turned the desktop's "off" back on and threw away
// whatever the user had put in the entry. A write that failed said nothing.
//
// The bridge is built directly (config_bridge_switches.cpp's shape), with
// VOCEM_CONFIG_NO_DAEMON, every XDG directory scratch and /dev/shm private.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

#include "config_bridge.h"
#include "private_shm.h"
#include "probe_alarm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

QByteArray read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void write_file(const QString& path, const QByteArray& text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int sandbox = vocem_test::ensure_private_shm(false);
    if (sandbox != -1) {
        return sandbox;
    }
    vocem_test::set_alarm(60, "the autostart switch");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const QString root = scratch.path();
    for (const char* sub : {"config/vocem", "config/autostart", "cache/vocem", "state", "data"}) {
        QDir().mkpath(root + '/' + sub);
    }
    qputenv("HOME", root.toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root + "/config").toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (root + "/cache").toLocal8Bit());
    qputenv("XDG_STATE_HOME", (root + "/state").toLocal8Bit());
    qputenv("XDG_DATA_HOME", (root + "/data").toLocal8Bit());
    qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("LD_PRELOAD");

    const QString entry = root + "/config/autostart/io.github.ales_drnz.vocem_overlay.desktop";
    const QByteArray ours =
        "[Desktop Entry]\nType=Application\nName=Vocem Overlay\n"
        "Exec=/usr/bin/vocem-config --hidden\n"
        // What a user adds by hand, or a desktop's own editor does.
        "X-KDE-autostart-phase=2\n";

    QGuiApplication app(argc, argv);

    // ---- the desktop switched the entry off, in each of the two spellings.
    write_file(entry, ours + "Hidden=true\n");
    {
        ConfigBridge bridge;
        check(!bridge.startAtLogin(), "an entry with Hidden=true reads as off");
    }
    write_file(entry, ours + "X-GNOME-Autostart-enabled=false\n");
    {
        ConfigBridge bridge;
        check(!bridge.startAtLogin(), "an entry with X-GNOME-Autostart-enabled=false reads as off");
        // And an Apply of something else does not switch it back on.
        bridge.setAvatarGap(7.0);
        bridge.apply();
        check(read_file(entry) == ours + "X-GNOME-Autostart-enabled=false\n",
              "Apply of another setting leaves the desktop's 'off' as it was");
    }
    // A key of the same name in another group is not the entry's.
    write_file(entry, ours + "[Desktop Action other]\nHidden=true\n");
    {
        ConfigBridge bridge;
        check(bridge.startAtLogin(), "Hidden=true in another group does not switch the entry off");
    }

    // ---- an entry that is on: Apply of another setting does not touch it.
    write_file(entry, ours);
    {
        ConfigBridge bridge;
        check(bridge.startAtLogin(), "an entry with neither key reads as on");
        bridge.setAvatarGap(9.0);
        bridge.apply();
        const QByteArray after = read_file(entry);
        const bool same = after == ours;
        if (!same) {
            std::printf("--  the entry now reads:\n%s--  (end)\n", after.constData());
        }
        check(same, "Apply of another setting leaves the entry byte for byte as it was");

        // The switch itself still works both ways.
        bridge.setStartAtLogin(false);
        bridge.apply();
        check(!QFile::exists(entry), "switching it off removes the entry");
        bridge.setStartAtLogin(true);
        bridge.apply();
        check(QFile::exists(entry) && read_file(entry).contains("--hidden"),
              "switching it on writes one that starts hidden");
        check(bridge.saveError().isEmpty(), "and nothing reported a failure");
    }

    // ---- a write that cannot happen is said.
    QFile::remove(entry);
    QDir(root + "/config/autostart").removeRecursively();
    write_file(root + "/config/autostart", "a file where the directory should be\n");
    {
        ConfigBridge bridge;
        check(!bridge.startAtLogin(), "no entry reads as off");
        bridge.setStartAtLogin(true);
        bridge.apply();
        const bool said = !bridge.saveError().isEmpty();
        if (said) {
            std::printf("--  the window says: %s\n", bridge.saveError().toUtf8().constData());
        }
        check(said, "an entry that could not be written is reported, not dropped in silence");
        check(bridge.pending(), "and the change is still waiting, so Apply can be pressed again");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
