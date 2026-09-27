// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Names in the avatar cache that are not the files they claim to be.
//
// The daemon writes every face through `<name>.part` and a rename, and the
// Flatpak bridge copies faces out of the cache into a sandbox. Through 0.1.10
// both opened by path the way a trusting program does: `fopen(temporary, "wb")`
// followed a symbolic link planted at the temporary name -- a dangling one
// created its target -- and blocked for ever on a FIFO there; `fopen(source,
// "rb")` followed a link at a cache name into whatever it named and copied it
// into the sandbox, and blocked on a FIFO, holding rescan() past SIGTERM until
// the unit's SIGKILL left the segment behind (entry 81's leftover). Measured by
// the review's partlink, partfifo and bridge_sig probes.
//
// Each case here plants one of those shapes and looks at what it reaches. The
// ones that could hang run in a forked child with an alarm, so "it blocked" is a
// result and not a test that never ends.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "fake_flatpak.h"
#include "flatpak_bridge.h"
#include "probe_alarm.h"
#include "vocem/avatar_rgba.h"
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
    char buffer[8192];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
}

bool exists(const std::string& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0;
}

// The work, in a child with an alarm. True when it came back by itself.
template <typename Fn>
bool completes_within(unsigned seconds, Fn&& work) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        // The default action, so that the alarm kills the child: the probe's
        // own handler (probe_alarm.h) would report the parent's deadline.
        signal(SIGALRM, SIG_DFL);
        alarm(seconds);
        work();
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

