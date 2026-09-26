// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A settings copy that failed is tried again.
//
// FlatpakBridge::mirror_config() copies config.ini into a sandbox when the
// host's file moves, and remembered the file's modification time as done even
// when the copy failed -- so that the failure would be said once per change and
// not once a second. The saying and the doing shared one variable: a copy that
// failed once (the sandbox's directory full, or briefly not writable) was never
// tried again until the user happened to change a setting, and the overlay in
// that game went on drawing with defaults or with stale settings.
//
// Held here: the sandbox's directory not writable for three sweeps, then
// writable, and the host's file never touched in between.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "flatpak_bridge.h"
#include "probe_alarm.h"
#include "vocem/flatpak.h"
#include "vocem/shared_state.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++failures;
    }
}

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

std::string read_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
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
    vocem_test::set_alarm(30, "a settings copy tried again");
    if (geteuid() == 0) {
        printf("skip running as root, which a read-only directory does not stop\n");
        return 77;
    }
    char scratch[] = "/tmp/vocem-flatpak-config-retry-XXXXXX";
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

    const std::string settings = "[appearance]\nscale = 1.75\n";
    make_directories(root + "/config/vocem");
    write_file(root + "/config/vocem/config.ini", settings);
    struct stat host_before {};
    stat((root + "/config/vocem/config.ini").c_str(), &host_before);

    const std::string bridge_dir = root + "/run/app/org.example.Game/vocem";
    make_directories(bridge_dir);
    write_file(bridge_dir + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=0\n");
    const std::string copy = bridge_dir + "/" + vocem::kBridgeConfigName;

    const std::string log = root + "/bridge.log";
    fflush(stderr);
    const int saved = dup(2);
    const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(out, 2);

    vocem::SharedState state{};
    state.abi_version = vocem::kAbiVersion;
    vocem::FlatpakBridge bridge;
    bridge.start();
    bridge.rescan();
    const bool adopted = bridge.served() == 1;

    // Not writable: the copy fails, three sweeps running.
    chmod(bridge_dir.c_str(), 0500);
    for (int i = 0; i < 3; ++i) {
        bridge.rescan();
        bridge.refresh_files(state);
    }
    const bool absent_while_closed = access(copy.c_str(), F_OK) != 0;

    // Writable again. Nothing about the host's file has changed.
    chmod(bridge_dir.c_str(), 0700);
    bridge.rescan();
    bridge.refresh_files(state);
    const std::string arrived = read_file(copy);
    bridge.stop();

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(out);

    check(adopted, "the sandbox is adopted");
    check(absent_while_closed, "while its directory is not writable the settings cannot arrive");
    struct stat host_after {};
    stat((root + "/config/vocem/config.ini").c_str(), &host_after);
    check(host_after.st_mtim.tv_sec == host_before.st_mtim.tv_sec &&
              host_after.st_mtim.tv_nsec == host_before.st_mtim.tv_nsec,
          "the host's config.ini is not touched in between");
    printf("--  after the directory became writable again the copy holds %zu bytes\n",
           arrived.size());
    check(arrived == settings, "the settings arrive on the next sweep, without a change to wait for");
    const long said = lines_with(log, "could not copy config.ini");
    printf("--  the failure was said %ld time(s) over three failing sweeps\n", said);
    check(said == 1, "and the failure was said once, not once a sweep");

    (void)!system(("chmod -R u+w '" + root + "'; rm -rf '" + root + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
