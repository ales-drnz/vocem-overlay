// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The one place that knows a Flatpak sandbox is in the way, and the way through.
//
// A game that is itself a Flatpak has a private /dev/shm (no daemon segment) and
// its own XDG directories under ~/.var/app (no config.ini, no avatar cache). One
// directory crosses with no permission asked or removable:
// $XDG_RUNTIME_DIR/app/<application id>, bind-mounted host<->sandbox at the same
// path. Both sides map the same pages, so the shared state's layout and
// kAbiVersion cross unchanged. The seqlock's counter does not: the daemon writes
// the mirror rather than mapping it (a truncated mapping would SIGBUS the process
// holding the Discord connection), so the sequence a sandbox reads is the daemon's
// own count, spelled in four pwrite()s (flatpak_bridge.cpp); two daemons would lie.
//
// It is a bridge, not a broadcast: the overlay creates `vocem/request` in its own
// directory and the daemon adopts the directories that asked. Asking hands over
// nothing by itself, since anything in the sandbox can write `drawing=1`. The
// channel, the faces and the words go only to an id whose exported desktop entry
// says Game or that `flatpak_apps` lists, only while a process of the user's runs
// in a sandbox whose /.flatpak-info names that id, and only while its overlay says
// it is drawing. The directory's name alone proves nothing: a sandbox with the
// xdg-run/app grant can create one under any name. Everyone else that asks gets
// config.ini and a cleared state.
//
// The daemon runs as the user and the far side controls every name under that
// directory, so everything the daemon opens there is opened O_NOFOLLOW.

