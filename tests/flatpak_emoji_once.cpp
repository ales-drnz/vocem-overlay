// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The emoji bank crosses into a sandbox once, not once per adoption.
//
// flatpak_bridge.cpp says the bank -- 16.3 MB today, with its sequence table --
// is "copied exactly once" into a sandbox, and remembered that in the mirror.
// A mirror is dropped whenever the sandbox stops asking and a new one is made
// when it asks again, so a sandbox toggling its `request` had the whole bank
// copied again each time, on the daemon's main thread, in the tick every other
// deadline runs on.
//
// Counted here by the copy's inode: every copy is written to a temporary and
// renamed into place, and the temporary is created while the old copy still
// has its name, so each copy is a new inode where the previous one stood.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
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

ino_t inode_of(const std::string& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 ? info.st_ino : 0;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    vocem_test::set_alarm(60, "the emoji bank crossing once");
    char scratch[] = "/tmp/vocem-flatpak-emoji-XXXXXX";
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
    make_directories(root + "/config/vocem");
    write_file(root + "/config/vocem/config.ini", "flatpak_apps = org.example.Game\n");

    // A bank and its table, a megabyte of it: the size is not the point, the
    // count is.
    make_directories(root + "/bank");
    const std::string bank = root + "/bank/emoji_bank.rgba";
    write_file(bank, std::string(1 << 20, 'B'));
    write_file(root + "/bank/" + vocem::kBridgeEmojiSequencesName, "table");
    setenv("VOCEM_EMOJI_BANK", bank.c_str(), 1);

    const std::string dir = root + "/run/app/org.example.Game/vocem";
    make_directories(dir);
    const std::string request = dir + "/" + vocem::kBridgeRequestName;
    const std::string copy = dir + "/" + vocem::kBridgeEmojiBankName;
    write_file(request, "pid=1\ndrawing=1\n");

    vocem::SharedState state{};
    state.abi_version = vocem::kAbiVersion;
    vocem::FlatpakBridge bridge;
    bridge.start();
    bridge.rescan();
    bridge.refresh_files(state);
    ino_t last = inode_of(copy);
    check(last != 0, "the bank is copied into a sandbox that draws");

    constexpr int kToggles = 5;
    int copies = 0;
    for (int i = 0; i < kToggles; ++i) {
        unlink(request.c_str());
        bridge.rescan();  // stopped asking: the mirror goes
        write_file(request, "pid=1\ndrawing=1\n");
        bridge.rescan();  // asking again: a new mirror
        bridge.refresh_files(state);
        const ino_t now = inode_of(copy);
        if (now != last) {
            ++copies;
        }
        last = now;
    }
    printf("--  %d re-adoptions copied the bank %d more time(s)\n", kToggles, copies);
    check(copies == 0, "a sandbox asking again does not have the bank copied again");

    // A bank that changed on the host is a different bank: copied again.
    struct timespec later[2] = {{0, UTIME_OMIT}, {0, 0}};
    clock_gettime(CLOCK_REALTIME, &later[1]);
    later[1].tv_sec += 5;
    utimensat(AT_FDCWD, bank.c_str(), later, 0);
    unlink(request.c_str());
    bridge.rescan();
    write_file(request, "pid=1\ndrawing=1\n");
    bridge.rescan();
    bridge.refresh_files(state);
    check(inode_of(copy) != last && inode_of(copy) != 0,
          "and a bank that changed on the host is copied again");
    last = inode_of(copy);

    // A copy the sandbox removed is not there any more: copied again.
    unlink(copy.c_str());
    unlink(request.c_str());
    bridge.rescan();
    write_file(request, "pid=1\ndrawing=1\n");
    bridge.rescan();
    bridge.refresh_files(state);
    check(inode_of(copy) != 0, "and one the sandbox removed is put back");

    bridge.stop();
    (void)!system(("rm -rf '" + root + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
