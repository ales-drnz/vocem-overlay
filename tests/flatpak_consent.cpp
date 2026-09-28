// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Who the bridge serves the voice channel to, decided on the host.
//
// `vocem/request` is a file inside the sandbox, written by whatever runs in
// there. Through 0.1.10 its `drawing=1` line was the whole of the decision: any
// Flatpak on the machine -- a chat client, a browser, anything the user never
// thought of as a game -- could write those sixteen bytes into its own
// $XDG_RUNTIME_DIR/app/<id>/vocem and be handed, on the next one-second sweep,
// the user's voice channel, every participant's name and face, and the words of
// every message Discord toasted (review of 2026-09-26: a bare request and a
// note_probe read "hey, the door code is 4812" out of the sandbox).
//
// The decision is the host's now, by application id -- the directory's name --
// and it is one of two things: the id's exported
// desktop entry says Game, or the user listed the id in `flatpak_apps`. This
// file is a sandbox that asks with each of those shapes and looks at what it was
// given: the note, the state mirror, the avatars and the emoji bank.
//
// And the directory's name is not evidence either: a sandbox holding the
// xdg-run/app grant can make `$XDG_RUNTIME_DIR/app/org.vinegarhq.Sober` itself
// (entries 134, 164), and was served as Sober through the first fix round. A
// directory is served only while a process of the user's runs in a sandbox
// whose /.flatpak-info names its id: the ids here that are meant to be served
// have one (tests/fake_flatpak.h, a nested bwrap), org.example.Absent has a
// Game entry and no process, and org.example.UserGame's process is stopped at
// the end.
//
// No shared segment: FlatpakBridge is given a state on the stack, and every
// directory it touches is a scratch one named through XDG_* here. bwrap only
// for the sandboxed processes; without it the test skips.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "fake_flatpak.h"
#include "flatpak_bridge.h"
#include "probe_alarm.h"
#include "vocem/avatar_rgba.h"
#include "vocem/flatpak.h"
#include "vocem/note.h"
#include "vocem/shared_state.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;
using vocem_test::write_file;
using vocem_test::read_file;

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

bool exists(const std::string& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0;
}

constexpr const char* kSecret = "hey, the door code is 4812";

// A Flatpak installation the way `flatpak install` leaves one: the exported
// entry is a symbolic link from exports/share/applications into the
// application's own deployed files (measured under /var/lib/flatpak on this
// machine, every one of the five exported entries).
void install_flatpak(const std::string& installation, const char* id, const char* categories) {
    const std::string deployed = installation + "/app/" + id +
                                 "/x86_64/stable/0123abcd/export/share/applications";
    make_directories(deployed);
    write_file(deployed + "/" + id + ".desktop",
               std::string("[Desktop Entry]\nType=Application\nName=Whatever\nExec=whatever\n"
                           "Categories=") +
                   categories + "\n");
    const std::string exports = installation + "/exports/share/applications";
    make_directories(exports);
    const std::string link = exports + "/" + id + ".desktop";
    unlink(link.c_str());
    (void)!symlink((deployed + "/" + id + ".desktop").c_str(), link.c_str());
}

struct Scene {
    std::string root;
    std::string runtime;
    std::string config;

    std::string bridge(const char* id) const { return runtime + "/app/" + id + "/vocem"; }

