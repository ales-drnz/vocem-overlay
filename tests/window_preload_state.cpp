// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// "Active in this session" is said only when this session has the preload.
//
// The Debug page's OpenGL row reads openglPreloadActive, which was true when
// EITHER this process's own LD_PRELOAD named the shim OR the systemd user
// manager's environment did. The two are not the same fact: a program
// launched from Plasma inherits plasmashell's environment, fixed at login,
// not the manager's -- so with the environment.d file installed after login
// the manager carries the preload and no game started from the desktop gets
// it until the next login, while the page said "Active in this session" and
// "Ready" (environment.h's own comment records that this split was measured
// on this machine).
//
// The bridge is built directly with a stub `systemctl` first on PATH, whose
// show-environment prints the shim or not, and this process's own LD_PRELOAD
// chosen per case. VOCEM_CONFIG_NO_DAEMON keeps the unit question and the
// daemon out of it; /dev/shm is private and every XDG directory is scratch.
// The properties are read by name, so the probe builds against a bridge that
// does not have openglPreloadInManager yet -- and reads it as false.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>

#include "config_bridge.h"
#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

struct Seen {
    bool known = false;
    bool active = false;
    bool manager = false;
};

// One bridge, until its preload question is answered (or five seconds).
Seen ask(const QByteArray& own_preload) {
    if (own_preload.isEmpty()) {
        qunsetenv("LD_PRELOAD");
    } else {
        qputenv("LD_PRELOAD", own_preload);
    }
    ConfigBridge bridge;
    QElapsedTimer clock;
    clock.start();
    while (!bridge.property("openglPreloadKnown").toBool() && clock.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    qunsetenv("LD_PRELOAD");
    return {bridge.property("openglPreloadKnown").toBool(),
            bridge.property("openglPreloadActive").toBool(),
            bridge.property("openglPreloadInManager").toBool()};
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int sandbox = vocem_test::ensure_private_shm(false);
    if (sandbox != -1) {
        return sandbox;
    }
    vocem_test::set_alarm(60, "the preload row");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const QString root = scratch.path();
    for (const char* sub : {"bin", "config/vocem", "cache/vocem", "state", "data"}) {
        QDir().mkpath(root + '/' + sub);
    }
    // show-environment answers with the manager's environment; which one is
    // decided by a file, so one stub serves every case.
    const QString manager_has = root + "/manager-has-shim";
    {
        QFile stub(root + "/bin/systemctl");
        if (stub.open(QIODevice::WriteOnly)) {
            stub.write("#!/bin/sh\n"
                       "if [ \"$2\" = show-environment ]; then\n"
                       "  echo 'PATH=/usr/bin'\n"
                       "  [ -e '" + manager_has.toLocal8Bit() + "' ] && "
                       "echo 'LD_PRELOAD=:/usr/$LIB/libvocem_gl_shim.so'\n"
                       "fi\n"
                       "exit 0\n");
        }
        stub.close();
        ::chmod((root + "/bin/systemctl").toLocal8Bit().constData(), 0755);
    }
    qputenv("PATH", (root + "/bin:").toLocal8Bit() + qgetenv("PATH"));
    qputenv("HOME", root.toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root + "/config").toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (root + "/cache").toLocal8Bit());
    qputenv("XDG_STATE_HOME", (root + "/state").toLocal8Bit());
    qputenv("XDG_DATA_HOME", (root + "/data").toLocal8Bit());
    qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("LD_PRELOAD");

    QGuiApplication app(argc, argv);

    // The case: the manager has it, this session does not.
    {
        QFile marker(manager_has);
        if (!marker.open(QIODevice::WriteOnly)) {
            std::printf("FAIL could not create the marker\n");
            return 1;
        }
    }
    const Seen manager_only = ask(QByteArray());
    std::printf("--  manager only: known %d, active %d, in manager %d\n", manager_only.known,
                manager_only.active, manager_only.manager);
    check(manager_only.known, "the question was answered");
    check(!manager_only.active,
          "with only the service manager carrying it, the row does not say this session has it");
    check(manager_only.manager, "and says the service manager does");

    // Both: this session has it, which is the whole answer.
    const Seen both = ask(QByteArrayLiteral(":/usr/$LIB/libvocem_gl_shim.so"));
    check(both.known && both.active, "with this session's own LD_PRELOAD naming the shim, it is active");

    // Neither.
    QFile::remove(manager_has);
    const Seen neither = ask(QByteArray());
    check(neither.known && !neither.active && !neither.manager, "with neither, it is neither");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
