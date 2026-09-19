// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The bridge directory belongs to the sandbox, and the daemon does not.
//
// $XDG_RUNTIME_DIR/app/<id> is bind-mounted into the application, read-write.
// Everything the daemon writes for a Flatpak game therefore goes into a
// directory the game can rearrange first -- and the daemon is not sandboxed at
// all: it runs as the user, with the user's whole home reachable and the Discord
// token in it. A `state` that is really a symbolic link to ~/.ssh/authorized_keys
// would be opened, sized and written by a process that has every right to do so.
//
// So every name the daemon opens below that directory is opened O_NOFOLLOW and
// relative to a directory descriptor, and anything that is not a regular file is
// refused out loud. This file is the hostile sandbox: it replaces each of those
// names in turn with a link pointing at a file it must not be able to reach, and
// then looks at that file.
//
// No shared segment and no bwrap: the state is a struct on the stack, which is
// all FlatpakBridge is given. Nothing here can reach the running daemon.

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "probe_alarm.h"
#include "flatpak_bridge.h"
#include "vocem/avatar_rgba.h"
#include "vocem/flatpak.h"
#include "vocem/shared_state.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

constexpr const char* kSecret = "the user's own file, which no sandbox may steer a write into";

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
    char buffer[8192];
    const size_t got = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    return std::string(buffer, got);
}

// Whether the daemon gets through a piece of work at all, in a child with an
// alarm on it, so that "it blocked" is a result rather than a test that hangs.
//
// This is the shape the FIFO cases need. A refusal that arrives is a pass; a
// refusal that never arrives is the defect, and the two are indistinguishable
// from the calling process without this.
template <typename Fn>
bool completes_within(int seconds, Fn&& work) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(seconds);
        work();
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// One sandbox, arranged however the test wants, and the daemon let loose on it.
struct Sandbox {
    std::string runtime;
    std::string bridge;

    void reset(const std::string& root, const char* id) {
        runtime = root + "/run";
        bridge = runtime + "/app/" + id + "/" + vocem::kBridgeDirName;
        std::string command = "rm -rf '" + runtime + "'";
        (void)!system(command.c_str());
        make_directories(runtime + "/app/" + id);
        setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    }
};

}  // namespace

