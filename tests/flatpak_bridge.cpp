// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The two sides of a Flatpak sandbox, held against each other.
//
// A game that is itself a Flatpak cannot see the daemon's shared segment, its
// config.ini or its avatar cache -- all three are behind the sandbox. What it
// can see is $XDG_RUNTIME_DIR/app/<its own id>, which Flatpak bind-mounts from
// the host, and that is where the daemon puts copies for the sandboxes whose
// overlay asked to be served.
//
// The point of this file is that neither half is checked against itself. The
// daemon's FlatpakBridge writes; a **forked child** with FLATPAK_ID set is the
// game, calls the same enter_flatpak_bridge() the injected code calls, and reads
// back with the same StateReader, the same Config and the same avatar loader a
// real overlay uses. Entry 55 is what proving one side against its own idea of
// the layout costs: every 32-bit game read empty rows for four packages while
// both halves agreed with themselves.
//
// The child also unlinks the POSIX segment before reading, so nothing it gets
// right can have come from /dev/shm. That is the whole claim: the state crossed
// the wall.
//
// A private /dev/shm, because a StateWriter is opened here -- see private_shm.h
// for what publishing on the real one cost once.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "probe_alarm.h"
#include "flatpak_bridge.h"
#include "vocem/apps.h"
#include "vocem/avatar_rgba.h"
#include "vocem/config.h"
#include "vocem/flatpak.h"
#include "vocem/note.h"
#include "vocem/shm.h"
#include "private_shm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

constexpr uint64_t kUserId = 310503940594860049ULL;
constexpr const char* kUserHash = "a71d433becd902959baa0b8e59e9095c";
constexpr const char* kServed = "org.example.Game";
constexpr const char* kSilent = "org.example.NotAsking";
constexpr uint64_t kNoteSerial = 7;

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

void write_file(const std::string& path, const void* data, size_t length) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    fwrite(data, 1, length, file);
    fclose(file);
}

// One avatar's worth of bytes, recognisable and the format's exact size.
void fill_avatar(unsigned char* out) {
    for (uint32_t i = 0; i < vocem::kAvatarRgbaBytes; ++i) {
        out[i] = static_cast<unsigned char>(i * 7u);
    }
}

// The game's half, in a child so that turning the bridge on cannot leak back
// into the daemon's half running in the parent.
int run_as_the_game(bool drawing) {
    setenv("FLATPAK_ID", kServed, 1);

    // Nothing this child reads may come from the segments the parent published
    // on the host: without the names, shm_open can only fail.
    char name[64];
    vocem::shm_name(name, sizeof(name), getuid());
    shm_unlink(name);
    vocem::note_shm_name(name, sizeof(name), getuid());
    shm_unlink(name);

    if (!vocem::enter_flatpak_bridge()) {
        printf("FAIL the overlay could not enter the bridge\n");
        return 1;
    }
    int child_failures = 0;
    auto expect = [&](bool condition, const char* what) {
        printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) {
            ++child_failures;
        }
    };

    expect(vocem::bridge_in_use(), "the overlay knows it is behind a sandbox");

    char root[512];
    vocem::bridge_root(root, sizeof(root));
    expect(strstr(root, kServed) != nullptr && strstr(root, "/vocem") != nullptr,
           "and the bridge it uses is its own application's");

    // The configuration: the same Config the panel reads, pointed at the copy.
    const std::string config_path = vocem::Config::path();
    expect(config_path.rfind(root, 0) == 0, "config.ini is read from the bridge");
    vocem::Config config;
    config.load();
    expect(config.notification_seconds > 10.9f && config.notification_seconds < 11.1f,
           "and it is the settings the user actually wrote, not the defaults");

    // The avatar: the same loader, pointed at the copy.
    char avatar_path[832];
    vocem::avatar_rgba_path(avatar_path, sizeof(avatar_path), kUserId, kUserHash);
    expect(strncmp(avatar_path, root, strlen(root)) == 0,
           "the avatar is looked for inside the bridge");
    unsigned char loaded[vocem::kAvatarRgbaBytes];
    unsigned char wanted[vocem::kAvatarRgbaBytes];
    fill_avatar(wanted);

    vocem::StateReader reader;
    expect(reader.open(), "the state opens by path where shm_open has nothing to give");
    vocem::Snapshot snapshot;
    expect(reader.read(snapshot), "and reads under the same seqlock");

    if (!drawing) {
        // The overlay in this sandbox has said it is not drawing -- the user's
        // switch, or an application the detection does not call a game. The
        // settings still have to arrive, because they are what that decision is
        // made from; nothing else may.
        expect(snapshot.user_count == 0 && snapshot.channel_name[0] == '\0',
               "an overlay that is not drawing is sent no channel and nobody in it");
        expect(!vocem::avatar_rgba_load(avatar_path, loaded),
               "and none of the faces of the people in it");
        return child_failures;
    }

    expect(vocem::avatar_rgba_load(avatar_path, loaded),
           "the picture the daemon cached is there at the format's one size");
    expect(memcmp(loaded, wanted, sizeof(loaded)) == 0, "and it is the same bytes");
    expect(strcmp(snapshot.channel_name, "Chilling") == 0,
           "the channel the daemon published crossed the sandbox");
    expect(snapshot.user_count == 1 && snapshot.users[0].id == kUserId,
           "and so did the whole 64-bit id of the person in it");
    expect(strcmp(snapshot.users[0].name, "Fazen") == 0, "and their name");
    expect(snapshot.connected != 0 && snapshot.in_channel != 0, "and the daemon's own status");

    // The words of a message. They are not in the segment above and never were
    // -- they live in a place of their own that exists only while the toast is
    // on screen -- so they need their own crossing, and a toast with a name and
    // a face and no text is what happens when they do not get one.
    vocem::NoteReader note;
    expect(strcmp(note.body_for(kNoteSerial), "THE-SECRET-TEXT") == 0,
           "the message's words crossed the sandbox too");
    return child_failures;
}

