// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The switch beside an application whose process name the lists cannot hold.
//
// The kernel cuts a process name to fifteen bytes, and a cut can land after a
// space: an executable called "Slay the Spire 2" runs as "Slay the Spire ".
// No list entry can be that name -- every reader trims the space away
// (vocem/apps.h, list_entry_fits) -- and the registry's reader trims it too,
// so the page names the application "Slay the Spire". Measured on the first
// round's bridge: the switch stored `hidden_apps = Slay the Spire`, which is
// neither the process name nor the executable's and matches nothing
// (draw_here compares the untrimmed name), while the page showed the
// application hidden. A name with a comma was refused outright, although its
// executable's name fitted.
// The executable's own name fits, and the overlay matches a rule against it
// too, so the switch writes that instead; it refuses only when neither fits.
// A name that fits is still preferred: under Proton the executable is wine's
// loader, and a rule on it would take in every Windows game.
//
// The registry record is written here in the shape common/src/apps.cpp writes
// it; the bridge is built directly (VOCEM_CONFIG_NO_DAEMON, scratch XDG
// directories, offscreen), as config_bridge_switches.cpp builds it.

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "config_bridge.h"
#include "probe_alarm.h"
#include "vocem/apps.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

void write_record(const QString& directory, const QString& file, const std::string& name,
                  const std::string& executable) {
    QFile record(directory + "/" + file);
    if (record.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const std::string text = "name = " + name + "\nexecutable = " + executable +
                                 "\napi = vulkan\ngame = true\nwhy = steam:1\nseen = 1700000000\n";
        record.write(text.data(), static_cast<qint64>(text.size()));
    }
}

// The name the page shows for the application whose executable is `executable`.
QString page_name(const ConfigBridge& bridge, const QString& executable) {
    for (const QVariant& entry : bridge.applications()) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("executable")).toString() == executable) {
            return map.value(QStringLiteral("name")).toString();
        }
    }
    return {};
}

// What draw_here asks of a list for a process with this comm and executable.
bool names_it(const std::string& list, const std::string& comm, const std::string& binary) {
    return vocem::listed(list, comm) || vocem::listed(list, binary);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    vocem_test::set_alarm(60, "the switch beside a cut process name");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const QString config_home = scratch.path() + "/config";
    const QString cache_home = scratch.path() + "/cache";
    QDir().mkpath(config_home + "/vocem");
    QDir().mkpath(cache_home + "/vocem/apps");
    qputenv("XDG_CONFIG_HOME", config_home.toUtf8());
    qputenv("XDG_CACHE_HOME", cache_home.toUtf8());
    qputenv("VOCEM_CONFIG_NO_DAEMON", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");

    // The cut name, as the kernel gives it, and the executable behind it.
    const std::string comm = "Slay the Spire ";
    const std::string binary = "Slay the Spire 2";
    const QString spire_exe = "/games/sts2/" + QString::fromStdString(binary);
    write_record(cache_home + "/vocem/apps", "Slay_the_Spire_", comm, spire_exe.toStdString());
    // A Proton title: the process name fits, the executable is wine's loader.
    const QString wine_exe = "/opt/proton/files/bin/wine64-preloader";
    write_record(cache_home + "/vocem/apps", "Hades.exe", "Hades.exe", wine_exe.toStdString());

    // A process name with the separator in it, over an executable whose name
    // fits (a program that renamed itself), and one where neither fits.
    const QString renamed_exe = "/games/foobar";
    write_record(cache_home + "/vocem/apps", "Foo__Bar", "Foo, Bar", renamed_exe.toStdString());
    const QString comma_exe = "/games/Baz, Qux";
    write_record(cache_home + "/vocem/apps", "Baz__Qux", "Baz, Qux", comma_exe.toStdString());

    QGuiApplication app(argc, argv);
    ConfigBridge bridge;
    const QString spire = page_name(bridge, spire_exe);
    std::printf("--  the page names the application '%s'\n", spire.toUtf8().constData());
    check(!spire.isEmpty(), "the page lists the application");

    // Hide it: the switch a game gets.
    bridge.setApplicationDrawn(spire, false, true);
    std::string hidden = bridge.hiddenApps().toStdString();
    std::printf("--  hidden_apps = '%s', message '%s'\n", hidden.c_str(),
                bridge.saveError().toUtf8().constData());
    check(names_it(hidden, comm, binary),
          "hidden_apps names the process the way the overlay matches it");
    check(bridge.saveError().isEmpty(), "and nothing was refused");
    check(!vocem::listed(hidden, "Slay the Spire"),
          "and no trimmed name that matches nothing was stored");

    // And back: the rule on the executable's name is taken out again.
    bridge.setApplicationDrawn(spire, true, true);
    hidden = bridge.hiddenApps().toStdString();
    check(!names_it(hidden, comm, binary), "switching it back on takes the rule out");

    // The Proton title keeps its rule on the process name, not on wine's loader.
    const QString hades = page_name(bridge, wine_exe);
    bridge.setApplicationDrawn(hades, false, true);
    hidden = bridge.hiddenApps().toStdString();
    check(vocem::listed(hidden, "Hades.exe"), "a name that fits is stored as itself");
    check(!vocem::listed(hidden, "wine64-preloader"),
          "and never as the loader every Windows game shares");

    // The separator: the executable's name stands in when it fits...
    bridge.setApplicationDrawn(page_name(bridge, renamed_exe), false, true);
    hidden = bridge.hiddenApps().toStdString();
    check(vocem::listed(hidden, "foobar") && !vocem::listed(hidden, "Foo") &&
              !vocem::listed(hidden, "Bar"),
          "a process name with a comma is hidden by its executable's name, not as two others");
    // ...and the switch is refused, and says so, when neither fits.
    const std::string before_refusal = hidden;
    bridge.setApplicationDrawn(page_name(bridge, comma_exe), false, true);
    check(bridge.hiddenApps().toStdString() == before_refusal,
          "a name neither spelling of which fits leaves the list alone");
    check(!bridge.saveError().isEmpty(), "and the window says why");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