int main() {
    vocem_test::set_alarm(60, "hostile files in a sandbox");
    char scratch[] = "/tmp/vocem-flatpak-hostile-XXXXXX";
    if (!mkdtemp(scratch)) {
        printf("FAIL could not make a scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    const std::string config_home = root + "/config";
    const std::string cache_home = root + "/cache";
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_CACHE_HOME", cache_home.c_str(), 1);
    unsetenv("FLATPAK_ID");
    make_directories(config_home + "/vocem");
    write_file(config_home + "/vocem/config.ini", "[general]\nnotification_seconds = 7\n");
    make_directories(cache_home + "/vocem/avatars");

    // What the sandbox is trying to reach. Rewritten before every attempt so a
    // check cannot pass on a leftover.
    const std::string target = root + "/private";

    vocem::SharedState state{};
    state.abi_version = vocem::kAbiVersion;
    state.connected = 1;
    state.in_channel = 1;
    state.user_count = 1;
    state.users[0].id = 310503940594860049ULL;
    snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash),
             "a71d433becd902959baa0b8e59e9095c");
    // A cached face on the host, so the avatar copy has something real to do.
    unsigned char picture[vocem::kAvatarRgbaBytes];
    memset(picture, 0x5a, sizeof(picture));
    char host_avatar[832];
    vocem::avatar_rgba_path(host_avatar, sizeof(host_avatar), state.users[0].id,
                            state.users[0].avatar_hash);
    FILE* face = fopen(host_avatar, "wb");
    if (face) {
        fwrite(picture, 1, sizeof(picture), face);
        fclose(face);
    }

    Sandbox sandbox;

    // 1. The whole bridge directory is a link into the user's home.
    {
        write_file(target, kSecret);
        sandbox.reset(root, "org.example.LinkedDirectory");
        const std::string elsewhere = root + "/elsewhere";
        make_directories(elsewhere);
        check(symlink(elsewhere.c_str(), sandbox.bridge.c_str()) == 0,
              "a sandbox can make its vocem/ a link to somewhere else");
        write_file(elsewhere + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0, "a bridge directory that is a symbolic link is not adopted");
        struct stat info {};
        check(stat((elsewhere + "/" + vocem::kBridgeStateName).c_str(), &info) != 0,
              "and nothing is written through it");
    }

    // 2. The request is a link. A file the daemon can read is not the same thing
    //    as an overlay asking to be served.
    {
        write_file(target, kSecret);
        sandbox.reset(root, "org.example.LinkedRequest");
        make_directories(sandbox.bridge);
        check(symlink(target.c_str(), (sandbox.bridge + "/" + vocem::kBridgeRequestName).c_str()) ==
                  0,
              "a sandbox can make its request a link to one of the user's files");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0, "a request that is a symbolic link is not a request");
    }

    // 3. The state is a link to a file the daemon must not size or write.
    {
        write_file(target, kSecret);
        sandbox.reset(root, "org.example.LinkedState");
        make_directories(sandbox.bridge);
        write_file(sandbox.bridge + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        check(symlink(target.c_str(), (sandbox.bridge + "/" + vocem::kBridgeStateName).c_str()) == 0,
              "a sandbox can make its state a link to one of the user's files");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0, "a state that is a symbolic link is refused");
        bridge.publish(state);
        check(read_file(target) == kSecret,
              "and the file it pointed at is neither truncated nor written");
    }

    // 4. The state is a named pipe. O_NOFOLLOW says nothing about those, and a
    //    daemon that opened one would block on a reader the sandbox controls.
    {
        sandbox.reset(root, "org.example.FifoState");
        make_directories(sandbox.bridge);
        write_file(sandbox.bridge + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        check(mkfifo((sandbox.bridge + "/" + vocem::kBridgeStateName).c_str(), 0600) == 0,
              "a sandbox can leave a named pipe where the state should be");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0, "a state that is not a regular file is refused");
    }

    // 4b. The same, at `request`, which is opened O_RDONLY -- and O_RDONLY on a
    //     named pipe waits for a writer that never comes. This is the case the
    //     S_ISREG rule does not cover on its own, because the check is on the
    //     far side of the open: one mkfifo inside any sandbox held vocemd in
    //     rescan() before it had reached Discord at all, through a wait SIGTERM
    //     does not interrupt.
    {
        sandbox.reset(root, "org.example.FifoRequest");
        make_directories(sandbox.bridge);
        check(mkfifo((sandbox.bridge + "/" + vocem::kBridgeRequestName).c_str(), 0600) == 0,
              "a sandbox can leave a named pipe where the request should be");
        const bool answered = completes_within(3, [&] {
            vocem::FlatpakBridge bridge;
            bridge.start();
            bridge.rescan();
            if (bridge.served() != 0) {
                _exit(1);
            }
        });
        check(answered, "and the daemon comes back from rescan() rather than waiting for ever");
    }

    // 4c. And at the settings copy's temporary, which is opened O_WRONLY --
    //     which waits for a *reader*. Same class, other direction, and it
    //     reaches the daemon while it is running rather than only at startup.
    {
        sandbox.reset(root, "org.example.FifoConfig");
        make_directories(sandbox.bridge);
        write_file(sandbox.bridge + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        check(mkfifo((sandbox.bridge + "/" + vocem::kBridgeConfigName + ".part").c_str(), 0600) == 0,
              "a sandbox can leave a named pipe where the settings copy is written");
        const bool answered = completes_within(3, [&] {
            vocem::FlatpakBridge bridge;
            bridge.start();
            bridge.rescan();
            bridge.refresh_files(state);
        });
        check(answered, "and the daemon comes back from refresh_files() too");
    }

    // 4d. The application's own directory as a link. O_NOFOLLOW on the last
    //     component alone left this open: with write access to
    //     $XDG_RUNTIME_DIR/app, which --filesystem=xdg-run/app grants, the state
    //     and the settings went through it.
    {
        write_file(target, kSecret);
        sandbox.reset(root, "org.example.Unused");
        const std::string elsewhere = root + "/appdir";
        make_directories(elsewhere + "/" + vocem::kBridgeDirName);
        write_file(elsewhere + "/" + vocem::kBridgeDirName + "/" + vocem::kBridgeRequestName,
                   "pid=1\ndrawing=1\n");
        check(symlink(elsewhere.c_str(),
                      (sandbox.runtime + "/app/org.example.LinkedAppDir").c_str()) == 0,
              "a sandbox can make its own application directory a link");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0, "an application directory that is a symbolic link is refused");
        struct stat written {};
        check(stat((elsewhere + "/" + vocem::kBridgeDirName + "/" + vocem::kBridgeStateName).c_str(),
                   &written) != 0,
              "and no state is written through it");
    }

    // 4e. A state file swapped out after adoption. The daemon goes on writing
    //     the orphaned inode while the name it left behind stays empty, which
    //     from inside the sandbox reads exactly like a daemon that is not
    //     running -- and nothing ever puts it right, because the request is
    //     still there.
    {
        sandbox.reset(root, "org.example.SwappedState");
        make_directories(sandbox.bridge);
        write_file(sandbox.bridge + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 1, "a well-formed sandbox is adopted");
        unlink((sandbox.bridge + "/" + vocem::kBridgeStateName).c_str());
        write_file(sandbox.bridge + "/" + vocem::kBridgeStateName, "not the daemon's");
        bridge.rescan();
        bridge.publish(state);
        // Dropped and taken up again in one pass, which is the recovery rather
        // than a refusal: what must not happen is the daemon going on writing
        // the orphaned inode while the name the sandbox can see stays as the
        // sandbox left it, which reads from in there exactly like a daemon that
        // is not running.
        check(bridge.served() == 1, "the sandbox is picked up again on the same pass");
        struct stat now {};
        check(stat((sandbox.bridge + "/" + vocem::kBridgeStateName).c_str(), &now) == 0 &&
                  now.st_size == static_cast<off_t>(sizeof(vocem::SharedState)),
              "and the file the sandbox can see is the one being published into");
        const std::string bytes = read_file(sandbox.bridge + "/" + vocem::kBridgeStateName);
        uint32_t version = 0;
        memcpy(&version, bytes.data(), sizeof(version));
        check(version == vocem::kAbiVersion, "with the daemon's own state in it");
    }

    // 5 and 6. An adopted sandbox that then aims the copies at the user's files.
    {
        write_file(target, kSecret);
        sandbox.reset(root, "org.example.LinkedCopies");
        make_directories(sandbox.bridge);
        write_file(sandbox.bridge + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 1, "an asking sandbox is adopted");

        // config.ini is written through a temporary and renamed, so the
        // temporary is the name to aim at.
        check(symlink(target.c_str(),
                      (sandbox.bridge + "/" + vocem::kBridgeConfigName + ".part").c_str()) == 0,
              "a sandbox can put a link where the settings copy is written");
        // The avatars directory, likewise.
        const std::string elsewhere = root + "/faces";
        make_directories(elsewhere);
        check(symlink(elsewhere.c_str(),
                      (sandbox.bridge + "/" + vocem::kBridgeAvatarsName).c_str()) == 0,
              "and a link where the avatar directory should be");

        bridge.refresh_files(state);

        check(read_file(target) == kSecret, "the settings are not copied through the link");
        char leaf[832];
        vocem::avatar_rgba_path(leaf, sizeof(leaf), state.users[0].id, state.users[0].avatar_hash);
        const char* slash = strrchr(leaf, '/');
        struct stat info {};
        check(stat((elsewhere + "/" + (slash ? slash + 1 : leaf)).c_str(), &info) != 0,
              "and no avatar is written into the directory the link pointed at");
    }

    // 7. More sandboxes asking than any machine runs. Every mirror costs two
    //    descriptors, a copy of the emoji bank and a share of every publish;
    //    a process that can create directories under $XDG_RUNTIME_DIR/app --
    //    which the xdg-run/app grant hands a sandbox -- could make two
    //    thousand of them, and the daemon adopted every one until its own
    //    descriptors ran out and the socket, /proc/net/tcp and the segment
    //    stopped opening (entry 134). Counted rather than assumed: the
    //    daemon's descriptor table before and after.
    {
        sandbox.reset(root, "org.example.Many0");
        const int asking = 2000;
        for (int i = 0; i < asking; ++i) {
            const std::string dir =
                sandbox.runtime + "/app/org.example.Many" + std::to_string(i) + "/" +
                vocem::kBridgeDirName;
            make_directories(dir);
            write_file(dir + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        }
        const auto open_descriptors = [] {
            long count = 0;
            if (DIR* handle = opendir("/proc/self/fd")) {
                while (readdir(handle)) {
                    ++count;
                }
                closedir(handle);
            }
            return count;
        };
        const long before = open_descriptors();
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        const long after = open_descriptors();
        printf("     %d sandboxes asking: %zu served, %ld descriptors open before and %ld after\n",
               asking, bridge.served(), before, after);
        check(bridge.served() <= 32, "the daemon serves at most its ceiling of sandboxes");
        check(after - before <= 2 * 32 + 2,
              "and holds at most two descriptors per served one, however many asked");
        bridge.stop();
    }

    // 8. A directory whose name is not a Flatpak application id. Flatpak makes
    //    reverse-DNS names and nothing else; a process making anything else
    //    under $XDG_RUNTIME_DIR/app is not a sandbox this daemon should look
    //    inside.
    {
        sandbox.reset(root, "org.example.Shape");
        for (const char* odd : {"noDots", "with space.x", "..", "a\x01b.c", "-.-"}) {
            const std::string dir =
                sandbox.runtime + "/app/" + odd + "/" + vocem::kBridgeDirName;
            make_directories(dir);
            write_file(dir + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
        }
        vocem::FlatpakBridge bridge;
        bridge.start();
        bridge.rescan();
        check(bridge.served() == 0,
              "a directory that is not the shape of an application id is not adopted");
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