int in_child(bool drawing) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        const int child_failures = run_as_the_game(drawing);
        fflush(stdout);  // _exit does not, and the child's lines are the evidence
        _exit(child_failures);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        printf("FAIL the game's half died on signal %d\n", WTERMSIG(status));
        return 1;
    }
    return WEXITSTATUS(status);
}

}  // namespace

int main() {
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "the bridge, both halves");

    char scratch[] = "/tmp/vocem-flatpak-bridge-XXXXXX";
    if (!mkdtemp(scratch)) {
        printf("FAIL could not make a scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    const std::string runtime = root + "/run";
    const std::string config_home = root + "/config";
    const std::string cache_home = root + "/cache";
    setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    setenv("XDG_CACHE_HOME", cache_home.c_str(), 1);
    unsetenv("FLATPAK_ID");

    // What the host has: a settings file the user wrote, and one cached face.
    make_directories(config_home + "/vocem");
    const std::string config_ini = "[general]\nnotification_seconds = 11\n";
    write_file(config_home + "/vocem/config.ini", config_ini.data(), config_ini.size());
    make_directories(cache_home + "/vocem/avatars");
    unsigned char avatar[vocem::kAvatarRgbaBytes];
    fill_avatar(avatar);
    char host_avatar[832];
    vocem::avatar_rgba_path(host_avatar, sizeof(host_avatar), kUserId, kUserHash);
    write_file(host_avatar, avatar, sizeof(avatar));

    // Two sandboxes: one whose overlay asked to be served, one that never did.
    make_directories(runtime + "/app/" + kServed + "/vocem");
    make_directories(runtime + "/app/" + kSilent);
    write_file(std::string(runtime + "/app/" + kServed + "/vocem/request"),
               "pid=1\ndrawing=0\n", 18);

    vocem::StateWriter writer;
    if (!writer.open()) {
        printf("FAIL could not open the segment even inside the sandbox\n");
        return 1;
    }

    vocem::FlatpakBridge bridge;
    check(bridge.start(), "the daemon finds $XDG_RUNTIME_DIR/app");
    bridge.rescan();
    check(bridge.served() == 1, "and serves exactly the one sandbox that asked");

    writer.on_publish_context = &bridge;
    writer.on_publish = [](const vocem::SharedState& state, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish(state);
    };

    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = static_cast<uint32_t>(vocem::DaemonStatus::Connected);
        state.user_count = 1;
        snprintf(state.channel_name, sizeof(state.channel_name), "Chilling");
        state.users[0].id = kUserId;
        state.users[0].flags = vocem::kFlagSelf;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Fazen");
        snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kUserHash);
    });
    bridge.refresh_files(*writer.state());

    // A sandbox that never asked must have nothing written into it -- the state
    // is the user's voice channel and it does not go where it was not wanted.
    struct stat unwanted {};
    check(stat((runtime + "/app/" + kSilent + "/vocem").c_str(), &unwanted) != 0,
          "nothing at all is written into a sandbox that did not ask");

    // First with the overlay in that sandbox saying it is not drawing, which is
    // what it says until it has read the settings -- and what it goes on saying
    // if the user has switched the overlay off for that game.
    failures += in_child(false);

    // Then with it drawing.
    write_file(std::string(runtime + "/app/" + kServed + "/vocem/request"),
               "pid=1\ndrawing=1\n", 18);
    bridge.rescan();
    bridge.publish(*writer.state());
    bridge.refresh_files(*writer.state());

    vocem::NoteWriter note;
    note.on_publish_context = &bridge;
    note.on_publish = [](uint64_t serial, const char* body, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish_note(serial, body);
    };
    note.publish(kNoteSerial, "THE-SECRET-TEXT");

    failures += in_child(true);

    // The toast is over: the words go, on both sides of the wall at once.
    note.clear();
    struct stat words {};
    check(stat((runtime + "/app/" + std::string(kServed) + "/vocem/note").c_str(), &words) != 0,
          "and they are taken away when the toast ends, not left behind");

    // The settings copy is written to a temporary and renamed into place, so a
    // game reading on the other side never meets a half-written one. Asked as
    // "did the name come to mean a different object": a rename always installs a
    // new inode, and a copy written straight into the final name always reuses
    // the old one. Checking only that no `.part` is left behind does not
    // distinguish the two -- it passes for an implementation that has no
    // temporary at all.
    const std::string mirrored_config =
        runtime + "/app/" + std::string(kServed) + "/vocem/config.ini";
    struct stat before {};
    check(stat(mirrored_config.c_str(), &before) == 0, "the settings arrived");
    const std::string longer = "[general]\nnotification_seconds = 13\nopacity = 0.5\n";
    write_file(config_home + "/vocem/config.ini", longer.data(), longer.size());
    bridge.refresh_files(*writer.state());
    struct stat after {};
    check(stat(mirrored_config.c_str(), &after) == 0 && after.st_ino != before.st_ino,
          "and a changed settings file is renamed into place rather than written over");
    struct stat partial {};
    check(stat((mirrored_config + ".part").c_str(), &partial) != 0,
          "with no half-written one left beside it");

    // The seqlock's counter moves forward and lands even, every time. A reader
    // that saw it stand still over a rewritten payload would believe the
    // payload.
    const std::string mirror = runtime + "/app/" + kServed + "/vocem/state";
    uint32_t sequences[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        writer.publish([](vocem::SharedState& state) { state.user_count = 1; });
        const int fd = open(mirror.c_str(), O_RDONLY);
        if (fd >= 0) {
            (void)!pread(fd, &sequences[i], sizeof(uint32_t),
                         static_cast<off_t>(offsetof(vocem::SharedState, sequence)));
            close(fd);
        }
    }
    check(sequences[0] % 2 == 0 && sequences[1] > sequences[0] && sequences[2] > sequences[1] &&
              sequences[2] % 2 == 0,
          "every publish moves the mirror's sequence forward and leaves it even");

    // The reader refuses a mirror from another ABI rather than interpreting it.
    // Written straight into the file, which is what a daemon of another version
    // would have left there.
    const std::string state_file = runtime + "/app/" + kServed + "/vocem/state";
    const int fd = open(state_file.c_str(), O_RDWR);
    const uint32_t wrong = vocem::kAbiVersion + 1;
    check(fd >= 0 && pwrite(fd, &wrong, sizeof(wrong), 0) == sizeof(wrong),
          "a mirror can be stamped with another ABI version");
    if (fd >= 0) {
        close(fd);
    }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("FLATPAK_ID", kServed, 1);
        vocem::enter_flatpak_bridge();
        vocem::StateReader reader;
        vocem::Snapshot snapshot;
        const bool opened = reader.open();
        _exit(opened && !reader.read(snapshot) ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a mirror from another ABI is refused, not guessed at");

    // A sandbox that stops asking stops being served.
    unlink((runtime + "/app/" + std::string(kServed) + "/vocem/request").c_str());
    bridge.rescan();
    check(bridge.served() == 0, "removing the request takes the sandbox off the list");

    // A daemon that stops takes the mirror's name with it. Without that, a
    // Flatpak game goes on drawing the last channel that was ever published --
    // the file stays where it is and every check the reader makes keeps
    // passing -- where a host game's overlay disappears the moment
    // unlink_segment() runs. Held with a reader that is already attached,
    // because that is the one the defect would have kept alive: it is
    // still_current() that has to notice, not the next open.
    write_file(std::string(runtime + "/app/" + kServed + "/vocem/request"),
               "pid=1\ndrawing=0\n", 18);
    bridge.rescan();
    bridge.publish(*writer.state());
    check(bridge.served() == 1, "the sandbox asks again and is served again");

    // The record of an application that cannot write its own where anybody can
    // read it. Inside a sandbox $XDG_CACHE_HOME is the sandbox's, so the record
    // was written, was right, and was invisible to the window that exists to show
    // it -- listed as a limit of the design for as long as there was no way
    // across. It comes over the bridge now, in the same file as the request, and
    // the daemon writes it here.
    // Written by the same record_application() the injected code calls, in a
    // child that is the game: neither half checked against itself, like the rest
    // of this file. The child's own $XDG_CACHE_HOME is the sandbox's, which is
    // the whole difficulty -- what it writes there is what nobody could read.
    check(vocem::known_applications().empty(), "nothing is written down before a record arrives");
    const std::string sandbox_cache = root + "/sandbox-cache";
    fflush(stdout);
    const pid_t recorder = fork();
    if (recorder == 0) {
        setenv("FLATPAK_ID", kServed, 1);
        setenv("XDG_CACHE_HOME", sandbox_cache.c_str(), 1);
        vocem::enter_flatpak_bridge();
        vocem::record_application("vulkan");
        fflush(stdout);
        _exit(0);
    }
    int recorder_status = 0;
    waitpid(recorder, &recorder_status, 0);
    check(vocem::known_applications().empty(),
          "what the sandbox wrote for itself stayed inside it, which is the difficulty");
    bridge.rescan();
    const std::vector<vocem::Application> crossed = vocem::known_applications();
    check(crossed.size() == 1, "a record from inside the sandbox reaches the host's list");
    if (crossed.size() == 1) {
        check(crossed[0].key == vocem::process_name(),
              "under the name the overlay in there was known by");
        check(crossed[0].api == "vulkan", "with what it drew with");
        check(!crossed[0].reason.empty(),
              "and the evidence, which is the whole point of the field");
        check(crossed[0].desktop == kServed,
              "the entry is the daemon's own knowledge -- the sandbox being served -- "
              "so the row can find its icon");
        check(crossed[0].seen > 0, "and it is stamped when it was written");
    }

    // Everything in that file was written by code inside somebody's game, and
    // this process is not sandboxed: what it does with those values is create a
    // file named after one of them. A name no kernel could have given, an api
    // that is not one of the two, and a value with a newline folded into it are
    // each dropped whole rather than written down in part.
    const auto refused = [&](const std::string& body, const char* what) {
        write_file(std::string(runtime + "/app/" + kServed + "/vocem/request"), body.data(),
                   body.size());
        bridge.rescan();
        const std::vector<vocem::Application> after = vocem::known_applications();
        check(after.size() == 1 && after[0].key == vocem::process_name(), what);
    };
    refused("pid=1\ndrawing=1\nname=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\napi=vulkan\ngame=1\n",
            "a name longer than the kernel's fifteen characters is not a record");
    refused("pid=1\ndrawing=1\nname=other\napi=nonsense\ngame=1\n",
            "nor is an api that is neither opengl nor vulkan");
    refused("pid=1\ndrawing=1\nname=../../escape\napi=vulkan\ngame=1\n",
            "nor a name that is trying to be a path");
    refused("pid=1\ndrawing=1\nname=other\napi=vulkan\ngame=1\nwhy=fine\nexe=\x01\x02\n",
            "nor one whose executable is not text");

    int ready[2];
    int go[2];
    if (pipe(ready) != 0 || pipe(go) != 0) {
        printf("FAIL could not make the pipes\n");
        return 1;
    }
    fflush(stdout);
    const pid_t attached = fork();
    if (attached == 0) {
        close(ready[0]);
        close(go[1]);
        setenv("FLATPAK_ID", kServed, 1);
        vocem::enter_flatpak_bridge();
        vocem::StateReader reader;
        const bool opened = reader.open();
        const bool current_before = reader.still_current();
        char token = 1;
        (void)!write(ready[1], &token, 1);
        (void)!read(go[0], &token, 1);  // the daemon has stopped by now
        const bool current_after = reader.still_current();
        _exit(opened && current_before && !current_after ? 0 : 1);
    }
    close(ready[1]);
    close(go[0]);
    char token = 0;
    (void)!read(ready[0], &token, 1);
    bridge.stop();
    struct stat left {};
    check(stat(state_file.c_str(), &left) != 0,
          "stopping takes the mirror's name away, as unlink_segment does");
    (void)!write(go[1], &token, 1);
    status = 0;
    waitpid(attached, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "and a game already attached notices its mirror is history");

    writer.close();
    vocem::StateWriter::unlink_segment();
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
