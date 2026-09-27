// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Apply writes what the window was asked for, and nothing else over the file.
//
// The three switches already wrote on top of the file as it stood (entries
// 136/203, tests/config_bridge_switches.cpp). Apply did not: it saved the
// window's whole copy, and while an edit waited for Apply that copy did not
// follow the file (reloadIfMoved keeps it, on purpose, so the edit is not
// taken away). So every key changed outside the window since it last followed
// the file was put back by the next Apply. The one that cost most is
// `flatpak_apps`: the README tells users to edit it by hand, the window has no
// control for it, and it is consent -- the list of Flatpak applications the
// overlay may enter. Measured on the first round's tree with this bridge:
// setTextShadow(true), `flatpak_apps = com.example.SomeGame` appended by hand,
// apply() -- the file said `flatpak_apps = `.
//
// The bridge is built directly, as config_bridge_switches.cpp builds it
// (VOCEM_CONFIG_NO_DAEMON, scratch XDG directories, offscreen), so nothing
// here reaches the owner's settings or services.
//
// Three legs:
//   * Apply right after a hand edit, before the window's four-second sweep
//     has read it: the hand edit survives, both a key the window has no
//     control for and one it has;
//   * the same with the sweep in between -- the sweep read the file while the
//     edit was pending, which is the ordinary case for a window left open;
//   * a key changed both by hand and in the window: the window's value is
//     written, because that is the edit Apply was pressed for.

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

#include "config_bridge.h"
#include "probe_alarm.h"
#include "vocem/config.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

QString read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void write_file(const QString& path, const QString& text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(text.toUtf8());
    }
}

void append_file(const QString& path, const QString& text) {
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write(text.toUtf8());
    }
}

// Long enough for the bridge's timer to reach its every-eighth-tick sweep
// (500 ms x 8), with a margin.
void let_the_sweep_run() {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 4700) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void show_on_failure(bool ok, const char* when, const QString& text) {
    if (!ok) {
        std::printf("--  %s the file says:\n%s\n", when, text.toUtf8().constData());
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    vocem_test::set_alarm(60, "Apply over a file edited outside the window");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const QString config_home = scratch.path() + "/config";
    const QString cache_home = scratch.path() + "/cache";
    QDir().mkpath(config_home + "/vocem");
    QDir().mkpath(cache_home + "/vocem");
    qputenv("XDG_CONFIG_HOME", config_home.toUtf8());
    qputenv("XDG_CACHE_HOME", cache_home.toUtf8());
    qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");

    const QString path = config_home + "/vocem/config.ini";
    write_file(path,
               "[appearance]\ntext_shadow = false\n"
               "[notifications]\nnotifications_enabled = true\n"
               "[spacing]\nrow_spacing = 9.0\n");

    QGuiApplication app(argc, argv);
    ConfigBridge bridge;
    check(!bridge.textShadow() && bridge.notificationsEnabled(),
          "the bridge read the file it was born with");

    // Leg 1: no sweep between the hand edit and Apply.
    bridge.setTextShadow(true);
    check(bridge.pending(), "an edit is waiting for Apply");
    write_file(path, read_file(path).replace("notifications_enabled = true",
                                             "notifications_enabled = false"));
    append_file(path, "[behaviour]\nflatpak_apps = com.example.SomeGame\n");
    bridge.apply();
    check(!bridge.pending(), "Apply succeeded");
    QString text = read_file(path);
    bool ok = text.contains("flatpak_apps = com.example.SomeGame");
    show_on_failure(ok, "after the first Apply", text);
    check(ok, "a hand-written flatpak_apps survives Apply (the window has no control for it)");
    check(text.contains("notifications_enabled = false"),
          "a hand edit of a key the window does control survives Apply too");
    check(text.contains("text_shadow = true"), "and the edit Apply was pressed for is written");
    check(!bridge.notificationsEnabled(), "the window now shows the file's value");

    // Leg 2: the sweep reads the file while an edit is pending.
    bridge.setRowSpacing(4.0);
    check(bridge.pending(), "a second edit is waiting for Apply");
    // Whatever the first leg left on the line: this leg stands on its own.
    write_file(path, read_file(path)
                         .replace(QRegularExpression("flatpak_apps = [^\n]*"),
                                  "flatpak_apps = com.example.SomeGame,org.example.Other")
                         .replace("text_shadow = true", "text_shadow = false"));
    let_the_sweep_run();
    check(bridge.pending(), "the sweep left the pending edit pending");
    check(qAbs(bridge.rowSpacing() - 4.0) < 0.001, "and left the window's edit in the window");
    bridge.apply();
    text = read_file(path);
    ok = text.contains("flatpak_apps = com.example.SomeGame,org.example.Other");
    show_on_failure(ok, "after the second Apply", text);
    check(ok, "a hand edit the sweep saw while an edit was pending survives Apply");
    check(text.contains("text_shadow = false"),
          "so does one of a key the window controls");
    check(text.contains("row_spacing = 4.0"), "and the window's edit is written");

    // Leg 3: the same key, by hand and in the window.
    bridge.setRowSpacing(6.0);
    write_file(path, read_file(path).replace("row_spacing = 4.0", "row_spacing = 2.0"));
    let_the_sweep_run();
    bridge.apply();
    text = read_file(path);
    ok = text.contains("row_spacing = 6.0");
    show_on_failure(ok, "after the third Apply", text);
    check(ok, "a key edited in the window AND by hand gets the window's value on Apply");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
