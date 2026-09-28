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
#include <set>
#include <string>
#include <vector>

#include "flatpak_bridge_parts.h"
#include "flatpak_process.h"
#include "log.h"
#include "vocem/apps.h"
#include "vocem/avatar_rgba.h"
#include "vocem/config.h"
#include "vocem/flatpak.h"
#include "vocem/note.h"
#include "vocem/shm.h"

namespace vocem {

using bridge::looks_like_app_id;
using bridge::open_regular;
using bridge::printable_id;
using bridge::remove_faces;
using bridge::write_at;

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

}  // namespace

// What the overlay inside a sandbox says about itself.
struct FlatpakBridge::Request {
    bool drawing = false;
    // The record that sandbox cannot write where anybody can see it: its
    // $XDG_CACHE_HOME is its own. Empty name means it has not said yet -- the
    // request exists from the moment the bridge is entered, and the record is
    // written a frame later.
    std::string name;
    std::string executable;
    std::string api;
    std::string why;
    bool game = false;
};

namespace {

// One line's value, or an empty string. Lines are `key=value`, and a key that is
// not there is the safe answer rather than an error.
std::string request_field(const char* body, const char* key) {
    const size_t length = std::strlen(key);
    for (const char* at = body; at && *at;) {
        const char* end = std::strchr(at, '\n');
        const size_t line = end ? static_cast<size_t>(end - at) : std::strlen(at);
        if (line > length && std::strncmp(at, key, length) == 0 && at[length] == '=') {
            return std::string(at + length + 1, line - length - 1);
        }
        at = end ? end + 1 : nullptr;
    }
    return {};
}

}  // namespace

