// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the settings window does to the session's daemon, measured with stubs.
//
// ConfigBridge starts the daemon when the window opens and stops it on Quit,
// on a close with no tray behind it (every offscreen run) and around
// Re-authorise. Which command it uses depends on one asynchronous question,
// `systemctl --user cat vocemd.service`, and until that was answered -- and
// FOREVER under the harness, which never asked -- the stop fell through to
// `pkill -TERM -x vocemd`: a SIGTERM to every vocemd of the user, the owner's
// live daemon included, from a window that was only being measured. And a
// question that took longer than its three-second cap was read as "no unit",
// so the start exec'd a second vocemd beside the one the unit had started.
//
// The bridge is built directly (the config_bridge_switches.cpp shape): Quit,
// the close and the tray's Quit are all quitOverlay(), Re-authorise is
// reauthorise(), and neither needs a window. `systemctl`, `pkill` and `vocemd`
// are stubs first on PATH that only write their arguments to a log, so no
// scenario can reach the owner's services whatever the code under test does;
// /dev/shm is private (tests/private_shm.h) so the bridge does not read the
// live daemon's segment either, and every XDG directory and HOME are scratch.
//
// Three scenarios, named by VOCEM_WINDOW_DAEMON_SCENARIO (an argument does not
// survive private_shm.h's re-exec under bwrap; the environment does):
//   harness     VOCEM_CONFIG_NO_DAEMON: Re-authorise, then Quit. Nothing may
//               be started or stopped, and the token is not touched.
//   early-quit  a real launch whose unit question takes a second to answer,
//               quit at once: the stop waits for the answer and goes through
//               systemctl; no pkill, and no start after the quit.
//   slow-probe  a real launch whose unit question outlives its cap, with a
//               unit on disk: the start goes through systemctl, and no second
//               vocemd is exec'd.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config_bridge.h"
#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

