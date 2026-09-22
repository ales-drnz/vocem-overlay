// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The three switches that write straight through, and what they do to a file
// somebody edited while the window was open.
//
// `persistNow()` is the exception to Apply: the overlay, panel and message
// switches are reachable from the header and from the tray, and a switch that
// does nothing until a button on another page is pressed is a broken switch
// (entry 136). So it writes on top of the file as it stands --
// `Config::write_switch()` loads it fresh, sets the one, saves -- and an
// edit made outside the window survives on disk. What did not survive was the
// window's own idea of that edit: the bridge stored the result in `saved_` and
// advanced `disk_mtime_`, and left `config_`, the copy the window shows and
// Apply writes, as it was. `reloadIfMoved()` compares the timestamp that write
// had just advanced, so it never looked again -- and the next Apply put the
// morning's values back over the edit, with the button grey and nothing saying
// the two differed.
//
// **Why this is a probe and not a window test.** The offscreen harness can
// walk sections, take screenshots and open a popup by objectName (entry 105);
// it cannot press a Switch, and these three settings have no other interface.
// So the bridge is built here directly, the way tests/daemon_ws_bounds.cpp
// compiles daemon/src/websocket.cpp and talks to it from a stub -- no window,
// no QML, no engine. It links Qt6::Gui (the bridge reaches QClipboard and
// QWindow), Qt6::Qml (its QML_ELEMENT registration) and fontconfig (the font
// list), and deliberately not Quick or Widgets: it was linking both, which
// measured 14 NEEDED entries and 79 shared objects at load against 11 and 73
// without them. VOCEM_CONFIG_NO_DAEMON keeps the constructor's probes
// away from the owner's systemd (config_bridge.cpp's `harness` flag), and
// XDG_CONFIG_HOME is a scratch directory, so nothing here can reach the
// settings a running game is reading.
//
// Two claims, and the second is the one that was false:
//   * the edit survives on disk -- true before this fix as well, and here so
//     that a regression in write_switch shows up as itself;
//   * the window's own copy follows it, so the next Apply does not write over
//     it.

#include <QGuiApplication>
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

// The whole file, so a comparison can name what it found.
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

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    vocem_test::set_alarm(60, "the bridge's instant switches");

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
    // The constructor's `harness` flag: no systemctl, no daemon, no autostart
    // work. Everything else runs as a real launch does.
    qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");

    const QString path = config_home + "/vocem/config.ini";
    // A setting the window holds a copy of, at a value it cannot have by
    // default, and the switch this test moves.
    write_file(path, "panel_enabled = true\nrow_spacing = 9.0\n");

    QGuiApplication app(argc, argv);
    ConfigBridge bridge;
    check(bridge.panelEnabled(), "the bridge read the file it was born with");
    check(qAbs(bridge.rowSpacing() - 9.0) < 0.001,
          "and holds the value that file gave it");

    // Somebody edits the file while the window is open: `vocem-config` is a
    // tray application that stays running for days, and this is the ordinary
    // way a setting changes under it -- by hand, or by a second window, or by a
    // script.
    write_file(path, "panel_enabled = true\nrow_spacing = 3.0\n");

    // The switch. This is persistNow(), and the file is rewritten from what is
    // on disk right now.
    bridge.setPanelEnabled(false);

    const QString after_switch = read_file(path);
    check(after_switch.contains("row_spacing = 3"),
          "the edit made outside the window survives the switch, on disk");
    check(after_switch.contains("panel_enabled = false"), "and the switch itself was written");

    // The claim that was false: the window's own copy followed the file it had
    // just written. Without it the bridge still reads 9.0 here.
    const bool followed = qAbs(bridge.rowSpacing() - 3.0) < 0.001;
    if (!followed) {
        std::printf("--  the bridge still holds row_spacing = %.3f where the file says 3.0\n",
                    bridge.rowSpacing());
    }
    check(followed, "and the window's own copy follows it, rather than the morning's value");

    // And the proof of what that costs. Apply writes the window's whole copy,
    // so the edit is lost the next time anybody presses it -- but only when
    // there is something to apply: `apply()` returns at once with nothing
    // pending, which is why this leg makes an ordinary edit first. Without it
    // the check passes against the defect as well, for a reason that has
    // nothing to do with what it claims (entry 77's shape).
    bridge.setAvatarGap(7.0);
    check(bridge.pending(), "an ordinary edit is pending, so Apply has something to write");
    bridge.apply();
    const QString after_apply = read_file(path);
    const bool kept = after_apply.contains("row_spacing = 3");
    if (!kept) {
        std::printf("--  after Apply the file says:\n%s\n", after_apply.toUtf8().constData());
    }
    check(kept, "so the next Apply keeps the edit instead of writing the morning's value over it");
    check(after_apply.contains("avatar_gap = 7"), "and writes what the window was actually asked for");

    // The OTHER switches. persistNow() wrote all three, and the two nobody
    // touched came from the window's copy -- so an edit that turned messages
    // off outside the window was turned back on by the next click on the
    // panel's switch, when that click came before the four-second sweep had
    // reloaded the file, or at any time while an edit was waiting for Apply
    // (entry 203). Here an edit IS waiting: avatar_gap below.
    bridge.setAvatarGap(5.0);
    check(bridge.pending(), "an edit is waiting for Apply again");
    write_file(path, read_file(path).replace("notifications_enabled = true",
                                             "notifications_enabled = false"));
    const bool planted = read_file(path).contains("notifications_enabled = false");
    check(planted, "messages are switched off in the file, outside the window");
    bridge.setPanelEnabled(true);
    const QString after_other = read_file(path);
    const bool untouched = after_other.contains("notifications_enabled = false");
    if (!untouched) {
        std::printf("--  after the panel's switch the file says:\n%s\n",
                    after_other.toUtf8().constData());
    }
    check(untouched, "the panel's switch writes the panel's switch and not the other two");
    check(after_other.contains("panel_enabled = true"), "and it did write the panel's");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
