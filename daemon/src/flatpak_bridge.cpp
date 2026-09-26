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
#include <vector>

#include "log.h"
#include "vocem/apps.h"
#include "vocem/avatar_rgba.h"
#include "vocem/config.h"
#include "vocem/flatpak.h"
#include "vocem/note.h"
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

namespace {

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

// A source opened O_NONBLOCK and checked with fstat before one byte is read.
// Through 0.1.10 this was `fopen(source, "rb")`: measured, a link planted at
// a cache name copied the file it named (a stand-in token) into the sandbox,
// and a FIFO there held rescan() in open() -- through SIGTERM, which restarts
// the call -- until the unit's SIGKILL, leaving the segment behind.
int open_source(const char* path, Source kind) {
    const int follow = kind == Source::Avatar ? O_NOFOLLOW : 0;
    const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | follow);
    if (fd < 0) {
        return -1;
    }
    struct stat info {};
    const off_t ceiling = kind == Source::Settings ? off_t{1} << 20   // a settings file is KB
                          : kind == Source::Bank   ? off_t{64} << 20  // 16.3 MB today
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
    const int source = open_source(source_path, kind);
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
    ::close(fd);
    ::close(source);
    if (!complete || ::renameat(directory, temporary.c_str(), directory, name) != 0) {
        ::unlinkat(directory, temporary.c_str(), 0);
        return false;
    }
    return true;
}

}  // namespace

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
        // quietly" -- which this line, unguarded, made false in both halves on
        // any machine without a session runtime directory.
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
void FlatpakBridge::write_record_for(Mirror& mirror, const Request& request) {
    if (request.name.empty() || request.name == mirror.recorded) {
        return;
    }
    mirror.recorded = request.name;
    Application application;
    application.key = request.name;
    application.executable = request.executable;
    application.api = request.api;
    application.desktop = mirror.id;
    application.looks_like_game = request.game;
    application.reason = request.why;
    write_application_record(application);
    LOG("wrote the record of '%s' for the Flatpak sandbox of %s (%s)", request.name.c_str(),
        mirror.id.c_str(), request.why.c_str());
}