#ifndef VOCEM_FLATPAK_H
#define VOCEM_FLATPAK_H

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vocem {

// The names under the bridge directory. One spelling each, here, because the
// daemon writes them and code inside somebody's game reads them.
inline constexpr const char* kBridgeDirName = "vocem";
inline constexpr const char* kBridgeStateName = "state";
// The words of one message, present only while its toast is on screen
// (vocem/note.h).
inline constexpr const char* kBridgeNoteName = "note";
inline constexpr const char* kBridgeConfigName = "config.ini";
inline constexpr const char* kBridgeAvatarsName = "avatars";
inline constexpr const char* kBridgeRequestName = "request";
// The colour emoji bank. Inside a sandbox the host's /usr is not mounted, so the
// compiled-in path finds nothing. It is ~16 MB and never changes, so it is copied
// once per sandbox (again only when the host's copy changed), and only into one
// that is given the voice channel.
inline constexpr const char* kBridgeEmojiBankName = "emoji_bank.rgba";
// The bank's sequence table (vocem/emoji_bank.h). The reader looks for it beside
// the bank under this exact name, so the bridge copies it BEFORE the bank: the
// bank's arrival is what the reader waits on, and a later table is never read.
inline constexpr const char* kBridgeEmojiSequencesName = "emoji_sequences.bin";

namespace detail {
// Room for the id parsed out of /.flatpak-info. Namespace scope and
// zero-initialised: a function-local static would need a guard variable, and
// this header is included by code that runs inside other people's processes.
inline char flatpak_id_storage[256] = {};
inline bool bridge_enabled = false;
// What was last written into `request`, so that saying the same thing again
// costs nothing. -1 until the first answer.
inline int bridge_drawing_written = -1;
// The record of this application as request lines (`name=`, `exe=`, `api=`,
// `game=`, `why=`), in the request itself so the daemon bounds one file, not two.
// Empty until the overlay writes its record (vocem/apps.h).
inline char bridge_record[1024] = {};

// An id, if it fits whole: half an id names another application's directory,
// and a Flatpak id is at most 255 characters, so anything longer is not one.
inline bool remember_flatpak_id(const char* value) {
    const size_t length = value ? std::strlen(value) : 0;
    if (length == 0 || length >= sizeof(flatpak_id_storage)) {
        return false;
    }
    std::memcpy(flatpak_id_storage, value, length + 1);
    return true;
}
}  // namespace detail

// Which application's sandbox this process is in, or nullptr on the host.
//
// /.flatpak-info (`name=` under `[Application]`) is asked first and FLATPAK_ID
// only when it is absent: nothing inside the sandbox can remove the file or make
// one appear on the host, while the variable is whatever somebody exported.
// One `fopen` per process; the answer is remembered.
inline const char* flatpak_app_id() {
    if (detail::flatpak_id_storage[0] != '\0') {
        return detail::flatpak_id_storage;
    }
    std::FILE* info = std::fopen("/.flatpak-info", "r");
    if (!info) {
        if (const char* id = std::getenv("FLATPAK_ID"); id && *id) {
            if (detail::remember_flatpak_id(id)) {
                return detail::flatpak_id_storage;
            }
        }
        return nullptr;
    }
    char line[512];
    bool in_application = false;
    while (std::fgets(line, sizeof(line), info)) {
        if (line[0] == '[') {
            in_application = std::strncmp(line, "[Application]", 13) == 0;
            continue;
        }
        if (!in_application || std::strncmp(line, "name=", 5) != 0) {
            continue;
        }
        char* value = line + 5;
        value[std::strcspn(value, "\r\n")] = '\0';
        detail::remember_flatpak_id(value);
        break;
    }
    std::fclose(info);
    return detail::flatpak_id_storage[0] != '\0' ? detail::flatpak_id_storage : nullptr;
}

// $XDG_RUNTIME_DIR/app/<id>/vocem for this process's own sandbox. False when
// there is no sandbox, or no runtime directory to put it in.
inline bool bridge_root(char* out, size_t capacity) {
    const char* id = flatpak_app_id();
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!id || !runtime || !*runtime) {
        return false;
    }
    const int written =
        std::snprintf(out, capacity, "%s/app/%s/%s", runtime, id, kBridgeDirName);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

// A named file inside it.
inline bool bridge_path(char* out, size_t capacity, const char* leaf) {
    char root[512];
    if (!bridge_root(root, sizeof(root))) {
        return false;
    }
    const int written = std::snprintf(out, capacity, "%s/%s", root, leaf);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

namespace detail {

// The whole of `request`, rewritten by this one writer: it carries `drawing`
// and the application's record, which arrive at different moments, and
// rewriting it for one must not drop the other.
inline bool write_bridge_request(int drawing) {
    char path[512];
    if (!bridge_path(path, sizeof(path), kBridgeRequestName)) {
        return false;
    }
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    char body[1280];
    const int length = std::snprintf(body, sizeof(body), "pid=%d\ndrawing=%d\n%s",
                                     static_cast<int>(::getpid()), drawing, bridge_record);
    if (length > 0) {
        (void)!::write(fd, body, static_cast<size_t>(length));
    }
    ::close(fd);
    bridge_drawing_written = drawing;
    return true;
}

}  // namespace detail

// Whether the state, config.ini and the avatar cache are taken from the bridge
// rather than the host's locations. Only the injected code turns it on: the
// daemon, the CLI and the settings window read their own files even sandboxed.
inline bool bridge_in_use() { return detail::bridge_enabled; }

// Called once by the overlay inside a game, off any hot path. Creates the
// directory and the request that tells the daemon this sandbox wants to be
// served, and switches the path lookups over. Returns the directory, or nullptr
// when this process is not in a sandbox and nothing had to change.
inline const char* enter_flatpak_bridge() {
    char root[512];
    if (!bridge_root(root, sizeof(root))) {
        return nullptr;
    }
    // The parent is Flatpak's and already there. mkdir failing with EEXIST is
    // success, so its result is not the answer -- the open below is.
    ::mkdir(root, 0700);

    char request[512];
    if (!bridge_path(request, sizeof(request), kBridgeRequestName)) {
        return nullptr;
    }
    // The ABI is deliberately not stated here: the mirrored segment carries it.
    // `drawing` starts at 0 because the settings that decide it arrive across
    // this very bridge.
    if (!detail::write_bridge_request(0)) {
        return nullptr;
    }

    detail::bridge_enabled = true;
    return detail::flatpak_id_storage;
}

// Whether the overlay in this process is actually drawing, told to the daemon.
//
// Told 0, the daemon serves the sandbox its settings and nothing else; told 1,
// it serves the channel too if the host consents to the id (file top). The
// per-application switch must stop the sending as well as the drawing. One
// `open` and one `write`, only when the answer changed.
inline void flatpak_bridge_drawing(bool drawing) {
    if (!detail::bridge_enabled || detail::bridge_drawing_written == (drawing ? 1 : 0)) {
        return;
    }
    detail::write_bridge_request(drawing ? 1 : 0);
}

// What this application is, told to the daemon so it can write the record on
// the host's side: inside a sandbox $XDG_CACHE_HOME/vocem/apps is the sandbox's
// own and the window cannot read it. Once per process, off any hot path. The
// daemon bounds and refuses what it finds here, since the file is the game's.
inline void flatpak_bridge_record(const char* name, const char* executable, const char* api,
                                  bool game, const char* why) {
    if (!detail::bridge_enabled || !name || !*name) {
        return;
    }
    // A newline in a process name or path would forge a line of this file, so
    // each value is flattened to one line before it is written.
    const auto one_line = [](char* out, size_t capacity, const char* value) {
        size_t at = 0;
        for (; value && value[at] && at + 1 < capacity; ++at) {
            out[at] = (value[at] == '\n' || value[at] == '\r') ? ' ' : value[at];
        }
        out[at] = '\0';
        return out;
    };
    char safe_name[64];
    char safe_executable[512];
    char safe_why[256];
    std::snprintf(detail::bridge_record, sizeof(detail::bridge_record),
                  "name=%s\nexe=%s\napi=%s\ngame=%d\nwhy=%s\n",
                  one_line(safe_name, sizeof(safe_name), name),
                  one_line(safe_executable, sizeof(safe_executable), executable),
                  api && (std::strcmp(api, "opengl") == 0 || std::strcmp(api, "vulkan") == 0)
                      ? api
                      : "",
                  game ? 1 : 0, one_line(safe_why, sizeof(safe_why), why));
    detail::write_bridge_request(detail::bridge_drawing_written < 0
                                     ? 0
                                     : detail::bridge_drawing_written);
}

}  // namespace vocem

#endif  // VOCEM_FLATPAK_H
