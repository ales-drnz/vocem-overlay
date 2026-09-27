// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The one place that knows a Flatpak sandbox is in the way, and where the way
// through it is.
//
// A game that is itself a Flatpak sees none of what this overlay stands on: its
// /dev/shm is a private tmpfs, so the daemon's POSIX segment is not there; its
// XDG directories are the application's own under ~/.var/app, so neither
// config.ini nor the avatar cache is there either. Measured on Flatpak 1.18
// inside org.vinegarhq.Sober, which is restricted (no filesystem=host).
//
// One directory does cross, with no permission asked for and none that could be
// taken away: $XDG_RUNTIME_DIR/app/<application id>. Flatpak bind-mounts the
// host's copy of it into the sandbox at the same path. Measured: a file created
// there by the host and a file created there by the sandbox each appear on the
// other side, and a MAP_SHARED mapping of the same file reports the same st_dev
// and st_ino on both sides and sees the other side's writes live. So the pages
// are the same pages, and the shared state's fixed layout and its kAbiVersion
// cross the boundary unchanged. What does NOT cross unchanged is the seqlock's
// counter: the daemon writes the mirror rather than mapping it (a truncated
// mapping is SIGBUS in the one process holding the Discord connection), so the
// sequence a sandbox reads is the daemon's own count for that mirror, spelled
// in four pwrite()s (flatpak_bridge.cpp), and a second daemon would lie where
// the canonical segment's shared atomic would not. Entry 86 corrected this
// paragraph's earlier claim; the paragraph took until 0.1.8 to follow.
//
// It is a bridge and not a broadcast. The daemon does not write into every
// sandbox on the machine: the overlay inside a game creates `vocem/request` in
// its own directory first, and the daemon adopts the directories that asked.
// Asking is not what hands over the voice channel, though. `request` is a file
// inside the sandbox, and anything running there can write `drawing=1` into it
// -- through 0.1.10 that alone was enough, for any Flatpak at all. The daemon
// decides on the host, by application id: the channel, the faces and the words
// go only to an id whose exported desktop entry says Game or that the user
// listed in `flatpak_apps`, only while a process of the user's runs in a
// sandbox whose /.flatpak-info names that id, and only while its overlay says
// it is drawing. The id is the directory's name, and the name alone proves
// nothing: a sandbox holding the xdg-run/app grant can make a directory there
// under any name (entries 134, 164). Everyone else that asks is given
// config.ini and a cleared state (flatpak_bridge.cpp).
//
// What crosses is therefore under the sandbox's control, and the daemon is not:
// it runs as the user with the user's whole home reachable. Everything it opens
// under that directory is opened O_NOFOLLOW, because the application on the far
// side can replace any of those names with a symbolic link.

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
// The words of one message, while its toast is on screen and not a moment
// longer. A file of its own for the same reason it is a segment of its own on
// the host (vocem/note.h): it exists only while there is something to draw.
inline constexpr const char* kBridgeNoteName = "note";
inline constexpr const char* kBridgeConfigName = "config.ini";
inline constexpr const char* kBridgeAvatarsName = "avatars";
inline constexpr const char* kBridgeRequestName = "request";
// The colour emoji bank. Inside a sandbox the host's /usr is not mounted, so
// the compiled-in path names the runtime's own /usr and finds nothing -- the
// same shape as the note being the fourth thing to carry and nothing carrying
// it. Sixteen megabytes and it never changes, so it is copied once per sandbox
// (and again only when the host's copy changed) and only into one that is
// given the voice channel.
inline constexpr const char* kBridgeEmojiBankName = "emoji_bank.rgba";
// The sequence table that belongs to that bank (vocem/emoji_bank.h): which
// codepoint sequences its keys from U+F0000 up stand for. The reader looks
// for it BESIDE the bank under this exact name, wherever the bank was found,
// so the bridge and the package both put it there, and the bridge copies it
// before the bank: the bank's arrival is what the reader waits on, and a table
// arriving after it would never be read.
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
// The record of this application, in the lines the daemon will find: `name=`,
// `exe=`, `api=`, `game=`, `why=`. It goes in the same file as the request
// because the request is rewritten whole whenever `drawing` changes, and a
// second file would be a second thing for the daemon to find, bound and refuse.
// Empty until the overlay writes its record (vocem/apps.h).
inline char bridge_record[1024] = {};