namespace {

// A Flatpak application id, as flatpak's own flatpak_is_valid_name() has
// it: at least two elements separated by dots, each starting with a letter
// or an underscore and made of letters, digits, '_' and '-', at most 255
// bytes in all. Nothing else creates a directory under $XDG_RUNTIME_DIR/app
// that this daemon should look inside.
bool looks_like_app_id(const char* id) {
    size_t length = 0;
    int elements = 0;
    bool element_start = true;
    for (const char* c = id; *c; ++c, ++length) {
        if (*c == '.') {
            if (element_start) {
                return false;  // an empty element
            }
            element_start = true;
            continue;
        }
        const bool letter = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_';
        const bool digit = *c >= '0' && *c <= '9';
        if (element_start) {
            if (!letter) {
                return false;
            }
            ++elements;
            element_start = false;
        } else if (!letter && !digit && *c != '-') {
            return false;
        }
    }
    return elements >= 2 && !element_start && length <= 255;
}

// A directory name for the log: anything a dirent can hold, one line.
std::string printable_id(const char* id) {
    std::string out;
    for (const char* c = id; *c && out.size() < 64; ++c) {
        out.push_back((*c >= 0x20 && *c != 0x7F) ? *c : '?');
    }
    return out;
}

// The Flatpak exports directories this daemon asks about an application id:
// the user's installation under $XDG_DATA_HOME/flatpak, and every
// $XDG_DATA_DIRS root that is one (Flatpak's own profile and its systemd user
// environment generator put both there -- measured on this machine:
// `~/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:...`).
// The system installation is added by name only when the list does not carry
// any exports directory at all, which is a session Flatpak's profile never
// reached; a list that names one is taken at its word, which is also what
// lets a test point both at scratch trees.
std::vector<std::string> flatpak_export_roots() {
    static const std::string kExports = "/flatpak/exports/share";
    const auto is_exports = [](std::string root) {
        while (root.size() > 1 && root.back() == '/') {
            root.pop_back();
        }
        return root.size() >= kExports.size() &&
               root.compare(root.size() - kExports.size(), kExports.size(), kExports) == 0;
    };
    const std::vector<std::string> data = detail::desktop_roots();
    std::vector<std::string> roots;
    if (!data.empty()) {
        roots.push_back(data.front() + kExports);  // $XDG_DATA_HOME, or ~/.local/share
    }
    bool listed_any = false;
    for (size_t i = 1; i < data.size(); ++i) {
        if (is_exports(data[i])) {
            roots.push_back(data[i]);
            listed_any = true;
        }
    }
    if (!listed_any) {
        roots.push_back("/var/lib/flatpak/exports/share");
    }
    return roots;
}

// A path with every link resolved, or empty. realpath() walks with lstat and
// readlink and opens nothing, so a FIFO on the way cannot hold it.
std::string resolved(const std::string& path) {
    char* real = ::realpath(path.c_str(), nullptr);
    if (!real) {
        return {};
    }
    std::string out = real;
    std::free(real);
    return out;
}

struct Consent {
    bool allowed = false;
    std::string why;
};

// Whether the application with this id may be given the voice channel. Asked
// of the host, never of the sandbox: the id is the name of the directory
// Flatpak made for it under $XDG_RUNTIME_DIR/app, which the application
// cannot choose, and everything else below is outside the sandbox.
//
// Two yeses. The user listed the id in `flatpak_apps`. Or the desktop entry
// Flatpak exported for that id says Game -- by the same rule the detection
// applies on the host (detail::categories_say_game: `Game`, not `GameTool` or
// `LauncherStore`), which lets Steam, Heroic or Sober through and keeps a chat
// client out.
//
// The exported entry is a symbolic link, always: measured, all five under
// /var/lib/flatpak/exports/share/applications on this machine point into
// `../../../app/<id>/current/active/export/...`. So the link is followed --
// an O_NOFOLLOW open refuses every real Flatpak -- and what it resolves to
// must be inside that installation's own `app/<id>/`: an exported name that
// leads into another application's files is not that application's entry.
// The file itself is read by detail::read_entry, which opens the resolved
// path O_NOFOLLOW|O_NONBLOCK, regular files only, a megabyte at most.
Consent consent_for(const std::string& id, const std::string& flatpak_apps) {
    if (listed(flatpak_apps, id)) {
        return {true, "listed in flatpak_apps"};
    }
    static const std::string kShare = "/exports/share";
    for (const std::string& root : flatpak_export_roots()) {
        const std::string link = root + "/applications/" + id + ".desktop";
        const std::string entry_path = resolved(link);
        if (entry_path.empty()) {
            continue;
        }
        const std::string installation =
            resolved(root.substr(0, root.size() >= kShare.size() ? root.size() - kShare.size() : 0));
        const std::string own = installation + "/app/" + id + "/";
        if (installation.empty() || entry_path.compare(0, own.size(), own) != 0) {
            return {false, "its exported desktop entry " + link +
                               " leads outside the application's own files"};
        }
        const detail::Entry entry = detail::read_entry(entry_path);
        if (!entry.found) {
            return {false, "its exported desktop entry " + link + " could not be read"};
        }
        if (detail::categories_say_game(entry.categories)) {
            return {true, "its desktop entry says Game"};
        }
        return {false, "its desktop entry is not a game (Categories=" +
                           printable_id(entry.categories.c_str()) + ")"};
    }
    return {false, "no Flatpak desktop entry is exported for it"};
}

}  // namespace

void FlatpakBridge::decide(Mirror& mirror, bool announce) {
    const Consent consent = consent_for(mirror.id, flatpak_apps_);
    const bool was = mirror.consented;
    mirror.consented = consent.allowed;
    mirror.consent_why = consent.why;
    if (!announce || was == consent.allowed) {
        return;
    }
    if (consent.allowed) {
        LOG("serving the voice channel to %s: %s", mirror.id.c_str(), consent.why.c_str());
        return;
    }
    // Taken back. The words go now -- the next publish is a cleared state --
    // and the refusal may be said again, since what it answers has changed.
    ::unlinkat(mirror.directory, kBridgeNoteName, 0);
    mirror.refusal_said = false;
    LOG("no longer serving the voice channel to %s: %s", mirror.id.c_str(), consent.why.c_str());
}

// Once per adoption, and only for a sandbox that asks to draw: one that says
// drawing=0 is not asking for anything this would refuse.
void FlatpakBridge::say_refusal(Mirror& mirror) {
    if (!mirror.drawing || mirror.consented || mirror.refusal_said) {
        return;
    }
    mirror.refusal_said = true;
    LOG("not serving the voice channel to %s: %s; add the id to flatpak_apps in config.ini to "
        "allow it (it is given its settings and a cleared state)",
        mirror.id.c_str(), mirror.consent_why.c_str());
}

