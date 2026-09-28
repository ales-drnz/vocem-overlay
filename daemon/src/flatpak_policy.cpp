// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Who is given what: see flatpak_bridge_parts.h.

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

using bridge::remove_faces;

namespace bridge {

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

}  // namespace bridge

namespace {

using bridge::printable_id;

// The Flatpak exports directories this daemon asks about an application id:
// the user's installation under $XDG_DATA_HOME/flatpak, and every
// $XDG_DATA_DIRS root that is one (Flatpak's own profile and its systemd user
// environment generator put both there:
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
// of the host, never of the sandbox: everything below is outside it. The id
// is the name of a directory under $XDG_RUNTIME_DIR/app, which a sandbox
// holding the xdg-run/app grant can make under any name. So this answers what
// the id may be given, and check_running() answers whether the directory is
// that application at all: a process of it must be running.
//
// Two yeses. The user listed the id in `flatpak_apps`. Or the desktop entry
// Flatpak exported for that id says Game -- by the same rule the detection
// applies on the host (detail::categories_say_game: `Game`, not `GameTool` or
// `LauncherStore`), which lets Steam, Heroic or Sober through and keeps a chat
// client out.
//
// The exported entry is always a symbolic link, into
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
    // Taken back. The words and the faces go now -- the next publish is a
    // cleared state -- and the refusal may be said again, since what it
    // answers has changed.
    ::unlinkat(mirror.directory, kBridgeNoteName, 0);
    take_faces_back(mirror);
    mirror.refusal_said = false;
    LOG("no longer serving the voice channel to %s: %s", mirror.id.c_str(), consent.why.c_str());
}

// The ids of the sandboxes with a process running, once per sweep at most:
// about a millisecond (flatpak_process.h), and only asked while some mirror
// is drawing with the host's consent -- which is a Flatpak game being played.
const std::set<std::string>& FlatpakBridge::running_ids() {
    if (!running_scanned_) {
        running_scanned_ = true;
        if (!running_flatpak_ids(running_ids_) && !proc_refused_said_) {
            proc_refused_said_ = true;
            LOG("/proc cannot be listed: no Flatpak sandbox can be seen running, so none is "
                "given the voice channel");
        }
    }
    return running_ids_;
}

// Whether the directory is the application its name says: a process of this
// user's is running in that id's Flatpak scope, or in a sandbox whose
// /.flatpak-info names it (flatpak_process.h: from the daemon's unit only the
// scope can be seen). The name alone is not evidence: any sandbox
// holding the xdg-run/app grant can `mkdir $XDG_RUNTIME_DIR/app/<a game's id>`
// and write a `request` with drawing=1 in it. A process's scope and its
// /.flatpak-info are what a sandbox cannot forge.
//
// Asked on every sweep, not once: a directory stays after its application
// exits, and a mirror whose game has gone is somebody else's to write into
// from then on. What a running application's own directory is exposed to --
// another sandbox with the same grant can read it while the game runs -- is
// not closed by this; the grant hands over the whole of $XDG_RUNTIME_DIR/app.
void FlatpakBridge::check_running(Mirror& mirror) {
    if (!mirror.drawing || !mirror.consented) {
        mirror.running = false;  // nothing to be served, so nothing to ask
        return;
    }
    const bool was = mirror.running;
    mirror.running = running_ids().count(mirror.id) != 0;
    if (mirror.running) {
        if (mirror.absence_said) {
            mirror.absence_said = false;
            LOG("serving the voice channel to %s: a process of it is running", mirror.id.c_str());
        }
        return;
    }
    if (was) {
        // Its game went away. The words go now, as when consent is taken back.
        ::unlinkat(mirror.directory, kBridgeNoteName, 0);
    }
    if (!mirror.absence_said) {
        mirror.absence_said = true;
        LOG("%s the voice channel to %s: no process of this user's is running in its sandbox "
            "(none in its Flatpak scope or with a /.flatpak-info naming it), and a "
            "directory's name alone is not the "
            "application",
            was ? "no longer serving" : "not serving", mirror.id.c_str());
    }
}

// Wherever the voice channel stops going -- consent taken back, the game gone,
// the overlay switched off there -- the faces copied for it are removed, as the
// note is. The emoji bank stays: it is the package's, the same for everyone,
// and sixteen megabytes to copy again.
void FlatpakBridge::take_faces_back(Mirror& mirror) {
    if (mirror.voice() || !mirror.faces_given) {
        return;
    }
    remove_faces(mirror.directory);
    mirror.faces_given = false;
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
    // Config::load() refuses what is not a regular file without opening it
    // for long (a FIFO at the name cannot hold the sweep), and says when the
    // file could not be read whole: consent is then what the defaults give,
    // which is nobody, rather than whatever part of the list was read.
    Config config;
    if (!config.load()) {
        config = Config();
    }
    flatpak_apps_ = config.flatpak_apps;
    for (Mirror& mirror : mirrors_) {
        decide(mirror, true);
    }
}

}  // namespace vocem
