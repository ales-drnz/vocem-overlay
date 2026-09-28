// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a sandbox can write into the host's application registry.
//
// A Flatpak game cannot write its record where the settings window reads it, so
// its overlay sends the record across the bridge and the daemon writes it on the
// host (FlatpakBridge::write_record_for). Through 0.1.10 the daemon wrote it
// under the name the sandbox gave -- the file is the process name -- and wrote a
// new one each time the name changed. So a sandbox rewriting its own request
// could fill ~/.cache/vocem/apps (the review's flood_probe: 5000 names, 5000
// files) and could overwrite any host application's record by naming it: a
// record saying `java` replaced the host's own Minecraft row, verdict and
// evidence included.
//
// One record per sandbox now, under a file name made from its application id
// that no host record can have, replaced when the name changes. This file is
// the sandbox doing both things at once, and the registry read back.

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "flatpak_bridge.h"
#include "probe_alarm.h"
#include "vocem/apps.h"
#include "vocem/flatpak.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

void make_directories(const std::string& path) {
    std::string partial;
    for (size_t i = 0; i <= path.size(); ++i) {
        if ((i == path.size() || path[i] == '/') && !partial.empty()) {
            mkdir(partial.c_str(), 0700);
        }
        if (i < path.size()) {
            partial.push_back(path[i]);
        }
    }
}

void write_file(const std::string& path, const std::string& body) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    fwrite(body.data(), 1, body.size(), file);
    fclose(file);
}

long files_in(const std::string& directory) {
    long count = 0;
    if (DIR* handle = opendir(directory.c_str())) {
        while (const dirent* entry = readdir(handle)) {
            if (entry->d_name[0] != '.') {
                ++count;
            }
        }
        closedir(handle);
    }
    return count;
}

long lines_with(const std::string& path, const char* needle) {
    long count = 0;
    if (FILE* file = fopen(path.c_str(), "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, needle)) {
                ++count;
            }
        }
        fclose(file);
    }
    return count;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    vocem_test::set_alarm(120, "what a sandbox writes into the registry");
    char scratch[] = "/tmp/vocem-flatpak-records-XXXXXX";
    if (!mkdtemp(scratch)) {
        printf("FAIL could not make a scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    setenv("XDG_RUNTIME_DIR", (root + "/run").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/cache").c_str(), 1);
    setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    setenv("XDG_DATA_DIRS", (root + "/none/flatpak/exports/share").c_str(), 1);
    unsetenv("FLATPAK_ID");
    const std::string apps = root + "/cache/vocem/apps";

    // A host application, recorded the way the overlay inside it records itself.
    vocem::Application host;
    host.key = "java";
    host.executable = "/usr/lib/jvm/java-21-openjdk/bin/java";
    host.api = "opengl";
    host.looks_like_game = true;
    host.reason = "minecraft";
    vocem::write_application_record(host);
    const std::vector<vocem::Application> before = vocem::known_applications();
    check(before.size() == 1 && before[0].key == "java", "the host's own record is there");

    const std::string request =
        root + "/run/app/org.evil.App/" + vocem::kBridgeDirName + "/" + vocem::kBridgeRequestName;
    make_directories(root + "/run/app/org.evil.App/" + vocem::kBridgeDirName);

    const std::string log = root + "/bridge.log";
    fflush(stderr);
    const int saved = dup(2);
    const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(out, 2);

    vocem::FlatpakBridge bridge;
    bridge.start();
    constexpr int kNames = 1000;
    for (int i = 0; i < kNames; ++i) {
        write_file(request, "pid=1\ndrawing=0\nname=n" + std::to_string(i) +
                                "\napi=vulkan\ngame=1\nwhy=forged\n");
        bridge.rescan();
    }
    // And the forgery: the name of the host's own application.
    write_file(request, "pid=1\ndrawing=0\nname=java\nexe=/app/bin/evil\napi=vulkan\ngame=0\n"
                        "why=forged\n");
    bridge.rescan();
    bridge.stop();

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(out);

    const long files = files_in(apps);
    printf("--  %d names and one forgery from one sandbox: %ld file(s) in the registry\n",
           kNames, files);
    check(files <= 2, "one sandbox leaves at most one record beside the host's");

    const std::vector<vocem::Application> after = vocem::known_applications();
    bool host_intact = false;
    bool sandbox_recorded = false;
    for (const vocem::Application& application : after) {
        if (application.executable == host.executable) {
            host_intact = application.key == "java" && application.reason == "minecraft" &&
                          application.looks_like_game && application.api == "opengl";
        }
        if (application.desktop == "org.evil.App") {
            sandbox_recorded = true;
        }
    }
    check(host_intact, "the host's own record is unchanged by a sandbox naming it");
    check(sandbox_recorded, "and the sandbox's record is there, as its own row");

    const long said = lines_with(log, "record");
    printf("--  the bridge spoke about records %ld time(s)\n", said);
    check(said <= 3, "and the renaming is not one log line per name");

    (void)!system(("rm -rf '" + root + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