void FlatpakBridge::refresh_consent() {
    const long long mtime = Config::mtime();
    if (mtime == consent_config_mtime_) {
        return;
    }
    consent_config_mtime_ = mtime;
    // Read only when it is a regular file: Config::load() is a plain fopen,
    // and a FIFO at the name would hold the sweep until a writer came.
    struct stat info {};
    Config config;
    if (::stat(Config::path().c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
        config.load();
    }
    flatpak_apps_ = config.flatpak_apps;
    for (Mirror& mirror : mirrors_) {
        decide(mirror, true);
    }
}

void FlatpakBridge::rescan() {
    if (applications_ < 0 && !start()) {
        return;
    }
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
            // The record arrives a frame after the request that adopted this
            // sandbox, so this is where it is usually seen.
            write_record_for(mirror, request);
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
        if (known) {
            continue;
        }
        // The two bounds on who gets served, refused out loud (entry 134).
        // A directory under $XDG_RUNTIME_DIR/app can be made by any process
        // of the user's -- a sandbox with the xdg-run/app grant included --
        // and every mirror costs two descriptors, the emoji bank -- 16.3 MB
        // today, with its sequence table beside it, where this comment said six
        // megabytes until 2026-09-18 and the two other comments about the same
        // file were corrected without it -- and a share of every publish:
        // without a ceiling a few hundred
        // asking directories exhausted this process's descriptors, after
        // which the socket, /proc/net/tcp and the segment itself all failed
        // to open. A Flatpak application id is reverse-DNS -- letters,
        // digits, '.', '_' and '-', at least one dot -- and a name that is
        // not one was never made by Flatpak.
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
        // listed in flatpak_apps. Everywhere else the answer is a cleared
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

        // The same "<name>.part" rule copy_into() spells: this was the one
        // place the bridge's file naming was stated twice, as a literal.
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

void FlatpakBridge::mirror_config(Mirror& mirror) {
    const long long mtime = Config::mtime();
    if (mtime == mirror.config_mtime) {
        return;
    }
    const std::string source = Config::path();
    if (copy_into(mirror.directory, source.c_str(), kBridgeConfigName, Source::Settings)) {
        mirror.config_mtime = mtime;
        DBG("copied %s into the Flatpak sandbox of %s", kBridgeConfigName, mirror.id.c_str());
    } else if (mtime != 0) {
        LOG("could not copy %s into the Flatpak sandbox of %s (%s)", kBridgeConfigName,
            mirror.id.c_str(), std::strerror(errno));
        mirror.config_mtime = mtime;  // say it once per change, not once per tick
    }
}

void FlatpakBridge::mirror_avatars(Mirror& mirror, const SharedState& state) {
    // Both refusals below are said once per sandbox: this directory is inside
    // territory the sandbox owns, so a file planted at the avatars name (which
    // makes the O_DIRECTORY|O_NOFOLLOW open fail) is exactly the hostile shape
    // the rest of this file refuses out loud -- and a game whose faces never
    // arrive with an empty log was the one quiet corner of it. Once, because
    // this runs on the tick and the condition persists.
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
// nothing, so the overlay drew every emoji in the monochrome fallback and said
// so in the log and nowhere else. It is the same omission the note segment had
// before entry 88 -- a fourth thing to carry that nothing carried -- and the
// same answer.
//
// Unlike the settings and the faces this is copied exactly once: sixteen
// megabytes that never change while the daemon runs. Only into a sandbox that
// is given the voice channel (Mirror::voice()), so a Flatpak the user has
// excluded or never consented to costs nothing, and sixteen megabytes of the
// runtime directory is a real cost to name rather than spend quietly.
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
    // installed path otherwise. The bridge used to take the installed path
    // alone, so in a dev tree the overlay opened the built bank while the
    // daemon copied nothing and logged a missing file that was not the one
    // being used. The table is beside whichever bank that is.
    const char* bank_path = std::getenv("VOCEM_EMOJI_BANK");
    if (!bank_path || !bank_path[0]) {
        bank_path = VOCEM_EMOJI_BANK_PATH;  // not sandboxed: the installed path is right here
    }
    std::string table_path = bank_path;
    const size_t slash = table_path.rfind('/');
    table_path = (slash == std::string::npos ? std::string() : table_path.substr(0, slash + 1)) +
                 kBridgeEmojiSequencesName;
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