// An id, if it fits whole. Nothing is truncated into a path: half an application
// id names another application's directory, and a Flatpak id is at most 255
// characters by Flathub's own rule, so anything longer is not one.
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
// /.flatpak-info is asked first, and FLATPAK_ID only if that is not there.
// Flatpak places the file in the sandbox root and nothing running inside can
// remove it or make one appear on the host, where the environment variable is
// simply whatever somebody exported. The order was the other way round at first,
// with a comment saying the file was there for the case the environment could
// not be trusted -- which the order then did not deliver. Measured inside
// org.vinegarhq.Sober: `[Application]` then `name=org.vinegarhq.Sober` on the
// following line.
//
// One `fopen` per process, at the first frame of a GL process and at
// `vkCreateInstance` in a Vulkan one, and never again: the answer is remembered.
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

// The whole of `request`, rewritten. One writer, because the file says two
// things -- whether this sandbox is asking and what the overlay decided about
// the application -- and they arrive at different moments: rewriting it for one
// of them must not take the other away.
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

// Whether the paths this project derives -- the state, config.ini, the avatar
// cache -- should be taken from the bridge rather than from the host's own
// locations.
//
// Off unless something turns it on, and the only things that turn it on are the
// two pieces of injected code. The daemon, the CLI and the settings window run
// as themselves and must keep reading their own files even when they are
// sandboxed, which a Flatpak of the settings window is.
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
    // The parent, $XDG_RUNTIME_DIR/app/<id>, is Flatpak's and already there; only
    // the last component is ours. mkdir on something that exists is the success
    // case, so its result is not the answer -- the open below is.
    ::mkdir(root, 0700);

    char request[512];
    if (!bridge_path(request, sizeof(request), kBridgeRequestName)) {
        return nullptr;
    }
    // What is in it is for the daemon and for a person reading it. The ABI is
    // deliberately not stated: it lives in the mirrored segment, where the
    // reader compares it, and a second copy of a version number is a second
    // thing to forget to move. `drawing` starts at 0, because at this point
    // nothing has read the settings yet -- the settings are on the other side of
    // this very bridge.
    if (!detail::write_bridge_request(0)) {
        return nullptr;
    }

    detail::bridge_enabled = true;
    return detail::flatpak_id_storage;
}

// Whether the overlay in this process is actually drawing, told to the daemon.
//
// The bridge has to be entered before the decision can be made at all, because
// the settings the decision reads only arrive across it -- so a sandbox is
// adopted first and asked afterwards. This is the afterwards. A daemon that is
// told 0 serves that sandbox its settings and nothing else: no channel, no
// names, no faces. Told 1, it serves the channel only if the host consents to
// this application id as well (the paragraph above). It matters because the user's per-application switch is what
// says whether the overlay belongs in a given game, and a switch that stops the
// drawing but not the sending would not be the switch it looks like.
//
// One `open` and one `write`, and only when the answer changed -- so nothing
// here happens per frame.
inline void flatpak_bridge_drawing(bool drawing) {
    if (!detail::bridge_enabled || detail::bridge_drawing_written == (drawing ? 1 : 0)) {
        return;
    }
    detail::write_bridge_request(drawing ? 1 : 0);
}

// What this application is, told to the daemon so that it can be written down on
// the host's side.
//
// Every process writes a record of itself under $XDG_CACHE_HOME/vocem/apps, and
// inside a sandbox that directory is the sandbox's own: the record is written, is
// correct, and cannot be read by the window that exists to show it. That was
// listed as a limit of the design for as long as there was no way across. There
// is one now -- the same bridge the settings arrive by -- so the record goes over
// it as five more lines of the request, and the daemon writes the file.
//
// Once per process, off any hot path, right after the local record is written.
// The daemon bounds and refuses what it finds here: this is a file inside
// somebody's game, and the daemon is not sandboxed.
inline void flatpak_bridge_record(const char* name, const char* executable, const char* api,
                                  bool game, const char* why) {
    if (!detail::bridge_enabled || !name || !*name) {
        return;
    }
    // A process name is whatever the process called itself and a path is whatever
    // it is: a newline in either would forge a line of this file, so each value is
    // put on one line before it is written rather than after.
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