    void ask(const char* id) const {
        make_directories(bridge(id));
        write_file(bridge(id) + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=1\n");
    }
};

// What a sandbox was handed, read from its side of the wall.
struct Handed {
    bool note = false;
    bool channel = false;
    bool avatar = false;
    bool emoji = false;
};

Handed what_was_handed(const Scene& scene, const char* id, const vocem::SharedState& state) {
    Handed handed;
    const std::string dir = scene.bridge(id);
    handed.note = read_file(dir + "/" + vocem::kBridgeNoteName).find(kSecret) != std::string::npos;
    const std::string bytes = read_file(dir + "/" + vocem::kBridgeStateName);
    if (bytes.size() == sizeof(vocem::SharedState)) {
        vocem::SharedState mirrored{};
        memcpy(&mirrored, bytes.data(), sizeof(mirrored));
        handed.channel = mirrored.user_count != 0 || mirrored.in_channel != 0 ||
                         strstr(mirrored.channel_name, "Chilling") != nullptr;
    }
    char leaf[832];
    vocem::avatar_rgba_path(leaf, sizeof(leaf), state.users[0].id, state.users[0].avatar_hash);
    const char* slash = strrchr(leaf, '/');
    handed.avatar = exists(dir + "/" + vocem::kBridgeAvatarsName + "/" + (slash ? slash + 1 : leaf));
    handed.emoji = exists(dir + "/" + vocem::kBridgeEmojiBankName);
    return handed;
}

// One daemon tick, the way main.cpp drives the bridge, plus one message.
void tick(vocem::FlatpakBridge& bridge, const vocem::SharedState& state) {
    bridge.rescan();
    bridge.publish(state);
    bridge.refresh_files(state);
    bridge.publish_note(7, kSecret);
}

// Lines of the log that carry every one of the needles.
long count_in(const std::string& path, const char* needle, const char* also = nullptr) {
    long said = 0;
    if (FILE* file = fopen(path.c_str(), "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, needle) && (!also || strstr(line, also))) {
                ++said;
            }
        }
        fclose(file);
    }
    return said;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (!vocem_test::fake_flatpak_available()) {
        printf("skip bwrap is not installed, so no process can be put in a sandbox\n");
        return 77;
    }
    vocem_test::set_alarm(60, "who the bridge serves");
    char scratch[] = "/tmp/vocem-flatpak-consent-XXXXXX";
    if (!mkdtemp(scratch)) {
        printf("FAIL could not make a scratch directory\n");
        return 1;
    }
    Scene scene;
    scene.root = scratch;
    scene.runtime = scene.root + "/run";
    const std::string config_home = scene.root + "/config";
    const std::string cache_home = scene.root + "/cache";
    const std::string data_home = scene.root + "/data";
    const std::string system_flatpak = scene.root + "/system/flatpak";
    setenv("XDG_RUNTIME_DIR", scene.runtime.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_CACHE_HOME", cache_home.c_str(), 1);
    // The exported entries are found where the Base Directory variables say,
    // and only there: the user's installation under $XDG_DATA_HOME/flatpak and
    // every $XDG_DATA_DIRS root that is a Flatpak exports directory. Pointed at
    // scratch trees, so this machine's own installation is not consulted.
    setenv("XDG_DATA_HOME", data_home.c_str(), 1);
    setenv("XDG_DATA_DIRS", (system_flatpak + "/exports/share:/usr/share").c_str(), 1);
    unsetenv("FLATPAK_ID");

    // The emoji bank the bridge would copy: any file will do.
    const std::string bank = scene.root + "/bank/emoji_bank.rgba";
    make_directories(scene.root + "/bank");
    write_file(bank, "bank");
    write_file(scene.root + "/bank/" + vocem::kBridgeEmojiSequencesName, "table");
    setenv("VOCEM_EMOJI_BANK", bank.c_str(), 1);

    scene.config = config_home + "/vocem/config.ini";
    make_directories(config_home + "/vocem");
    write_file(scene.config, "[behaviour]\nflatpak_apps = org.example.Listed, org.example.Other\n");

    vocem::SharedState state{};
    state.abi_version = vocem::kAbiVersion;
    state.connected = 1;
    state.in_channel = 1;
    state.user_count = 1;
    snprintf(state.channel_name, sizeof(state.channel_name), "Chilling");
    state.users[0].id = 310503940594860049ULL;
    snprintf(state.users[0].name, sizeof(state.users[0].name), "Fazen");
    snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash),
             "a71d433becd902959baa0b8e59e9095c");
    make_directories(cache_home + "/vocem/avatars");
    char host_avatar[832];
    vocem::avatar_rgba_path(host_avatar, sizeof(host_avatar), state.users[0].id,
                            state.users[0].avatar_hash);
    write_file(host_avatar, std::string(vocem::kAvatarRgbaBytes, 'Z'));

    // The installations: a game in the user's, a game and a chat client in the
    // system one, and an entry whose exported link leaves its own application.
    install_flatpak(data_home + "/flatpak", "org.example.UserGame", "Game;ActionGame;");
    install_flatpak(system_flatpak, "org.example.SystemGame", "GNOME;GTK;Game;");
    install_flatpak(system_flatpak, "org.example.Chat", "Chat;Network;InstantMessaging;");
    install_flatpak(system_flatpak, "org.example.Tool", "Game;GameTool;");
    install_flatpak(system_flatpak, "org.example.Absent", "Game;");
    {
        // org.example.Borrowed's exported name, pointing at the game's entry.
        const std::string link =
            system_flatpak + "/exports/share/applications/org.example.Borrowed.desktop";
        (void)!symlink((system_flatpak +
                        "/app/org.example.SystemGame/x86_64/stable/0123abcd/export/share/"
                        "applications/org.example.SystemGame.desktop")
                           .c_str(),
                       link.c_str());
    }

    const char* refused[] = {"org.example.Stranger", "org.example.Chat", "org.example.Tool",
                             "org.example.Borrowed"};
    const char* served[] = {"org.example.Listed", "org.example.UserGame",
                            "org.example.SystemGame"};
    for (const char* id : refused) {
        scene.ask(id);
    }
    for (const char* id : served) {
        scene.ask(id);
    }
    // A Game entry, a directory that asks, and no process of it anywhere: the
    // shape a sandbox with the xdg-run/app grant makes under somebody else's id.
    const char* absent = "org.example.Absent";
    scene.ask(absent);

    // The applications that are meant to be served are running.
    vocem_test::FakeFlatpak running[3];
    for (int i = 0; i < 3; ++i) {
        running[i] = vocem_test::start_fake_flatpak(scene.root, served[i]);
        if (running[i].pid <= 0) {
            printf("FAIL could not start a sandboxed process for %s\n", served[i]);
            return 1;
        }
    }

    // Every bridge line goes to stderr; kept, so the refusal can be counted.
    const std::string log = scene.root + "/bridge.log";
    fflush(stderr);
    const int saved = dup(2);
    const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(out, 2);

    vocem::FlatpakBridge bridge;
    bridge.start();
    for (int i = 0; i < 5; ++i) {
        tick(bridge, state);
    }

    // The toast ends, the state stays: the note is published again for the
    // revocation case below.
    fflush(stderr);
    dup2(saved, 2);

    printf("--  %zu sandboxes adopted\n", bridge.served());
    for (const char* id : refused) {
        const Handed handed = what_was_handed(scene, id, state);
        printf("--  %-22s note=%d channel=%d avatar=%d emoji=%d\n", id, handed.note,
               handed.channel, handed.avatar, handed.emoji);
        check(!handed.note, std::string(id) + ": the message's words are not written into it");
        check(!handed.channel, std::string(id) + ": its state mirror holds no channel");
        check(!handed.avatar && !handed.emoji,
              std::string(id) + ": and neither faces nor the emoji bank are copied");
        const std::string line = std::string("not serving the voice channel to ") + id + ":";
        const long said = count_in(log, line.c_str(), "flatpak_apps");
        printf("--  %s: refusal said %ld time(s) over five sweeps\n", id, said);
        check(said == 1, std::string(id) +
                             ": the refusal is said once, naming the key that would allow it");
    }
    {
        const Handed handed = what_was_handed(scene, absent, state);
        printf("--  %-22s note=%d channel=%d avatar=%d emoji=%d\n", absent, handed.note,
               handed.channel, handed.avatar, handed.emoji);
        check(!handed.note && !handed.channel && !handed.avatar && !handed.emoji,
              std::string(absent) +
                  ": a Game's id with no process of it running is given nothing");
        const std::string line = std::string("not serving the voice channel to ") + absent + ":";
        const long said = count_in(log, line.c_str(), "no process");
        printf("--  %s: absence said %ld time(s) over five sweeps\n", absent, said);
        check(said == 1, std::string(absent) + ": and that is said once, naming the reason");
    }
    for (const char* id : served) {
        const Handed handed = what_was_handed(scene, id, state);
        printf("--  %-22s note=%d channel=%d avatar=%d emoji=%d\n", id, handed.note,
               handed.channel, handed.avatar, handed.emoji);
        check(handed.note && handed.channel && handed.avatar && handed.emoji,
              std::string(id) + ": served the channel, the words, the faces and the bank");
    }

    // The user changes their mind: the listed id comes off the list. The words
    // that were already written go at once, and the next publish is a cleared
    // state.
    write_file(scene.config, "[behaviour]\nflatpak_apps = org.example.Other\n");
    struct timespec later[2] = {{0, UTIME_OMIT}, {0, 0}};
    clock_gettime(CLOCK_REALTIME, &later[1]);
    later[1].tv_sec += 5;  // a different mtime whatever the filesystem's granularity
    utimensat(AT_FDCWD, scene.config.c_str(), later, 0);
    bridge.rescan();
    bridge.publish(state);
    const Handed withdrawn = what_was_handed(scene, "org.example.Listed", state);
    check(!withdrawn.note && !withdrawn.channel,
          "an id taken off flatpak_apps loses the words and the channel on the next sweep");
    // And the faces: until the second fix round the note was the only thing
    // taken back, and every face already copied stayed in the sandbox.
    check(!withdrawn.avatar, "and the faces already copied into it");

    // The game exits and its directory stays, as Flatpak leaves it: from now
    // on the directory is whoever writes into it.
    vocem_test::stop_fake_flatpak(running[1]);  // org.example.UserGame
    bool gone = false;
    for (int i = 0; i < 30 && !gone; ++i) {
        tick(bridge, state);
        const Handed after = what_was_handed(scene, "org.example.UserGame", state);
        gone = !after.note && !after.channel;
        if (!gone) {
            usleep(100 * 1000);
        }
    }
    check(gone, "an application whose process exited loses the words and the channel");
    check(!what_was_handed(scene, "org.example.UserGame", state).avatar, "and the faces");

    // The daemon stops: every face it put in a sandbox goes with the state.
    check(what_was_handed(scene, "org.example.SystemGame", state).avatar,
          "a served sandbox holds a face while the daemon runs");

    bridge.stop();
    check(!what_was_handed(scene, "org.example.SystemGame", state).avatar,
          "and none after the daemon stopped");
    for (vocem_test::FakeFlatpak& process : running) {
        vocem_test::stop_fake_flatpak(process);
    }
    close(saved);
    close(out);
    (void)!system(("rm -rf '" + scene.root + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