// Whether that sandbox is asking to be served, what its overlay is doing, and
// what it decided about the application. The file is written by code inside a
// game, so it is read the way anything from over there is read: bounded, and
// every field held to what it can legitimately be before it is believed. This
// process is not sandboxed and what it does with these values is create a file
// named after one of them.
bool FlatpakBridge::read_request(int directory, Request& request) {
    const int fd = open_regular(directory, kBridgeRequestName, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char body[1536];
    const ssize_t got = ::read(fd, body, sizeof(body) - 1);
    ::close(fd);
    request = Request{};
    if (got <= 0) {
        return true;
    }
    body[got] = '\0';
    request.drawing = request_field(body, "drawing") == "1";

    // A name is what /proc/self/comm gives, so at most fifteen characters, and
    // the api is one of two words. Anything else is not a record this project
    // wrote and is dropped whole rather than written down in part.
    const std::string name = request_field(body, "name");
    const std::string api = request_field(body, "api");
    if (name.empty() || name.size() > vocem::kCommLength || (api != "opengl" && api != "vulkan") ||
        name.find('/') != std::string::npos || name.front() == '.') {
        // A record is written under this name. `sanitised()` already makes it a
        // file name that cannot leave its directory, so this is not the wall --
        // it is the rule that a name from over there has to look like a name
        // before anything is done with it at all.
        return true;
    }
    const auto printable = [](const std::string& text, size_t limit) {
        if (text.size() > limit) {
            return false;
        }
        for (const char character : text) {
            if (static_cast<unsigned char>(character) < 0x20) {
                return false;
            }
        }
        return true;
    };
    const std::string executable = request_field(body, "exe");
    const std::string why = request_field(body, "why");
    if (!printable(name, vocem::kCommLength) || !printable(executable, 512) || !printable(why, 256)) {
        return true;
    }
    request.name = name;
    request.executable = executable;
    request.api = api;
    request.why = why;
    request.game = request_field(body, "game") == "1";
    return true;
}

// Whether this refusal is the first one about this directory name. See the
// header for why the memory exists and why it is bounded.
bool FlatpakBridge::say_refusal_once(const char* id) {
    const std::string name = id ? id : "";
    if (refusals_said_.count(name)) {
        return false;
    }
    if (refusals_said_.size() >= kMirrorCeiling) {
        if (!refusals_full_said_) {
            refusals_full_said_ = true;
            LOG("%zu directories under the runtime app directory have been refused; further "
                "refusals are not logged",
                refusals_said_.size());
        }
        return false;
    }
    refusals_said_.insert(name);
    return true;
}

void FlatpakBridge::stop() {
    for (Mirror& mirror : mirrors_) {
        if (mirror.directory >= 0) {
            ::unlinkat(mirror.directory, kBridgeStateName, 0);
            remove_faces(mirror.directory);
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
        // Once, not once per second: rescan() retries this for the life of the
        // process, and the header promises the refusal is said "once and
        // quietly".
        if (!runtime_missing_said_) {
            runtime_missing_said_ = true;
            LOG("no XDG_RUNTIME_DIR: Flatpak games cannot be reached from here");
        }
        return false;
    }
    applications_ = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (applications_ < 0) {
        // Not an error: a machine that has never run a Flatpak has no such
        // directory. It may appear later, and rescan() keeps looking.
        DBG("%s is not there yet; no Flatpak game has run in this session",
            directory.c_str());
    }
    return applications_ >= 0;
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
    // already holds, `O_NOFOLLOW` on every step, so the application's own
    // directory cannot be a symbolic link: with write access to
    // $XDG_RUNTIME_DIR/app -- which `--filesystem=xdg-run/app` grants -- the
    // state and the settings copy would go through it (entry 86). Nothing above
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
    Request request;
    if (!read_request(directory, request)) {
        // The directory exists but nothing in that sandbox is asking to be
        // served -- or `request` is not a regular file, which is somebody being
        // clever rather than an overlay asking.
        if ((errno == EINVAL || errno == ELOOP) && say_refusal_once(id)) {
            LOG("refusing the Flatpak bridge for %s: its %s is not a regular file", id,
                kBridgeRequestName);
        }
        ::close(directory);
        return false;
    }

    Mirror mirror;
    mirror.id = id;
    mirror.directory = directory;
    mirror.drawing = request.drawing;
    mirror.state_file = open_regular(directory, kBridgeStateName, O_RDWR | O_CREAT);
    // Both refusals below are asked again on every one-second rescan, like the
    // ones above them, and said once per name like them (entry 198).
    if (mirror.state_file < 0) {
        const int error = errno;
        if (say_refusal_once(id)) {
            LOG("refusing the Flatpak bridge for %s: %s could not be opened as a regular file "
                "(%s)", id, kBridgeStateName, std::strerror(error));
        }
        close(mirror);
        return false;
    }
    if (::ftruncate(mirror.state_file, sizeof(SharedState)) != 0) {
        const int error = errno;
        if (say_refusal_once(id)) {
            LOG("refusing the Flatpak bridge for %s: %s cannot be sized (%s)", id,
                kBridgeStateName, std::strerror(error));
        }
        close(mirror);
        return false;
    }
    // A `note` already in this directory was not written by us: publish_note()
    // only ever writes to mirrors it is already holding, so anything here came
    // from a daemon that is gone -- one that was killed before it could retire
    // the words. The host half of that inheritance is declined in main(); this
    // is the sandbox's copy of the same words.
    ::unlinkat(directory, kBridgeNoteName, 0);
    decide(mirror, false);
    LOG("serving the overlay inside the Flatpak sandbox of %s (voice channel: %s, %s)", id,
        mirror.consented ? "yes" : "no", mirror.consent_why.c_str());
    say_refusal(mirror);
    check_running(mirror);
    // Faces already in there came from a daemon before this one; like the
    // note above, they stay only if this one serves the channel here.
    mirror.faces_given = true;
    take_faces_back(mirror);
    write_record_for(mirror, request);
    mirrors_.push_back(std::move(mirror));
    return true;
}

// The record of an application that cannot write its own where the window can
// read it: inside a sandbox, $XDG_CACHE_HOME is the sandbox's. It is written here
// instead, from what came across the bridge, and only when it changed -- this is
// asked on every rescan tick.
//
// The entry the row's icon is looked up by is the daemon's own knowledge, not the
// sandbox's word for it: the application id of the sandbox being served is the id
// of the entry Flatpak exported on the host.
//
// One record per sandbox, under a file name made from the application id --
// `flatpak@` and the id with its dots spelled as colons -- and never under the
// name the sandbox gives, which could fill ~/.cache/vocem/apps or replace a host
// application's record (entry 247). A host record's file name is
// detail::sanitised() of a process name, which never holds '@' or ':', so the
// two cannot meet; the dots are spelled otherwise because the reader skips any
// name holding ".tmp." -- an id like `com.tmp.Game` would be a record nobody
// sees. A name that changes replaces the sandbox's one file, and is said once.
void FlatpakBridge::write_record_for(Mirror& mirror, const Request& request) {
    if (request.name.empty() || request.name == mirror.recorded) {
        return;
    }
    const bool renamed = !mirror.recorded.empty();
    mirror.recorded = request.name;
    Application application;
    application.key = request.name;
    application.executable = request.executable;
    application.api = request.api;
    application.desktop = mirror.id;
    application.looks_like_game = request.game;
    application.reason = request.why;
    std::string file_name = "flatpak@" + mirror.id;
    for (char& c : file_name) {
        c = c == '.' ? ':' : c;
    }
    // An id is at most 255 bytes and a file name too, with the prefix and the
    // writer's ".tmp.<pid>" on top: cut, since only two ids sharing their
    // first 230 bytes could then meet, and only in each other's record.
    if (file_name.size() > 230) {
        file_name.resize(230);
    }
    write_application_record(application, file_name);
    if (!renamed) {
        LOG("wrote the record of '%s' for the Flatpak sandbox of %s (%s)", request.name.c_str(),
            mirror.id.c_str(), request.why.c_str());
    } else if (!mirror.rename_said) {
        mirror.rename_said = true;
        LOG("the Flatpak sandbox of %s renamed its record to '%s'; its one record is replaced, "
            "and further renames are not logged",
            mirror.id.c_str(), request.name.c_str());
    }
}

void FlatpakBridge::rescan() {
    if (applications_ < 0 && !start()) {
        return;
    }
    running_scanned_ = false;  // this sweep asks /proc afresh, if anybody asks
    refresh_consent();
    // Drop the sandboxes that stopped asking, the ones whose directory went away
    // with the application, and the ones whose state file is no longer the one
    // this daemon opened -- a sandbox that replaces it leaves the daemon writing
    // an orphaned inode while the name it left behind stays empty, which reads
    // from inside exactly like a daemon that is not running.
    for (size_t i = mirrors_.size(); i > 0; --i) {
        Mirror& mirror = mirrors_[i - 1];
        Request request;
        const char* gone = nullptr;
        if (!read_request(mirror.directory, request)) {
            gone = "it stopped asking";
        } else if (!state_is_ours(mirror)) {
            gone = "its state file was replaced";
        }
        if (!gone) {
            mirror.drawing = request.drawing;
            say_refusal(mirror);
            check_running(mirror);
            take_faces_back(mirror);
            // The record arrives a frame after the request that adopted this
            // sandbox, so this is where it is usually seen.
            write_record_for(mirror, request);
            continue;
        }
        LOG("no longer serving the Flatpak sandbox of %s: %s", mirror.id.c_str(), gone);
        remove_faces(mirror.directory);
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
        if (known) {
            continue;
        }
        // The two bounds on who gets served, refused out loud (entry 134).
        // A directory under $XDG_RUNTIME_DIR/app can be made by any process
        // of the user's -- a sandbox with the xdg-run/app grant included --
        // and every mirror costs two descriptors, the emoji bank (about
        // 16 MB, with its sequence table beside it) and a share of every
        // publish: without a ceiling a few hundred asking directories would
        // exhaust this process's descriptors, after which the socket,
        // /proc/net/tcp and the segment itself would fail to open. A Flatpak
        // application id is reverse-DNS -- letters, digits, '.', '_' and '-',
        // at least one dot -- and a name that is not one was never made by
        // Flatpak.
        if (mirrors_.size() >= kMirrorCeiling) {
            if (!ceiling_said_) {
                ceiling_said_ = true;
                LOG("not serving the Flatpak sandbox of %s: %zu sandboxes are already served, "
                    "which is the ceiling",
                    printable_id(entry->d_name).c_str(), mirrors_.size());
            }
            continue;
        }
        if (!looks_like_app_id(entry->d_name)) {
            if (say_refusal_once(entry->d_name)) {
                LOG("not serving %s: not the shape of a Flatpak application id",
                    printable_id(entry->d_name).c_str());
            }
            continue;
        }
        if (adopt(entry->d_name)) {
            // Adopted: forget the refusals said about it, so a sandbox that
            // comes back and fails differently is heard.
            refusals_said_.erase(entry->d_name);
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
        // short file lengthens it again.
        //
        // The seqlock is the same one the segment has, spelled in four writes:
        // odd, the two runs of the struct that the sequence field divides, then
        // even. A reader that samples in between retries, exactly as it does on
        // the canonical segment.
        // The channel goes only where both halves say yes (Mirror::voice()).
        // The sandbox's `drawing` is the user's per-application switch as the
        // overlay in there read it -- which holds for an overlay that tells
        // the truth and for nothing else, because anything in that sandbox
        // can write the line. The host's consent is what holds for the rest:
        // an application id whose exported entry says Game, or one the user
        // listed in flatpak_apps, with a process of that application running
        // (check_running). Everywhere else the answer is a cleared
        // state, published rather than left alone so that switching a game
        // off empties its panel instead of freezing it.
        SharedState empty{};
        empty.abi_version = kAbiVersion;
        empty.status = static_cast<uint32_t>(DaemonStatus::WaitingForDiscord);
        const SharedState& payload = mirror.voice() ? state : empty;

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
            // Once per sandbox: this runs on every tick, and a mirror whose
            // file cannot be written fails the same way every time.
            if (!mirror.publish_refused) {
                mirror.publish_refused = true;
                LOG("could not publish into the Flatpak sandbox of %s (%s)", mirror.id.c_str(),
                    std::strerror(errno));
            }
            continue;
        }
        mirror.publish_refused = false;
        mirror.sequence = even;
    }
}

void FlatpakBridge::publish_note(uint64_t serial, const char* body) {
    for (Mirror& mirror : mirrors_) {
        if (mirror.directory < 0) {
            continue;
        }
        // Gone, or never wanted here. Unlinking is what tells a reader inside
        // the game that the words are history, exactly as it does on the host.
        if (!body || serial == 0 || !mirror.voice()) {
            ::unlinkat(mirror.directory, kBridgeNoteName, 0);
            continue;
        }
        // Written whole into a temporary and renamed into place, rather than
        // updated under a seqlock: the file is created for one message and
        // removed with it, so a reader either meets a complete one or none at
        // all, and the sequence it finds is always stable.
        NoteShared note{};
        note.abi_version = kNoteAbiVersion;
        note.serial = serial;
        note.sequence.store(2, std::memory_order_relaxed);
        copy_string(note.body, kNotificationBodyCapacity, body, std::strlen(body));

        // The same "<name>.part" rule copy_into() spells.
        const std::string temporary = std::string(kBridgeNoteName) + ".part";
        const int fd =
            open_regular(mirror.directory, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            LOG("could not write the message into the Flatpak sandbox of %s (%s)",
                mirror.id.c_str(), std::strerror(errno));
            continue;
        }
        const bool written = write_at(fd, &note, sizeof(note), 0);
        ::close(fd);
        if (!written ||
            ::renameat(mirror.directory, temporary.c_str(), mirror.directory, kBridgeNoteName) != 0) {
            ::unlinkat(mirror.directory, temporary.c_str(), 0);
        }
    }
}

}  // namespace vocem