QString read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void write_script(const QString& path, const QByteArray& body) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(body);
    }
    file.close();
    ::chmod(path.toLocal8Bit().constData(), 0755);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int sandbox = vocem_test::ensure_private_shm(false);
    if (sandbox != -1) {
        return sandbox;
    }
    const char* asked = std::getenv("VOCEM_WINDOW_DAEMON_SCENARIO");
    const char* scenario = asked && *asked ? asked : "harness";
    vocem_test::set_alarm(40, "the window's daemon lifecycle");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const QString root = scratch.path();
    for (const char* sub : {"bin", "config/vocem", "cache/vocem", "state/vocem", "data", "home"}) {
        QDir().mkpath(root + '/' + sub);
    }
    const QString log = root + "/stub.log";
    const QString token = root + "/state/vocem/token";
    {
        QFile planted(token);
        if (planted.open(QIODevice::WriteOnly)) {
            planted.write("scratch-token\n");
        }
    }

    // The stubs: each writes its own name and arguments, one line a call.
    write_script(root + "/bin/systemctl",
                 "#!/bin/sh\n"
                 "echo \"systemctl $*\" >> \"$VOCEM_TEST_STUB_LOG\"\n"
                 "case \"$2\" in\n"
                 "  cat) sleep \"${VOCEM_TEST_UNIT_DELAY:-0}\"; echo '[Unit]'; exit 0 ;;\n"
                 "  show-environment) echo 'PATH=/usr/bin'; exit 0 ;;\n"
                 "esac\n"
                 "exit 0\n");
    write_script(root + "/bin/pkill", "#!/bin/sh\necho \"pkill $*\" >> \"$VOCEM_TEST_STUB_LOG\"\nexit 1\n");
    write_script(root + "/bin/vocemd", "#!/bin/sh\necho \"vocemd $*\" >> \"$VOCEM_TEST_STUB_LOG\"\nexit 0\n");

    qputenv("PATH", (root + "/bin:").toLocal8Bit() + qgetenv("PATH"));
    qputenv("VOCEM_TEST_STUB_LOG", log.toLocal8Bit());
    qputenv("HOME", (root + "/home").toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root + "/config").toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (root + "/cache").toLocal8Bit());
    qputenv("XDG_STATE_HOME", (root + "/state").toLocal8Bit());
    qputenv("XDG_DATA_HOME", (root + "/data").toLocal8Bit());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("LD_PRELOAD");
    for (const char* harness : {"VOCEM_CONFIG_NO_DAEMON", "VOCEM_CONFIG_GEOMETRY",
                                "VOCEM_CONFIG_SCREENSHOT"}) {
        qunsetenv(harness);
    }

    const bool harness = std::strcmp(scenario, "harness") == 0;
    const bool early_quit = std::strcmp(scenario, "early-quit") == 0;
    const bool slow_probe = std::strcmp(scenario, "slow-probe") == 0;
    if (!harness && !early_quit && !slow_probe) {
        std::printf("FAIL unknown scenario '%s'\n", scenario);
        return 1;
    }
    if (harness) {
        qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    } else if (early_quit) {
        qputenv("VOCEM_TEST_UNIT_DELAY", "1");
    } else {
        // Past the probe's three-second cap. A unit where systemd would find
        // one, in this test's own configuration directory, so the answer does
        // not depend on whether this machine has the package installed.
        qputenv("VOCEM_TEST_UNIT_DELAY", "6");
        QDir().mkpath(root + "/config/systemd/user");
        QFile unit(root + "/config/systemd/user/vocemd.service");
        if (unit.open(QIODevice::WriteOnly)) {
            unit.write("[Service]\nExecStart=/usr/bin/vocemd\n");
        }
    }

    QGuiApplication app(argc, argv);
    ConfigBridge bridge;

    if (harness) {
        // Re-authorise first -- a stop, the token, a start -- and then Quit,
        // which ends the process through the stop's own completion.
        bridge.reauthorise();
        QTimer::singleShot(3000, &bridge, [&bridge] { bridge.quitOverlay(); });
    } else if (early_quit) {
        // The window closed inside the unit question's first second.
        bridge.quitOverlay();
    } else {
        // The start is settled at the cap (3 s); quit well after it.
        QTimer::singleShot(4500, &bridge, [&bridge] { bridge.quitOverlay(); });
    }
    const int code = app.exec();
    // The stubs spawned detached may still be writing their line.
    QThread::msleep(300);
    check(code == 0, "the window ended through its own Quit");

    const QString calls = read_file(log);
    std::printf("--  the stubs were called with:\n%s--  (end)\n", calls.toUtf8().constData());
    const QStringList lines = calls.split('\n', Qt::SkipEmptyParts);
    const auto count = [&lines](const char* prefix) {
        int found = 0;
        for (const QString& line : lines) {
            found += line.startsWith(QLatin1String(prefix)) ? 1 : 0;
        }
        return found;
    };

    check(count("pkill") == 0, "no pkill: nothing signals every vocemd of the user");
    if (harness) {
        check(count("systemctl --user start") == 0 && count("systemctl --user stop") == 0,
              "under the harness systemctl neither starts nor stops the daemon");
        check(count("systemctl --user cat") == 0, "and the unit is not even asked about");
        check(count("vocemd") == 0, "and no vocemd is exec'd");
        check(QFile::exists(token), "and Re-authorise did not remove the token");
    } else if (early_quit) {
        check(count("systemctl --user stop vocemd.service") == 1,
              "the stop waited for the unit question and went through systemctl");
        check(count("systemctl --user start") == 0,
              "and the start the window wanted on opening was dropped by the quit");
        check(count("vocemd") == 0, "and no vocemd was exec'd");
    } else {
        check(count("vocemd") == 0,
              "a unit question that outlived its cap did not exec a second vocemd");
        check(count("systemctl --user start vocemd.service") == 1,
              "the start went through systemctl, to the unit found on disk");
        check(count("systemctl --user stop vocemd.service") == 1, "and so did the stop");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
