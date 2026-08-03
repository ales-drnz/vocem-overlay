// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "flatpak_bridge.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "vocem/avatar_rgba.h"
#include "vocem/config.h"
#include "vocem/flatpak.h"
#include "vocem/shm.h"

namespace vocem {
namespace {

// $XDG_RUNTIME_DIR/app, where Flatpak keeps one directory per running
// application and bind-mounts each into the application it belongs to.
std::string applications_directory() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        return {};
    }
    return std::string(runtime) + "/app";
}

const char* leaf_of(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// A regular file, opened without following a link and without ever blocking,
// relative to a directory the sandbox owns. Every open this file makes below an
// application's directory goes through here: the application on the other side
// can replace any of these names with something that is not a file, and this
// process runs as the user with the user's whole home reachable.
//
// `O_NONBLOCK` is the load-bearing flag and it was not here at first. The
// `S_ISREG` check below is useless without it, because it is on the far side of
// the `open` -- opening a FIFO `O_RDONLY` waits for a writer that never comes,
// and one `mkfifo` inside any sandbox hung `vocemd` in `rescan()` before it had
// even reached Discord, through a wait `SIGTERM` does not interrupt. With the
// flag the open returns, the check runs, and the flag is taken off again so
// nothing downstream inherits non-blocking semantics it did not ask for. On a
// regular file `O_NONBLOCK` means nothing, which is the point.
int open_regular(int directory, const char* name, int flags, mode_t mode = 0600) {
    const int fd =
        ::openat(directory, name, flags | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        return -1;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }
    const int current = ::fcntl(fd, F_GETFL);
    if (current >= 0) {
        ::fcntl(fd, F_SETFL, current & ~O_NONBLOCK);
    }
    return fd;
}

// A whole pwrite, or false. Short writes are legal on a regular file and would
// leave the mirror holding half a struct with a sequence that says it is whole.
bool write_at(int fd, const void* data, size_t length, off_t offset) {
    const char* bytes = static_cast<const char*>(data);
    while (length > 0) {
        const ssize_t written = ::pwrite(fd, bytes, length, offset);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        offset += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

// Whether that sandbox is asking to be served, and whether its overlay is
// actually drawing. The file is written by code inside a game, so it is read the
// way anything from over there is read: bounded, and a line that is not there is
// the safe answer rather than an error.
bool read_request(int directory, bool& drawing) {
    const int fd = open_regular(directory, kBridgeRequestName, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char body[256];
    const ssize_t got = ::read(fd, body, sizeof(body) - 1);
    ::close(fd);
    drawing = false;
    if (got > 0) {
        body[got] = '\0';
        if (const char* found = std::strstr(body, "drawing=")) {
            drawing = found[8] == '1';
        }
    }
    return true;
}

bool copy_into(int directory, const char* source_path, const char* name) {
    std::FILE* source = std::fopen(source_path, "rb");
    if (!source) {
        return false;
    }
    // Temporary-and-rename, like every other file this project publishes: a game
    // reading on the other side must never meet a half-written one.
    std::string temporary = std::string(name) + ".part";
    const int fd = open_regular(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        std::fclose(source);
        return false;
    }
    char buffer[8192];
    bool complete = true;
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (::write(fd, buffer, got) != static_cast<ssize_t>(got)) {
            complete = false;
            break;
        }
    }
    complete = complete && std::ferror(source) == 0;
    ::close(fd);
    std::fclose(source);
    if (!complete || ::renameat(directory, temporary.c_str(), directory, name) != 0) {
        ::unlinkat(directory, temporary.c_str(), 0);
        return false;
    }
    return true;
}

}  // namespace

void FlatpakBridge::stop() {
    for (Mirror& mirror : mirrors_) {
        if (mirror.directory >= 0) {
            ::unlinkat(mirror.directory, kBridgeStateName, 0);
        }
        close(mirror);
    }
    if (!mirrors_.empty()) {
        LOG("stopped serving %zu Flatpak sandbox%s", mirrors_.size(),
            mirrors_.size() == 1 ? "" : "es");
    }
    mirrors_.clear();
}

FlatpakBridge::~FlatpakBridge() {
    for (Mirror& mirror : mirrors_) {
        close(mirror);
    }
    mirrors_.clear();
    if (applications_ >= 0) {
        ::close(applications_);
        applications_ = -1;
    }
}

bool FlatpakBridge::start() {
    const std::string directory = applications_directory();
    if (directory.empty()) {
        LOG("no XDG_RUNTIME_DIR: Flatpak games cannot be reached from here");
        return false;
    }
    applications_ = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    started_ = applications_ >= 0;
    if (!started_) {
        // Not an error: a machine that has never run a Flatpak has no such
        // directory. It may appear later, and rescan() keeps looking.
        DBG("%s is not there yet; no Flatpak game has run in this session",
            directory.c_str());
    }
    return started_;
}

bool FlatpakBridge::state_is_ours(const Mirror& mirror) const {
    // Two questions, because either one alone can be answered wrongly. The
    // object we hold open must still have a name at all -- `st_nlink` drops to
    // zero the moment the sandbox unlinks it -- and the name must still mean
    // this object. Comparing only the inode numbers is not enough on a tmpfs,
    // which hands the number of a file just deleted straight back to the next
    // one created; comparing only the link count would miss a rename over the
    // top, which does not unlink anything of ours.
    struct stat ours {};
    if (::fstat(mirror.state_file, &ours) != 0 || ours.st_nlink == 0) {
        return false;
    }
    struct stat named {};
    if (::fstatat(mirror.directory, kBridgeStateName, &named, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    return named.st_dev == ours.st_dev && named.st_ino == ours.st_ino;
}

void FlatpakBridge::close(Mirror& mirror) {
    if (mirror.state_file >= 0) {
        ::close(mirror.state_file);
        mirror.state_file = -1;
    }
    if (mirror.directory >= 0) {
        ::close(mirror.directory);
        mirror.directory = -1;
    }
}

bool FlatpakBridge::adopt(const char* id) {
    // Walked one component at a time from the directory descriptor this class
    // already holds, `O_NOFOLLOW` on every step. The first version built one
    // absolute path and put `O_NOFOLLOW` on the last component only, which left
    // the application's own directory able to be a symbolic link: with write
    // access to $XDG_RUNTIME_DIR/app -- which `--filesystem=xdg-run/app` grants
    // -- the state and the settings copy went through it. Nothing above
    // $XDG_RUNTIME_DIR/app is reachable from a sandbox, so the walk starts there.
    const int application =
        ::openat(applications_, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (application < 0) {
        return false;
    }
    const int directory =
        ::openat(application, kBridgeDirName, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(application);
    if (directory < 0) {
        return false;
    }
    bool drawing = false;
    if (!read_request(directory, drawing)) {
        // The directory exists but nothing in that sandbox is asking to be
        // served -- or `request` is not a regular file, which is somebody being
        // clever rather than an overlay asking.
        if (errno == EINVAL || errno == ELOOP) {
            LOG("refusing the Flatpak bridge for %s: its %s is not a regular file", id,
                kBridgeRequestName);
        }
        ::close(directory);
        return false;
    }

    Mirror mirror;
    mirror.id = id;
    mirror.directory = directory;
    mirror.drawing = drawing;
    mirror.state_file = open_regular(directory, kBridgeStateName, O_RDWR | O_CREAT);
    if (mirror.state_file < 0) {
        LOG("refusing the Flatpak bridge for %s: %s could not be opened as a regular file (%s)",
            id, kBridgeStateName, std::strerror(errno));
        close(mirror);
        return false;
    }
    if (::ftruncate(mirror.state_file, sizeof(SharedState)) != 0) {
        LOG("refusing the Flatpak bridge for %s: %s cannot be sized (%s)", id, kBridgeStateName,
            std::strerror(errno));
        close(mirror);
        return false;
    }
    LOG("serving the overlay inside the Flatpak sandbox of %s", id);
    mirrors_.push_back(std::move(mirror));
    return true;
}

void FlatpakBridge::rescan() {
    if (applications_ < 0 && !start()) {
        return;
    }
    // Drop the sandboxes that stopped asking, the ones whose directory went away
    // with the application, and the ones whose state file is no longer the one
    // this daemon opened -- a sandbox that replaces it leaves the daemon writing
    // an orphaned inode while the name it left behind stays empty, which reads
    // from inside exactly like a daemon that is not running.
    for (size_t i = mirrors_.size(); i > 0; --i) {
        Mirror& mirror = mirrors_[i - 1];
        bool drawing = false;
        const char* gone = nullptr;
        if (!read_request(mirror.directory, drawing)) {
            gone = "it stopped asking";
        } else if (!state_is_ours(mirror)) {
            gone = "its state file was replaced";
        }
        if (!gone) {
            mirror.drawing = drawing;
            continue;
        }
        LOG("no longer serving the Flatpak sandbox of %s: %s", mirror.id.c_str(), gone);
        close(mirror);
        mirrors_.erase(mirrors_.begin() + static_cast<long>(i - 1));
    }

    const std::string directory = applications_directory();
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return;
    }
    while (const dirent* entry = ::readdir(handle)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        bool known = false;
        for (const Mirror& mirror : mirrors_) {
            if (mirror.id == entry->d_name) {
                known = true;
                break;
            }
        }
        if (!known) {
            adopt(entry->d_name);
        }
    }
    ::closedir(handle);
}

void FlatpakBridge::publish(const SharedState& state) {
    for (Mirror& mirror : mirrors_) {
        if (mirror.state_file < 0) {
            continue;
        }
        // Written, not mapped. The daemon mapping a file the sandbox can shrink
        // would hand every Flatpak application on the machine a way to kill it:
        // touching a mapping past a truncated file's end raises SIGBUS, and this
        // process holds the only Discord connection there is. pwrite into a
        // short file simply lengthens it again.
        //
        // The seqlock is the same one the segment has, spelled in four writes:
        // odd, the two runs of the struct that the sequence field divides, then
        // even. A reader that samples in between retries, exactly as it does on
        // the canonical segment.
        // A sandbox whose overlay is not drawing is served a cleared state, not
        // the channel: the user's per-application switch is what says whether
        // the overlay belongs in that game, and it must mean something on this
        // side of the wall too. It is still published rather than left alone, so
        // that switching a game off empties its panel instead of freezing it.
        SharedState empty{};
        empty.abi_version = kAbiVersion;
        empty.status = static_cast<uint32_t>(DaemonStatus::WaitingForDiscord);
        const SharedState& payload = mirror.drawing ? state : empty;

        const uint32_t odd = mirror.sequence + 1;
        const uint32_t even = mirror.sequence + 2;
        constexpr off_t kSequenceAt = static_cast<off_t>(offsetof(SharedState, sequence));
        constexpr size_t kSequenceBytes = sizeof(uint32_t);
        constexpr off_t kTailAt = kSequenceAt + static_cast<off_t>(kSequenceBytes);

        const char* bytes = reinterpret_cast<const char*>(&payload);
        if (!write_at(mirror.state_file, &odd, kSequenceBytes, kSequenceAt) ||
            !write_at(mirror.state_file, bytes, static_cast<size_t>(kSequenceAt), 0) ||
            !write_at(mirror.state_file, bytes + kTailAt,
                      sizeof(SharedState) - static_cast<size_t>(kTailAt), kTailAt) ||
            !write_at(mirror.state_file, &even, kSequenceBytes, kSequenceAt)) {
            LOG("could not publish into the Flatpak sandbox of %s (%s)", mirror.id.c_str(),
                std::strerror(errno));
            continue;
        }
        mirror.sequence = even;
    }
}

void FlatpakBridge::mirror_config(Mirror& mirror) {
    const long long mtime = Config::mtime();
    if (mtime == mirror.config_mtime) {
        return;
    }
    const std::string source = Config::path();
    if (copy_into(mirror.directory, source.c_str(), kBridgeConfigName)) {
        mirror.config_mtime = mtime;
        DBG("copied %s into the Flatpak sandbox of %s", kBridgeConfigName, mirror.id.c_str());
    } else if (mtime != 0) {
        LOG("could not copy %s into the Flatpak sandbox of %s (%s)", kBridgeConfigName,
            mirror.id.c_str(), std::strerror(errno));
        mirror.config_mtime = mtime;  // say it once per change, not once per tick
    }
}

void FlatpakBridge::mirror_avatars(Mirror& mirror, const SharedState& state) {
    if (::mkdirat(mirror.directory, kBridgeAvatarsName, 0700) != 0 && errno != EEXIST) {
        return;
    }
    const int avatars = ::openat(mirror.directory, kBridgeAvatarsName,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (avatars < 0) {
        return;
    }

    // Only what this state names: at most the people in the channel plus the
    // author of the last message. The picture for a given id and hash never
    // changes, so a file that is already there at the format's one size is
    // already right.
    auto ensure = [&](uint64_t user_id, const char* hash) {
        char source[832];
        avatar_rgba_path(source, sizeof(source), user_id, hash);
        const char* name = leaf_of(source);
        const int existing = open_regular(avatars, name, O_RDONLY);
        if (existing >= 0) {
            struct stat info {};
            const bool complete = ::fstat(existing, &info) == 0 &&
                                  info.st_size == static_cast<off_t>(kAvatarRgbaBytes);
            ::close(existing);
            if (complete) {
                return;
            }
        }
        copy_into(avatars, source, name);
    };

    const uint32_t count = state.user_count < kMaxUsers ? state.user_count : kMaxUsers;
    for (uint32_t i = 0; i < count; ++i) {
        ensure(state.users[i].id, state.users[i].avatar_hash);
    }
    if (state.notification.serial != 0) {
        ensure(state.notification.user_id, state.notification.avatar_hash);
    }
    ::close(avatars);
}

void FlatpakBridge::refresh_files(const SharedState& state) {
    for (Mirror& mirror : mirrors_) {
        // The settings always: they are what the overlay reads to decide whether
        // it draws in this game at all, so withholding them would make the
        // decision unanswerable. The faces only where it does draw.
        mirror_config(mirror);
        if (mirror.drawing) {
            mirror_avatars(mirror, state);
        }
    }
}

}  // namespace vocem