constexpr uint64_t kUser = 310503940594860049ULL;
constexpr const char* kHash = "a71d433becd902959baa0b8e59e9095c";
constexpr const char* kSecret = "the user's token, which is exactly the size of nothing in particular";

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    vocem_test::set_alarm(60, "hostile names in the avatar cache");
    char scratch[] = "/tmp/vocem-avatar-hostile-XXXXXX";
    if (!mkdtemp(scratch)) {
        printf("FAIL could not make a scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    const std::string cache_home = root + "/cache";
    const std::string config_home = root + "/config";
    setenv("XDG_CACHE_HOME", cache_home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (root + "/run").c_str(), 1);
    setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    setenv("XDG_DATA_DIRS", (root + "/none/flatpak/exports/share").c_str(), 1);
    setenv("VOCEM_EMOJI_BANK", (root + "/no-bank").c_str(), 1);
    unsetenv("FLATPAK_ID");
    make_directories(cache_home + "/vocem/avatars");

    char path[832];
    vocem::avatar_rgba_path(path, sizeof(path), kUser, kHash);
    const std::string cached = path;
    const std::string part = cached + ".part";
    unsigned char face[vocem::kAvatarRgbaBytes];
    memset(face, 'F', sizeof(face));

    // 1. The temporary is a link to a file of the user's.
    {
        const std::string victim = root + "/victim";
        write_file(victim, kSecret);
        unlink(part.c_str());
        check(symlink(victim.c_str(), part.c_str()) == 0, "a link can be planted at the temporary");
        const bool wrote = vocem::avatar_rgba_write(path, face, vocem::kAvatarPixels,
                                                    vocem::kAvatarPixels);
        printf("--  write returned %d, the victim is now %zu bytes\n", wrote,
               read_file(victim).size());
        check(read_file(victim) == kSecret, "the file the link named is not written through it");
        struct stat info {};
        check(lstat(cached.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                  info.st_size == static_cast<off_t>(vocem::kAvatarRgbaBytes),
              "and the face lands in the cache as a regular file of its one size");
        unlink(cached.c_str());
    }

    // 2. The temporary is a dangling link: following it would create its target.
    {
        const std::string target = root + "/created-by-the-daemon";
        unlink(part.c_str());
        check(symlink(target.c_str(), part.c_str()) == 0, "a dangling link can be planted");
        vocem::avatar_rgba_write(path, face, vocem::kAvatarPixels, vocem::kAvatarPixels);
        check(!exists(target), "and nothing is created where it pointed");
        unlink(cached.c_str());
    }

    // 3. The temporary is a FIFO: opening it for writing waits for a reader.
    {
        unlink(part.c_str());
        check(mkfifo(part.c_str(), 0600) == 0, "a FIFO can be planted at the temporary");
        const bool back = completes_within(3, [&] {
            vocem::avatar_rgba_write(path, face, vocem::kAvatarPixels, vocem::kAvatarPixels);
        });
        check(back, "the writer comes back rather than waiting for a reader");
        unlink(part.c_str());
        unlink(cached.c_str());
    }

    // The bridge's copies, into a sandbox the host consents to serve: listed
    // in flatpak_apps, and with a process of it running, without which nothing
    // is copied at all and the checks below would pass for nothing.
    const char* id = "org.example.Game";
    if (!vocem_test::fake_flatpak_available()) {
        printf("skip bwrap is not installed, so no process can be put in a sandbox\n");
        return 77;
    }
    vocem_test::FakeFlatpak game = vocem_test::start_fake_flatpak(root, id);
    if (game.pid <= 0) {
        printf("FAIL could not start a sandboxed process for %s\n", id);
        return 1;
    }
    const std::string bridge_dir = root + "/run/app/" + id + "/vocem";
    make_directories(bridge_dir);
    write_file(bridge_dir + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
    make_directories(config_home + "/vocem");
    write_file(config_home + "/vocem/config.ini", std::string("flatpak_apps = ") + id + "\n");
    vocem::SharedState state{};
    state.abi_version = vocem::kAbiVersion;
    state.user_count = 1;
    state.users[0].id = kUser;
    snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kHash);
    const char* leaf = strrchr(path, '/') + 1;
    const std::string mirrored = bridge_dir + "/" + vocem::kBridgeAvatarsName + "/" + leaf;

    // 4. A cache name that is a link to one of the user's files.
    {
        const std::string token = root + "/token";
        write_file(token, kSecret);
        unlink(cached.c_str());
        check(symlink(token.c_str(), cached.c_str()) == 0, "a link can be planted at a cache name");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        bridge.refresh_files(state);
        const std::string copied = read_file(mirrored);
        printf("--  the sandbox's copy of that face holds %zu bytes%s\n", copied.size(),
               copied == kSecret ? ", and they are the token's" : "");
        check(copied.find("token") == std::string::npos,
              "what the link named is not copied into the sandbox");
        bridge.stop();
        unlink(cached.c_str());
    }

    // 5. A cache name that is a FIFO: opening it for reading waits for a writer.
    {
        unlink(mirrored.c_str());
        check(mkfifo(cached.c_str(), 0600) == 0, "a FIFO can be planted at a cache name");
        const bool back = completes_within(3, [&] {
            vocem::FlatpakBridge bridge;
            bridge.start();
            bridge.rescan();
            bridge.refresh_files(state);
            bridge.stop();
        });
        check(back, "the bridge's copy comes back rather than waiting for a writer");
        unlink(cached.c_str());
    }

    // 6. A cache name that is a regular file of the wrong size: not a face.
    {
        write_file(cached, "short");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        bridge.refresh_files(state);
        check(!exists(mirrored), "a cache file of any other size is not copied as a face");
        bridge.stop();
    }

    // 7. The control: a face that is a face is copied, so the refusals above
    //    are the bridge refusing and not the bridge doing nothing.
    {
        vocem::avatar_rgba_write(path, face, vocem::kAvatarPixels, vocem::kAvatarPixels);
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        bridge.refresh_files(state);
        check(read_file(mirrored).size() == vocem::kAvatarRgbaBytes,
              "and a face of the format's one size is copied into the sandbox");
        bridge.stop();
    }
    vocem_test::stop_fake_flatpak(game);

    (void)!system(("rm -rf '" + root + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
