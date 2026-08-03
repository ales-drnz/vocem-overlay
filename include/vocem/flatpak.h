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
// are the same pages, and the shared state's seqlock, its fixed layout and its
// kAbiVersion cross the boundary unchanged -- this is a second *name* for the
// segment, not a second transport with a contract of its own.
//
// It is a bridge and not a broadcast. The daemon does not write into every
// sandbox on the machine: the overlay inside a game creates `vocem/request` in
// its own directory first, and the daemon serves the directories that asked.
// That keeps the user's voice state out of applications that never load the
// overlay, and it is what stops a Flatpak *settings window* -- which has an
// application id like any other -- from reading a mirror meant for a game.
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
inline constexpr const char* kBridgeConfigName = "config.ini";
inline constexpr const char* kBridgeAvatarsName = "avatars";
inline constexpr const char* kBridgeRequestName = "request";

namespace detail {
// Room for the id parsed out of /.flatpak-info. Namespace scope and
// zero-initialised: a function-local static would need a guard variable, and
// this header is included by code that runs inside other people's processes.
inline char flatpak_id_storage[256] = {};
inline bool bridge_enabled = false;
// What was last written into `request`, so that saying the same thing again
// costs nothing. -1 until the first answer.
inline int bridge_drawing_written = -1;

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
    const int fd = ::open(request, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return nullptr;
    }
    // What is in it is for the daemon and for a person reading it. The ABI is
    // deliberately not stated: it lives in the mirrored segment, where the
    // reader compares it, and a second copy of a version number is a second
    // thing to forget to move. `drawing` starts at 0, because at this point
    // nothing has read the settings yet -- the settings are on the other side of
    // this very bridge.
    char body[128];
    const int length = std::snprintf(body, sizeof(body), "pid=%d\ndrawing=0\n",
                                     static_cast<int>(::getpid()));
    if (length > 0) {
        (void)!::write(fd, body, static_cast<size_t>(length));
    }
    ::close(fd);
    detail::bridge_drawing_written = 0;

    detail::bridge_enabled = true;
    return detail::flatpak_id_storage;
}

// Whether the overlay in this process is actually drawing, told to the daemon.
//
// The bridge has to be entered before the decision can be made at all, because
// the settings the decision reads only arrive across it -- so a sandbox is
// adopted first and asked afterwards. This is the afterwards. A daemon that is
// told 0 serves that sandbox its settings and nothing else: no channel, no
// names, no faces. It matters because the user's per-application switch is what
// says whether the overlay belongs in a given game, and a switch that stops the
// drawing but not the sending would not be the switch it looks like.
//
// One `open` and one `write`, and only when the answer changed -- so nothing
// here happens per frame.
inline void flatpak_bridge_drawing(bool drawing) {
    if (!detail::bridge_enabled || detail::bridge_drawing_written == (drawing ? 1 : 0)) {
        return;
    }
    char request[512];
    if (!bridge_path(request, sizeof(request), kBridgeRequestName)) {
        return;
    }
    const int fd = ::open(request, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    char body[128];
    const int length = std::snprintf(body, sizeof(body), "pid=%d\ndrawing=%d\n",
                                     static_cast<int>(::getpid()), drawing ? 1 : 0);
    if (length > 0) {
        (void)!::write(fd, body, static_cast<size_t>(length));
    }
    ::close(fd);
    detail::bridge_drawing_written = drawing ? 1 : 0;
}

}  // namespace vocem

#endif  // VOCEM_FLATPAK_H
