// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// How bytes cross into a sandbox: see flatpak_bridge_parts.h.

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
namespace bridge {

// A regular file, opened without following a link and without ever blocking,
// relative to a directory the sandbox owns. Every open this file makes below an
// application's directory goes through here: the application on the other side
// can replace any of these names with something that is not a file, and this
// process runs as the user with the user's whole home reachable.
//
// `O_NONBLOCK` is the load-bearing flag. The `S_ISREG` check below is useless
// without it, because it is on the far side of the `open`: opening a FIFO
// `O_RDONLY` waits for a writer that never comes, in a wait `SIGTERM` does not
// interrupt, so one `mkfifo` inside any sandbox would hang `rescan()`. With the
// flag the open returns, the check runs, and the flag is taken off again so
// nothing downstream inherits non-blocking semantics it did not ask for. On a
// regular file `O_NONBLOCK` means nothing, which is the point.
int open_regular(int directory, const char* name, int flags, mode_t mode) {
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

// Every face this bridge may have put in a sandbox's avatars directory: the
// names that end in `.rgba`, and their `.part` temporaries. The directory is
// the sandbox's, so it is opened O_NOFOLLOW, and unlinkat() removes a link and
// never what it points at. At most a few thousand names are looked at: the
// sandbox can fill the directory, and this runs on the daemon's main thread.
void remove_faces(int directory) {
    const int avatars =
        ::openat(directory, kBridgeAvatarsName, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (avatars < 0) {
        return;
    }
    DIR* listing = ::fdopendir(avatars);
    if (!listing) {
        ::close(avatars);
        return;
    }
    const auto ends_with = [](const std::string& name, const char* suffix) {
        const size_t length = std::strlen(suffix);
        return name.size() > length && name.compare(name.size() - length, length, suffix) == 0;
    };
    std::vector<std::string> faces;
    size_t looked = 0;
    while (const dirent* entry = ::readdir(listing)) {
        if (++looked > 4096) {
            break;
        }
        const std::string name = entry->d_name;
        if (ends_with(name, ".rgba") || ends_with(name, ".rgba.part")) {
            faces.push_back(name);
        }
    }
    for (const std::string& name : faces) {
        ::unlinkat(::dirfd(listing), name.c_str(), 0);
    }
    ::closedir(listing);
}

}  // namespace bridge

using bridge::open_regular;
using bridge::remove_faces;
using bridge::write_at;

namespace {

const char* leaf_of(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// What a copy's source is allowed to be. The copy goes into a sandbox, so its
// source is opened with the same care as anything under the sandbox's
// directory: never waiting on a FIFO, a regular file or nothing, and no
// bigger than the thing it claims to be.
enum class Source {
    // The user's config.ini. A link is followed: a dotfile manager makes
    // exactly that, and whoever can plant one at ~/.config/vocem can write
    // the settings (and read the token) directly.
    Settings,
    // A face from the avatar cache. Every file there is one the daemon wrote
    // itself, so a link is never legitimate and is refused, and anything but
    // the format's one size is not a face.
    Avatar,
    // The emoji bank and its table, from the installed path or
    // $VOCEM_EMOJI_BANK: the daemon's own configuration, links followed.
    Bank,
};

// A source opened O_NONBLOCK and checked with fstat before one byte is read,
// so that a link planted at a cache name cannot copy the file it names into
// the sandbox, and a FIFO there cannot hold rescan() in open() (entry 245).
int open_source(const char* path, Source kind, struct stat& info) {
    const int follow = kind == Source::Avatar ? O_NOFOLLOW : 0;
    const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | follow);
    if (fd < 0) {
        return -1;
    }
    const off_t ceiling = kind == Source::Settings ? off_t{1} << 20   // a settings file is KB
                          : kind == Source::Bank   ? off_t{64} << 20  // the bank is about 16 MB
                                                   : static_cast<off_t>(kAvatarRgbaBytes);
    const bool shaped = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                        (kind == Source::Avatar ? info.st_size == ceiling
                                                : info.st_size <= ceiling);
    if (!shaped) {
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

bool copy_into(int directory, const char* source_path, const char* name, Source kind) {
    struct stat source_info {};
    const int source = open_source(source_path, kind, source_info);
    if (source < 0) {
        return false;
    }
    // Temporary-and-rename, like every other file this project publishes: a game
    // reading on the other side must never meet a half-written one.
    std::string temporary = std::string(name) + ".part";
    const int fd = open_regular(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        const int error = errno;
        ::close(source);
        errno = error;
        return false;
    }
    char buffer[8192];
    bool complete = true;
    off_t offset = 0;
    for (;;) {
        const ssize_t got = ::read(source, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            complete = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (!write_at(fd, buffer, static_cast<size_t>(got), offset)) {
            complete = false;
            break;
        }
        offset += got;
    }
    if (complete && kind == Source::Bank) {
        // The bank's copy carries the source's modification time, which is how
        // the next adoption knows it is already there (same_copy()).
        const struct timespec times[2] = {source_info.st_atim, source_info.st_mtim};
        complete = ::futimens(fd, times) == 0;
    }
    ::close(fd);
    ::close(source);
    if (!complete || ::renameat(directory, temporary.c_str(), directory, name) != 0) {
        ::unlinkat(directory, temporary.c_str(), 0);
        return false;
    }
    return true;
}

// Whether the sandbox already holds a copy of this source: a regular file of
// the same size carrying the same modification time, which copy_into() gives
// every bank copy it makes. A copy of another version, or one the sandbox
// removed or replaced, is not the same copy.
bool same_copy(int directory, const char* name, const char* source_path) {
    struct stat source {};
    struct stat copy {};
    return ::stat(source_path, &source) == 0 && S_ISREG(source.st_mode) &&
           ::fstatat(directory, name, &copy, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(copy.st_mode) && copy.st_size == source.st_size &&
           copy.st_mtim.tv_sec == source.st_mtim.tv_sec &&
           copy.st_mtim.tv_nsec == source.st_mtim.tv_nsec;
}

}  // namespace

// config.ini when it moved, and again on every sweep until a copy of that
// version has arrived. What was copied and what was said are two memories, so
// that a failure is said once per version and still retried (entry 248).
//
// No config.ini on the host (removed, or a link to nothing yet) means none in
// the sandbox either: the overlay in there reads the defaults a host game
// reads, not the last copy for ever.
void FlatpakBridge::mirror_config(Mirror& mirror) {
    const long long mtime = Config::mtime();
    if (mtime == mirror.config_mtime) {
        return;
    }
    const std::string source = Config::path();
    struct stat host {};
    if (mtime == 0 && ::stat(source.c_str(), &host) != 0 && (errno == ENOENT || errno == ENOTDIR)) {
        // unlinkat() removes whatever the sandbox has at that name, a link
        // included, and never what a link points at.
        if (::unlinkat(mirror.directory, kBridgeConfigName, 0) == 0 || errno == ENOENT) {
            mirror.config_mtime = 0;
            DBG("no %s on the host: none in the Flatpak sandbox of %s", kBridgeConfigName,
                mirror.id.c_str());
        }
        return;
    }
    if (copy_into(mirror.directory, source.c_str(), kBridgeConfigName, Source::Settings)) {
        mirror.config_mtime = mtime;
        mirror.config_failure_said = 0;
        DBG("copied %s into the Flatpak sandbox of %s", kBridgeConfigName, mirror.id.c_str());
    } else if (mtime != 0 && mirror.config_failure_said != mtime) {
        LOG("could not copy %s into the Flatpak sandbox of %s (%s); trying again every sweep",
            kBridgeConfigName, mirror.id.c_str(), std::strerror(errno));
        mirror.config_failure_said = mtime;  // said once per version, tried every sweep
    }
}

void FlatpakBridge::mirror_avatars(Mirror& mirror, const SharedState& state) {
    // Both refusals below are said once per sandbox: this directory is inside
    // territory the sandbox owns, so a file planted at the avatars name (which
    // makes the O_DIRECTORY|O_NOFOLLOW open fail) is exactly the hostile shape
    // the rest of this file refuses out loud. Once, because this runs on the
    // tick and the condition persists.
    if (::mkdirat(mirror.directory, kBridgeAvatarsName, 0700) != 0 && errno != EEXIST) {
        if (!mirror.avatars_refused) {
            mirror.avatars_refused = true;
            LOG("could not create %s in the Flatpak sandbox of %s (%s): its faces stay grey",
                kBridgeAvatarsName, mirror.id.c_str(), std::strerror(errno));
        }
        return;
    }
    const int avatars = ::openat(mirror.directory, kBridgeAvatarsName,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (avatars < 0) {
        if (!mirror.avatars_refused) {
            mirror.avatars_refused = true;
            LOG("%s in the Flatpak sandbox of %s is not an ordinary directory (%s): "
                "its faces stay grey",
                kBridgeAvatarsName, mirror.id.c_str(), std::strerror(errno));
        }
        return;
    }
    mirror.avatars_refused = false;
    mirror.faces_given = true;

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
        copy_into(avatars, source, name, Source::Avatar);
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

// The colour emoji bank, once per sandbox that draws.
//
// Inside a Flatpak the compiled-in path names the *runtime's* /usr and finds
// nothing, so without this copy the overlay would draw every emoji in the
// monochrome fallback.
//
// Unlike the settings and the faces this is copied once per sandbox: sixteen
// megabytes that never change while the daemon runs. Once per SANDBOX and not
// once per mirror: a mirror is dropped when the sandbox stops asking and made
// anew when it asks again, so the flag below is not the whole memory. What is
// asked is the sandbox's own directory -- same_copy(): the pair is already
// there, at the source's size and modification time -- which also holds
// across a daemon restart, and still copies a bank that changed on the host
// or that the sandbox took away. Only into a sandbox that is given the voice
// channel (Mirror::voice()), so a Flatpak the user has excluded or never
// consented to costs nothing.
//
// Two files, in this order: the sequence table first, the bank second. The
// overlay waits on the BANK (emoji_bank.h: it looks twice a second, and reads
// the table beside the bank at the moment the bank opens), so a table copied
// after the bank could land in the half-second the overlay had already read
// past, and every sequence in that sandbox would draw as its parts for the
// life of the game. A missing table is copied as missing: the overlay says so.
void FlatpakBridge::mirror_emoji_bank(Mirror& mirror) {
    if (mirror.emoji_bank_copied) {
        return;
    }
    // The same resolution every reader of the bank makes (emoji_bank.h, and
    // CMakeLists.txt documents the override): $VOCEM_EMOJI_BANK first, the
    // installed path otherwise, so a dev tree's daemon copies the bank its
    // overlay opens. The table is beside whichever bank that is.
    const char* bank_path = std::getenv("VOCEM_EMOJI_BANK");
    if (!bank_path || !bank_path[0]) {
        bank_path = VOCEM_EMOJI_BANK_PATH;  // not sandboxed: the installed path is right here
    }
    std::string table_path = bank_path;
    const size_t slash = table_path.rfind('/');
    table_path = (slash == std::string::npos ? std::string() : table_path.substr(0, slash + 1)) +
                 kBridgeEmojiSequencesName;
    if (same_copy(mirror.directory, kBridgeEmojiSequencesName, table_path.c_str()) &&
        same_copy(mirror.directory, kBridgeEmojiBankName, bank_path)) {
        mirror.emoji_bank_copied = true;
        DBG("%s is already in the Flatpak sandbox of %s", kBridgeEmojiBankName,
            mirror.id.c_str());
        return;
    }
    // Either one differing is a new pair: both go, the table first.
    if (!copy_into(mirror.directory, table_path.c_str(), kBridgeEmojiSequencesName,
                   Source::Bank)) {
        LOG("no emoji sequence table at %s to give the Flatpak sandbox of %s: its emoji "
            "sequences draw as their parts (%s)", table_path.c_str(), mirror.id.c_str(),
            std::strerror(errno));
    }
    if (copy_into(mirror.directory, bank_path, kBridgeEmojiBankName, Source::Bank)) {
        mirror.emoji_bank_copied = true;
        DBG("copied %s into the Flatpak sandbox of %s", kBridgeEmojiBankName, mirror.id.c_str());
        return;
    }
    // Said once, not once per tick: a machine with no bank installed is a
    // decision somebody made, not an error to repeat every second.
    mirror.emoji_bank_copied = true;
    LOG("no colour emoji bank at %s to give the Flatpak sandbox of %s: its emoji stay "
        "monochrome (%s)", bank_path, mirror.id.c_str(), std::strerror(errno));
}

void FlatpakBridge::refresh_files(const SharedState& state) {
    for (Mirror& mirror : mirrors_) {
        // The settings always: they are what the overlay reads to decide whether
        // it draws in this game at all, so withholding them would make the
        // decision unanswerable -- and an overlay that cannot read shown_apps
        // cannot say drawing=1 for a game the user switched on, so the refusal
        // above would never be said for the one case it exists for. They carry
        // the user's settings and application lists, not the voice channel.
        // The faces and the emoji bank only where the channel goes.
        mirror_config(mirror);
        if (mirror.voice()) {
            mirror_avatars(mirror, state);
            mirror_emoji_bank(mirror);
        }
    }
}

}  // namespace vocem
