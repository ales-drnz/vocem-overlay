// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which applications the overlay is loaded into, and which of them it must leave
// alone.
//
// The list is not guessed and nothing is scanned for. The overlay is already
// inside every Vulkan and OpenGL process on the machine -- that is what it is --
// so each process writes down that it was here, once, and the configuration window
// reads the result. An application that never ran cannot be in the list, and an
// application that ran cannot be missing from it.
//
// One small file per application under $XDG_CACHE_HOME/vocem/apps, written with
// the usual temporary-and-rename so a half-written file is never read. One file
// each rather than one shared list, because these are written from inside other
// people's processes, concurrently, with no lock between them: two games starting
// at once would otherwise interleave into one corrupt line. The temporary carries
// the writer's pid for the same reason -- the file name is the process name, and
// two processes of one name are ordinary (`java`, `CrGpuMain`).
//
// Header-only and dependency-free, like the rest of what the injected code shares:
// the in-game side must not pull in a library to do this.

#ifndef VOCEM_APPS_H
#define VOCEM_APPS_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "vocem/flatpak.h"
#include "vocem/paths.h"

namespace vocem {

// What one application is known by.
struct Application {
    // The key a rule is written against, and the file's name: the process name,
    // which is what /proc/self/comm reports.
    std::string key;
    // The executable behind it, in full. /proc/self/comm is truncated to fifteen
    // characters by the kernel, so it is not enough to tell two long names apart
    // and not enough to show anybody either.
    std::string executable;
    // "vulkan" or "opengl": which of the two paths drew here.
    std::string api;
    // The desktop entry this process was launched from, when the session said so:
    // either the file's path or its id. Two standard sources, both read from
    // inside the process, which is the one place they can be read from.
    std::string desktop;
    // Steam's app id, when the process has it in its environment. Not a path and
    // not a guess about where Steam is installed: it is the number the game itself
    // was started with, and it is what Steam's own desktop entries name.
    std::string steam_app_id;
    // When it was last seen, as a unix timestamp.
    long seen = 0;
    // What the detection made of it when it ran, which is what decides whether the
    // overlay draws there unless the user has said otherwise.
    bool looks_like_game = false;
    // Which signal decided that, in the process's own words: `steam:2357570`,
    // `desktop:Overwatch.desktop`, `not-ours:minecraft-launcher`, `nothing`. A game
    // the detection misses has to stay a case somebody can explain, so the evidence
    // is written down beside the verdict rather than being thrown away with it.
    std::string reason;
};

namespace detail {

inline std::string read_first_line(const char* path) {
    std::FILE* file = std::fopen(path, "r");
    if (!file) {
        return {};
    }
    char buffer[512] = {};
    const char* read = std::fgets(buffer, sizeof(buffer), file);
    std::fclose(file);
    if (!read) {
        return {};
    }
    std::string value(buffer);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

inline std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A file name that cannot escape the directory it is meant to be in. Process
// names come from whatever is running and are not to be trusted with a path.
inline std::string sanitised(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char character : name) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '.' ||
                          character == '-' || character == '_';
        out.push_back(safe ? character : '_');
    }
    if (out.empty() || out == "." || out == "..") {
        out = "unknown";
    }
    return out;
}

// A systemd unit name, with the escaping systemd puts in it taken back out.
//
// The dash is the separator in a unit name, so a dash that is part of a name is
// written `\x2d`. Every application id with a dash in it therefore arrives from
// `/proc/self/cgroup` spelled wrong, and the entry is looked for under a name no
// file has: found in the field, in a record that said
// `no-entry:io.github.plrigaux.sysd\x2dmanager` when the entry is plainly
// `io.github.plrigaux.sysd-manager`. It was not a rare shape either -- a dash in an
// id is ordinary, and every one of them was silently unrecognised.
//
// `\xNN` is the whole of the escaping that matters here; anything else is left
// alone rather than guessed at, and a truncated escape at the end of the string is
// left as it stands.
inline std::string unescaped_unit(const std::string& name) {
    if (name.find("\\x") == std::string::npos) {
        return name;
    }
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        const int high = i + 3 < name.size() && name[i] == '\\' && name[i + 1] == 'x'
                             ? digit(name[i + 2])
                             : -1;
        const int low = high >= 0 ? digit(name[i + 3]) : -1;
        if (low >= 0) {
            out.push_back(static_cast<char>(high * 16 + low));
            i += 3;
        } else {
            out.push_back(name[i]);
        }
    }
    return out;
}

// Two process names that are the same name, allowing for where the kernel cut one
// of them.
//
// /proc/self/comm is TASK_COMM_LEN - 1 = fifteen characters, so anything longer
// arrives cut: `minecraft-launcher` as `minecraft-launc`, `steamwebhelper.exe` as
// `steamwebhelper.`. The cut form is only compared against a candidate long enough
// to have been cut, so that no short name gains a prefix match it should not have.
inline bool same_name(const char* candidate, const std::string& name) {
    if (name == candidate) {
        return true;
    }
    return std::strlen(candidate) > kCommLength && name.size() == kCommLength &&
           name.compare(0, kCommLength, candidate, kCommLength) == 0;
}

// The application ids a systemd cgroup line can be read as, best first.
//
// systemd's own `DESKTOP_ENVIRONMENTS.md` spells the unit
// `app[-<launcher>]-<ApplicationID>[@<RANDOM>].service` and
// `app[-<launcher>]-<ApplicationID>-<RANDOM>.scope`, and says the id "can be
// retrieved by stripping the prefix and postfix". The prefix has an optional
// launcher inside it and this code used to keep it, which cost two things
// measured on this machine: a D-Bus-activated application is a unit *inside* an
// `app-dbus-...` slice, so Telegram's record read
// `no-entry:dbus-:1.2-org.telegram.desktop.slice/dbus-:1.2-org.telegram.desktop`
// and the logout greeter's the same; and GNOME writes
// `app-gnome-<id>-<pid>.scope`, where the whole desktop-entry signal was dead
// before it started.
//
// Which reading is right cannot be decided from the string, so this returns them
// in order and the caller tries each against the entries on disk. Widening it is
// safe because an entry still has to name this executable before it is believed --
// the guard in `game_verdict` -- and a wrong id that passes that guard is not
// wrong in any way that shows.
inline std::vector<std::string> desktop_ids_from_cgroup(const std::string& cgroup) {
    std::vector<std::string> candidates;
    if (cgroup.find("/app-") == std::string::npos) {
        return candidates;
    }
    // The leaf unit, which is the one that actually ran: in the D-Bus shape the
    // `app-` piece is the slice around it.
    std::string unit = cgroup.substr(cgroup.find_last_of('/') + 1);
    bool scope = false;
    for (const char* suffix : {".scope", ".service", ".slice"}) {
        const size_t length = std::strlen(suffix);
        if (unit.size() > length && unit.compare(unit.size() - length, length, suffix) == 0) {
            scope = (suffix[1] == 's' && suffix[2] == 'c');
            unit.resize(unit.size() - length);
            break;
        }
    }
    // One base's readings: the `app-` prefix off, the escaping out, then the
    // same name with the optional launcher taken off -- `gnome-`, `flatpak-`,
    // and the two pieces D-Bus activation puts in front (`dbus-`, then the
    // connection's name, `:1.2`). Deduplicated, because the two bases below
    // often agree.
    const auto offer = [&candidates](std::string base) {
        if (base.compare(0, 4, "app-") == 0) {
            base = base.substr(4);
        }
        base = unescaped_unit(base);
        if (base.empty()) {
            return;
        }
        const auto push = [&candidates](const std::string& value) {
            for (const std::string& existing : candidates) {
                if (existing == value) {
                    return;
                }
            }
            candidates.push_back(value);
        };
        push(base);
        for (int strip = 0; strip < 2; ++strip) {
            const size_t dash = base.find('-');
            if (dash == std::string::npos) {
                break;
            }
            base = base.substr(dash + 1);
            push(base);
        }
    };
    // The random part: `@<RANDOM>` on a service, `-<RANDOM>` on a scope -- and
    // on a service the whole part is OPTIONAL (systemd's own spelling is
    // `app[-<launcher>]-<ApplicationID>[@<RANDOM>].service`). With an `@` the
    // cut is certain. Without one the dash rule is a guess: right for every
    // scope, where the random part is not optional, and wrong for a service
    // that simply has none, where it ate everything after the id's first dash
    // -- `app-org.gnome.Evince.service` read as ["app"] and the whole
    // desktop-entry signal was dead for that shape. So for a service (or a
    // slice, which never carries a random part) the unit as it stands is
    // offered as a second base. Widening is safe for the reason above: a found
    // entry still has to name this executable before it is believed.
    std::string stripped = unit;
    if (const size_t at = stripped.find('@'); at != std::string::npos) {
        stripped.resize(at);
        offer(stripped);
    } else {
        if (const size_t last = stripped.rfind('-'); last != std::string::npos) {
            stripped.resize(last);
        }
        offer(stripped);
        if (!scope) {
            offer(unit);
        }
    }
    return candidates;
}

// Where entries live: the user's directory first, then the system ones, in the
// order the Base Directory Specification gives them.
inline std::vector<std::string> desktop_roots() {
    std::vector<std::string> roots;
    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home) {
        roots.emplace_back(home);
    } else if (const char* base = std::getenv("HOME"); base && *base) {
        roots.emplace_back(std::string(base) + "/.local/share");
    }
    const char* dirs = std::getenv("XDG_DATA_DIRS");
    const std::string list = dirs && *dirs ? dirs : "/usr/local/share:/usr/share";
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(':', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        if (end > start) {
            roots.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return roots;
}

// The entry with this id.
//
// An id is a path with the separators turned into dashes: the specification says
// to "make its full path relative to the $XDG_DATA_DIRS component in which the
// desktop file is installed, remove the 'applications/' prefix, and turn '/' into
// '-'", so `/usr/share/applications/foo/bar.desktop` is `foo-bar.desktop`. Going
// back the other way is a guess, because a dash in an id may be a dash or may be
// a directory, so each is tried in turn, left to right. This is not academic:
// wine writes the entries for the programs it installs under
// `applications/wine/Programs/`, so a game installed in a prefix has an id with
// two directories in it, and looking only for the flat name found nothing.
inline std::string find_desktop_entry(const std::string& reference) {
    if (!reference.empty() && reference.front() == '/') {
        return reference;
    }
    if (reference.empty()) {
        return {};
    }
    std::string file =
        reference.size() > 8 && reference.compare(reference.size() - 8, 8, ".desktop") == 0
            ? reference
            : reference + ".desktop";

    const std::vector<std::string> roots = desktop_roots();
    size_t dash = std::string::npos;
    for (;;) {
        for (const std::string& root : roots) {
            const std::string path = root + "/applications/" + file;
            struct stat info {};
            if (::stat(path.c_str(), &info) == 0) {
                return path;
            }
        }
        dash = file.find('-', dash == std::string::npos ? 0 : dash + 1);
        if (dash == std::string::npos) {
            return {};
        }
        file[dash] = '/';
    }
}

// The three fields of a desktop entry this decision turns on, read in one pass.
//
// `found` is a different question from "the file opened". A
// `GIO_LAUNCHED_DESKTOP_FILE` need not name a desktop entry at all: measured
// here, a game started from the file manager arrived with it pointing at the
// executable itself, and reading an ELF for `Exec=` naturally found nothing --
// which the verdict then reported as `not-ours:`, when the truth was that there
// was no entry for it to be somebody else's.
struct Entry {
    bool found = false;
    std::string exec;
    std::string try_exec;
    std::string categories;
};

inline Entry read_entry(const std::string& path) {
    Entry entry;
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return entry;
    }
    bool inside = false;
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (!text.empty() && text.front() == '[') {
            if (inside) {
                break;  // the group ended, and everything wanted is behind us
            }
            inside = text == "[Desktop Entry]";
            entry.found = entry.found || inside;
            continue;
        }
        if (!inside) {
            continue;
        }
        for (const auto& field : {std::pair<const char*, std::string*>{"Exec", &entry.exec},
                                  {"TryExec", &entry.try_exec},
                                  {"Categories", &entry.categories}}) {
            const size_t length = std::strlen(field.first);
            if (text.size() > length && text.compare(0, length, field.first) == 0 &&
                text[length] == '=') {
                *field.second = text.substr(length + 1);
            }
        }
    }
    std::fclose(file);
    return entry;
}

// Whether an entry's `Exec` or `TryExec` line names this program.
//
// The first word of `Exec` is not the program often enough to matter, and taking
// only the first word is how a program whose entry starts a wrapper was told it
// belonged to somebody else. Measured against this machine's own entries:
// `"/usr/lib/REAPER/reaper" %F` is quoted, so the first word ended in a quote and
// matched nothing; `/usr/bin/env systemctl start ...` and `/bin/sh -c "..."` and
// `env "WINEPREFIX=..." wine start ...` all name a wrapper first; the Mojang
// launcher's entry is `minecraft-launcher.sh` while the process behind it is
// `minecraft-launcher`; and the Steam client's entry on this machine reads
// `/usr/bin/mangohud /usr/bin/steam %U`.
//
// So every word is considered, minus the ones that cannot be a program: options,
// the specification's field codes (`%U`, `%f`), and the `VAR=value` assignments
// `env` takes in front of a command. A quoted word is unquoted but never split:
// what is inside `sh -c "..."` is a script, not an argument list, and guessing at
// it would be exactly the widening this guard exists to prevent.
inline bool exec_names(const std::string& exec, const std::string& binary,
                       const std::string& comm) {
    size_t index = 0;
    while (index < exec.size()) {
        while (index < exec.size() && (exec[index] == ' ' || exec[index] == '\t')) {
            ++index;
        }
        std::string token;
        if (index < exec.size() && (exec[index] == '"' || exec[index] == '\'')) {
            const char quote = exec[index++];
            while (index < exec.size() && exec[index] != quote) {
                if (exec[index] == '\\' && index + 1 < exec.size()) {
                    ++index;
                }
                token.push_back(exec[index++]);
            }
            if (index < exec.size()) {
                ++index;
            }
        } else {
            while (index < exec.size() && exec[index] != ' ' && exec[index] != '\t') {
                token.push_back(exec[index++]);
            }
        }
        if (token.empty() || token.front() == '-' || token.front() == '%') {
            continue;
        }
        if (const size_t equals = token.find('='); equals != std::string::npos) {
            const size_t slash = token.find('/');
            if (slash == std::string::npos || slash > equals) {
                continue;  // an environment assignment, not a program
            }
        }
        const std::string name = basename_of(token);
        if (name.empty()) {
            continue;
        }
        if ((!binary.empty() && name == binary) || same_name(name.c_str(), comm)) {
            return true;
        }
    }
    return false;
}

// Whether a `Categories=` line puts this entry in the games section. Defined here
// because the search below needs it; the reasoning is at its second reader.
inline bool categories_say_game(const std::string& categories);

// The installed entry that runs this program, when exactly one kind of entry
// names it and that kind is a game.
//
// This is the last thing asked and the only signal nobody handed over: the other
// three are somebody saying so -- a launcher through the environment, the game
// through its own arguments, the session through the scope it started us in. A
// game started from a terminal, from a script, or by hand has none of those, and
// that was written down as "not found, by design". It is findable: the entries are
// on disk, and one of them may name this executable.
//
// What makes it safe is the second half of the rule: **exactly one** entry may
// name this program, and it has to be a game. An entry naming it is not enough,
// because entries name interpreters and wrappers -- a `Categories=Game` entry
// reading `Exec=python3 /usr/share/foo/main.py` would otherwise make every Python
// program on the machine a game. Measured here, that is not a hypothetical: the
// thirty per-game entries Steam writes all read `Exec=steam steam://rungameid/…`
// and all say `Game`, so "every entry that names it is a game" was true of the
// name `steam` itself. One entry, and one only, is the rule that held.
//
// Duplicate ids across the data directories are one entry, not two: the
// specification says the first in `$XDG_DATA_DIRS` order is the one used, and the
// user's copy of a system entry is the ordinary case.
//
// Costs one pass over the installed entries, once, in the process that pays it:
// measured here at 2.2-2.7 ms over four runs for the 251 entries of this machine
// -- the whole verdict, of which this pass is nearly all -- in the same frame that
// already writes the record. It is only reached by processes that got no answer
// from anywhere else, which on this machine is the compositor, the portals and the
// probes -- everything with an entry of its own has already been decided by it.
inline std::string entry_that_runs_this(const std::string& binary, const std::string& comm) {
    if (binary.empty() && comm.empty()) {
        return {};
    }
    // Entries live in subdirectories too -- wine's are two deep -- and an id
    // spells those directories with dashes, so the search is over the tree and
    // the answer is the id rather than the path.
    std::vector<std::pair<std::string, std::string>> pending;  // directory, id prefix
    for (const std::string& root : desktop_roots()) {
        pending.emplace_back(root + "/applications", std::string());
    }
    std::vector<std::string> naming;  // the distinct ids that name this program
    std::string found;                // the id of the one that says Game
    for (size_t at = 0; at < pending.size() && naming.size() < 2; ++at) {
        DIR* handle = ::opendir(pending[at].first.c_str());
        if (!handle) {
            continue;
        }
        while (const dirent* item = ::readdir(handle)) {
            const std::string name = item->d_name;
            if (name.empty() || name.front() == '.') {
                continue;
            }
            const std::string path = pending[at].first + "/" + name;
            if (name.size() < 9 || name.compare(name.size() - 8, 8, ".desktop") != 0) {
                struct stat info {};
                // A directory, whose name becomes part of every id inside it. Two
                // levels are enough for anything the specification produces and
                // keep a stray tree from being walked to the bottom.
                if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                    std::count(pending[at].second.begin(), pending[at].second.end(), '-') < 2) {
                    pending.emplace_back(path, pending[at].second + name + "-");
                }
                continue;
            }
            const Entry entry = read_entry(path);
            if (!entry.found || (!exec_names(entry.exec, binary, comm) &&
                                 !exec_names(entry.try_exec, binary, comm))) {
                continue;
            }
            const std::string id = pending[at].second + name;
            if (std::find(naming.begin(), naming.end(), id) != naming.end()) {
                continue;  // the same id in two data directories is one entry
            }
            naming.push_back(id);
            if (naming.size() > 1) {
                break;  // named by more than one: not specific enough to go on
            }
            if (categories_say_game(entry.categories)) {
                found = id;
            }
        }
        ::closedir(handle);
    }
    return naming.size() == 1 ? found : std::string();
}

}  // namespace detail

// Ours, and never to be drawn in.
//
// The configuration window is an OpenGL application like any other, and the shim
// is preloaded for the whole session, so the overlay loaded itself into its own
// settings window and was perfectly prepared to paint a voice panel over it --
// verified, not suspected: the log shows the backend coming up and avatars being
// uploaded inside vocem-config. It also turned up in the window's own list of
// applications, with a switch offering to turn it off.
//
// Not a setting and not a default anybody can edit: there is no configuration in
// which this is wanted.
inline bool is_own_process(const std::string& name) {
    return name == "vocem-config" || name == "vocemd" || name == "vocem" ||
           name == "vocem-run";
}

// Things that carry a game's signals without being the game.
//
// This is a correction to the two signals below, not a policy of its own, and it
// exists because both signals are wrong about exactly this set:
//
//   * **A launcher's desktop entry says `Categories=Game`.** Measured on this
//     machine: `steam.desktop` is `Network;FileTransfer;Game`, PrismLauncher is
//     `Game;ActionGame;AdventureGame;Simulation;PackageManager`, the official
//     Minecraft launcher is `Game;Application`, and GOverlay -- a configuration
//     tool for an overlay -- is `Game`. The registry has `LauncherStore` and
//     `GameTool` for precisely this and they are refused below, but nothing on
//     this machine uses either, so the category alone lets every launcher in.
//   * **A wine service runs inside the game's environment.** `explorer.exe` draws
//     the wine desktop and has the game's `SteamAppId`, because Proton put it
//     there for the whole prefix.
//   * **gamescope is a Vulkan client** and, when Steam runs a game inside it, has
//     that game's `SteamAppId` too -- so the overlay would be drawn once by
//     gamescope and once by the game inside it, on top of each other.
//
// MangoHud carries a list of the same kind for the same reason and calls it a
// blacklist; there it *is* the policy, because MangoHud draws everywhere else.
// Here it only takes back what the detection wrongly gave. It is code rather than
// a default in the settings file for the same reason `is_own_process` is: there is
// no configuration in which the overlay belongs on the Steam client's own window,
// and a default that can be edited away is not a guarantee. `shown_apps` still
// overrides it, because the user has the last word about their own machine.
// Which of those names this is, or nullptr. The name is kept rather than thrown
// away: `launcher` on its own is a verdict without its evidence, and the whole
// point of the `why` field is that a refusal can be read.
inline const char* launcher_name(const std::string& name) {
    static const char* const names[] = {
        // Launchers whose own entry is in the games section.
        "steam", "steamwebhelper", "lutris", "heroic", "bottles", "prismlauncher",
        "minecraft-launcher", "goverlay",
        // The nested compositor, which would draw a second copy over the game's.
        "gamescope",
        // Wine's own, running under the game's Proton environment. The cut forms are
        // what the kernel gives for the long ones -- see detail::same_name -- and a
        // Proton process cannot fall back on its executable's name, which is the
        // wine loader for every game alike.
        "Steam.exe", "steamwebhelper.exe", "explorer.exe", "rundll32.exe", "iexplore.exe",
        "tabtip.exe",
    };
    for (const char* known : names) {
        if (detail::same_name(known, name)) {
            return known;
        }
    }
    return nullptr;
}

inline bool is_launcher(const std::string& name) { return launcher_name(name) != nullptr; }

// The name this process is known by, from /proc/self/comm. Cached: it cannot
// change under us in a way that matters, and this is called from a draw path.
inline const std::string& process_name() {
    static const std::string name = detail::read_first_line("/proc/self/comm");
    return name;
}

// The executable behind it. A Windows title under Proton is a wine process whose
// executable is the loader, so this is not always the interesting name -- which is
// why a rule matches either this or the one above.
inline const std::string& process_executable() {
    static const std::string path = [] {
        char buffer[4096] = {};
        const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string();
    }();
    return path;
}

// Walks a delimited list, calling `visit(from, to)` for every trimmed,
// non-empty entry until it returns false. One walker for the user's
// comma-separated lists and the desktop entries' semicolon categories: the
// same trim-and-split loop existed twice in this file, free to drift.
template <typename Visit>
inline void each_entry(const std::string& list, char delimiter, Visit visit) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(delimiter, start);
        if (end == std::string::npos) {
            end = list.size();
        }
        size_t from = start;
        size_t to = end;
        while (from < to && (list[from] == ' ' || list[from] == '\t')) ++from;
        while (to > from && (list[to - 1] == ' ' || list[to - 1] == '\t')) --to;
        if (to > from && !visit(from, to)) {
            return;
        }
        start = end + 1;
    }
}

// True when `name` appears in a comma-separated list. Whitespace around an entry
// is ignored, so a list edited by hand behaves the way it looks.
inline bool listed(const std::string& list, const std::string& name) {
    if (name.empty() || list.empty()) {
        return false;
    }
    bool found = false;
    each_entry(list, ',', [&](size_t from, size_t to) {
        if (list.compare(from, to - from, name) == 0) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

// The desktop entry this process was launched from, if the session said so.
//
// Two standard sources, in order of how much they can be trusted:
//
//   1. GIO_LAUNCHED_DESKTOP_FILE, which GLib's launcher sets to the path of the
//      entry it started. It also sets GIO_LAUNCHED_DESKTOP_FILE_PID, because the
//      variable is inherited by everything the application then starts -- so it is
//      only ours if that pid is ours.
//   2. The systemd unit this process is in. Desktop environments start
//      applications in their own scope named app[-<launcher>]-<id>[-<random>], so
//      the id is in /proc/self/cgroup -- see detail::desktop_ids_from_cgroup for
//      the shapes that line comes in. This one is inherited by children with no
//      pid to check against -- a game started by a launcher reports the launcher
//      -- so what comes out of here is a candidate, and it counts only once the
//      entry it names has been shown to point back at this executable.
//
// Cached: two callers ask for it (the verdict and the record), and answering costs
// a read of /proc and a handful of stats.
inline const std::string& launched_from_desktop_entry() {
    static const std::string reference = [] {
        if (const char* file = std::getenv("GIO_LAUNCHED_DESKTOP_FILE"); file && *file) {
            const char* pid = std::getenv("GIO_LAUNCHED_DESKTOP_FILE_PID");
            if (pid && std::atoi(pid) == static_cast<int>(::getpid())) {
                return std::string(file);
            }
        }
        // More than one reading of the unit name is possible; the one that names an
        // entry that exists is the one meant. When none of them does, the first is
        // still what gets reported, so `no-entry:` names something a person can go
        // and look for.
        const std::vector<std::string> candidates =
            detail::desktop_ids_from_cgroup(detail::read_first_line("/proc/self/cgroup"));
        for (const std::string& candidate : candidates) {
            if (!detail::find_desktop_entry(candidate).empty()) {
                return candidate;
            }
        }
        return candidates.empty() ? std::string() : candidates.front();
    }();
    return reference;
}

// Steam's app id, when this process has one that means anything.
//
// Steam puts it in the environment of everything it starts, and it is what finds
// the entry Steam writes for that game -- which names the game rather than naming
// Steam. But `0` is not an id: umu sets both variables from
// `STEAM_COMPAT_APP_ID`, which stays "0" unless the umu id ends in a number
// (`umu/umu_run.py`: `env["SteamAppId"] = env["STEAM_COMPAT_APP_ID"]`), and Heroic
// launches every game with `GAMEID=umu-0` (`src/backend/launcher.ts`). So every
// Heroic game arrived wearing `SteamAppId=0`: recorded as `steam:0`, counted as a
// Steam game by the window, and sent to look up its icon under
// `steam://rungameid/0`, which names nothing. The same line can carry a word --
// `umu-default` becomes `default` -- so what is accepted is what Steam issues:
// digits, not all of them zero.
inline std::string steam_app_id() {
    for (const char* name : {"SteamAppId", "SteamGameId"}) {
        const char* value = std::getenv(name);
        if (!value || !*value) {
            continue;
        }
        bool digits = true;
        bool zero = true;
        for (const char* character = value; *character; ++character) {
            digits = digits && *character >= '0' && *character <= '9';
            zero = zero && *character == '0';
        }
        if (digits && !zero) {
            return value;
        }
    }
    return {};
}

namespace detail {

// Whether a `Categories=` line puts this entry in the games section.
//
// Whole entries between the semicolons, never a substring: `Game` is a main
// category in the menu specification's registry, and every game subcategory in
// the additional registry -- `ActionGame`, `RolePlaying`, `Shooter`, `Simulation`,
// `Emulator` and the rest -- names `Game` as the category it is used with. So
// requiring the main one loses no real entry and stops anything that merely
// contains the four letters from counting.
//
// Two entries in the same registry mean the opposite of a game and are refused
// even when `Game` is beside them: `LauncherStore`, "a place to browse and install
// games or a launcher for a game or games", and `GameTool`, "companion apps for
// games, such as addon managers, remote play clients or other tools". That is what
// a store front and an addon manager are supposed to write about themselves. In
// practice almost none of them do -- which is what `is_launcher` is for -- but an
// entry that is honest about it should be believed.
//
// `Games`, in the plural, is not in the registry at all. It is what Discord writes
// into the entries it generates for the games it detects, and there is one of those
// on this machine, so it is read as `Game`.
inline bool categories_say_game(const std::string& categories) {
    bool game = false;
    bool refused = false;
    each_entry(categories, ';', [&](size_t from, size_t to) {
        const std::string entry = categories.substr(from, to - from);
        if (entry == "LauncherStore" || entry == "GameTool") {
            refused = true;
            return false;
        }
        if (entry == "Game" || entry == "Games") {
            game = true;
        }
        return true;
    });
    return game && !refused;
}

// Minecraft, read off the game's own arguments, because there is nowhere else.
//
// Every Minecraft client is a JVM. Measured here with a small probe that printed
// its own three files: `/proc/self/comm` is `java`, `/proc/self/exe` is
// `/usr/lib/jvm/java-17-openjdk/bin/java`, and the command line is the only one of
// the three that says which program this is. Neither of the first two can tell
// Minecraft from any other Java application, and the truncation to fifteen
// characters never even comes into it.
//
// Prism and the other MultiMC forks put the instance in the environment and are
// caught before this; Mojang's own launcher puts nothing there. What it does pass,
// like every other launcher, are the arguments the game itself parses:
// `--assetIndex` and `--gameDir`. Those come from the version manifest rather than
// from the launcher, so they survive Forge and Fabric -- whose own main class
// replaces `net.minecraft.client.main.Main`, which is why that name is not what is
// looked for here.
//
// **This one was read and not measured.** No Minecraft was launched on this
// machine; the argument list is the wiki's. That is why it takes two arguments
// together and not one, and why a Minecraft that still goes unrecognised says so
// in its record instead of leaving somebody guessing.
//
// `cmdline` is /proc/self/cmdline as it comes: arguments separated by NUL.
inline bool cmdline_says_minecraft(const std::string& cmdline) {
    bool asset_index = false;
    bool game_dir = false;
    size_t start = 0;
    while (start < cmdline.size()) {
        size_t end = cmdline.find('\0', start);
        if (end == std::string::npos) {
            end = cmdline.size();
        }
        const std::string argument = cmdline.substr(start, end - start);
        if (argument == "--assetIndex") {
            asset_index = true;
        } else if (argument == "--gameDir") {
            game_dir = true;
        }
        start = end + 1;
    }
    return asset_index && game_dir;
}

// The arguments this process was started with. Read once, off the present path,
// like everything else here. A command line can be long -- a Minecraft class path
// is thousands of characters -- so this reads to the end rather than a line.
inline std::string read_cmdline() {
    std::FILE* file = std::fopen("/proc/self/cmdline", "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, read);
    }
    std::fclose(file);
    return out;
}

}  // namespace detail

// What the detection concluded, and what it concluded it from.
struct Verdict {
    bool game = false;
    // A short token, written into the record and into the log. Positive: which
    // signal fired and what it said. Negative: which of them nearly fired, so that
    // a game the overlay stays out of is a case that can be read rather than a
    // mystery to be argued about.
    std::string reason;
};

// Whether this process is a game, from what only it can see: its own environment,
// its own arguments, and the desktop entry it was started from. Worked out once
// and never again -- a process does not become a game halfway through, and this
// gates resources that cannot be built mid-frame.
//
// **A launcher's id in the environment** is the first and best signal, because it
// is the launcher itself saying so about a process it started. Each of these was
// read out of the launcher's own source, and the record written for Overwatch on
// this machine carries `SteamAppId=2357570`. They are asked in the order below,
// which is the order of how much they say -- the ones that name the game first,
// then Steam's id, then the ones that only say a game is being run:
//
//   * `LUTRIS_GAME_UUID` -- Lutris. Not a guess this time: `lutris/game.py` reads
//     it back out of the running game's environment to find the game's processes,
//     which is only possible because it is in there.
//   * `HEROIC_APP_NAME` -- Heroic, beside `HEROIC_APP_RUNNER` and
//     `HEROIC_APP_SOURCE` (`src/backend/launcher.ts`).
//   * `INST_MC_DIR` -- Prism Launcher and the other MultiMC forks. `getVariables`
//     puts the instance's directories in, `createEnvironment` copies them into the
//     environment, and `createLaunchEnvironment` is what the game process is given
//     (`launcher/minecraft/MinecraftInstance.cpp`, `LauncherPartLaunch.cpp`) -- so
//     these reach the JVM itself and not only a pre-launch script.
//   * `SteamAppId` / `SteamGameId` -- Steam, for native and Proton games alike.
//     Also every umu game, since umu sets both itself (`umu/umu_run.py`), and umu
//     is what Lutris, Heroic and Bottles run Windows games through -- which is why
//     `steam_app_id()` refuses the `0` umu puts there when it has no id to put.
//   * `GAMEID` -- umu's own id, which it always sets, defaulting to `umu-default`
//     when the caller gives none (`docs/umu.1.scd`). Heroic sets it to `umu-0`.
//   * `ITCHIO_APP` -- the itch.io app, on everything it launches
//     (`butler`, `endpoints/launch/launch.go`: `env["ITCHIO_APP"] = "1"`). Read at
//     its documented value and nowhere measured on this machine: the itch app is
//     not installed here.
//   * `ENABLE_GAMESCOPE_WSI` -- gamescope sets it on everything it launches
//     (`src/steamcompmgr.cpp`), so a game inside the nested compositor is known
//     even when it came from nowhere else. Only at the value `1`: it is the WSI
//     layer's `enable_environment`, so `=0` is how somebody turns that layer off.
//
// `PROTONPATH` used to be in this list and is not any more. It names a Proton
// directory, which is not a statement that anything is a game, and it is the one
// variable here a person plausibly exports in their shell profile -- which would
// have made every OpenGL program in that session a game. Nothing is lost by
// dropping it: umu sets `GAMEID` and `SteamAppId` on top of it in every case.
//
// **The game's own arguments** come next, for the one case that has neither an
// environment nor an entry of its own: Minecraft under Mojang's launcher. See
// `cmdline_says_minecraft`.
//
// **The desktop entry** is last, and it is the signal that needs a guard. The
// categories are the freedesktop answer to what kind of application this is and
// they separate cleanly on this machine -- Spectacle is `Utility`, System Settings
// is `Settings`, Overwatch and Baldur's Gate 3 are `Game`, and the emulators are
// `Game;Emulator` -- which is also how the overlay tells `dolphin-emu` from KDE's
// `dolphin` without a single rule about either name. But the systemd scope the
// entry comes from is inherited: `glxgears` started from a terminal reported the
// terminal's application id, and the record for it on this machine still says
// `com.anthropic.Claude`. So the entry counts only when it names this executable,
// and a process that fails that check says `not-ours:` and which entry it was.
// "Names this executable" means any word of `Exec` or `TryExec` and not the first
// one alone -- see `detail::exec_names`, and the wrappers and quoted paths this
// machine's own entries are full of.
//
// Inside a Flatpak there is no guard to apply and none needed: `FLATPAK_ID` names
// the sandbox, the sandbox is the application, and everything running in it belongs
// to that application. Measured: a Flatpak's environment carries
// `FLATPAK_ID=sh.cider.Cider`, `XDG_DATA_DIRS` begins with `/app/share`, and the
// entry is where that says it is.
inline const Verdict& game_verdict() {
    static const Verdict answer = [] {
        const std::string binary = detail::basename_of(process_executable());
        const char* launcher = launcher_name(process_name());
        if (!launcher) {
            launcher = launcher_name(binary);
        }
        if (launcher) {
            return Verdict{false, std::string("launcher:") + launcher};
        }

        // What the launcher told the process it is, in order of how much it says.
        // The value is kept and not only tested for: it is what the record shows,
        // what the window puts on the row, and -- for Steam -- what the game's own
        // icon is looked up by.
        //
        // These name the game, so they are asked first. The order used to have
        // Steam's variables at the top, which is how every Heroic game came out as
        // `steam:0`: Heroic launches through umu, umu sets `SteamAppId` from an app
        // id it does not have, and `HEROIC_APP_NAME` was sitting in the same
        // environment saying which game it was.
        static const struct {
            const char* variable;
            const char* label;
        } names_the_game[] = {
            {"LUTRIS_GAME_UUID", "lutris"},
            {"HEROIC_APP_NAME", "heroic"},
            {"INST_MC_DIR", "minecraft"},
        };
        for (const auto& entry : names_the_game) {
            if (const char* value = std::getenv(entry.variable); value && *value) {
                return Verdict{true, std::string(entry.label) + ":" + value};
            }
        }

        if (const std::string steam = steam_app_id(); !steam.empty()) {
            return Verdict{true, "steam:" + steam};
        }

        // And these only say that a game is being run here, with a value that is
        // the same for every one of them: umu's id when it is the fallback, the
        // itch app's `ITCHIO_APP=1` (`butler`, `endpoints/launch/launch.go`:
        // `env["ITCHIO_APP"] = "1"`), and gamescope's own variable. The value is
        // not repeated into the reason, because it would say nothing.
        //
        // `ENABLE_GAMESCOPE_WSI` counts only when it is `1`. It is the layer's
        // `enable_environment` (`VkLayer_FROG_gamescope_wsi.json.in`), which the
        // loader documents as "must be set to the given value or else the implicit
        // layer is not loaded" -- so `ENABLE_GAMESCOPE_WSI=0` is what somebody
        // exports to keep that layer out, and reading it as a game turned every
        // process in such a session into one.
        static const struct {
            const char* variable;
            const char* value;
            const char* label;
        } says_a_game_runs[] = {
            {"GAMEID", nullptr, "umu"},
            {"ITCHIO_APP", "1", "itch"},
            {"ENABLE_GAMESCOPE_WSI", "1", "gamescope"},
        };
        for (const auto& entry : says_a_game_runs) {
            const char* value = std::getenv(entry.variable);
            if (!value || !*value) {
                continue;
            }
            if (entry.value && std::strcmp(value, entry.value) != 0) {
                continue;
            }
            return Verdict{true, entry.value ? std::string(entry.label)
                                             : std::string(entry.label) + ":" + value};
        }

        if (detail::cmdline_says_minecraft(detail::read_cmdline())) {
            return Verdict{true, "minecraft:arguments"};
        }

        // The entry, and how it was found. A Flatpak names its own; anything else
        // is a candidate that has to name this executable back. And when that
        // comes to nothing, the last resort: go and look for the entry that runs
        // this program. See `detail::entry_that_runs_this` for why that is last
        // and what keeps it from believing an interpreter.
        const auto searched = [&binary](const std::string& otherwise) {
            const std::string id = detail::entry_that_runs_this(binary, process_name());
            return id.empty() ? Verdict{false, otherwise} : Verdict{true, "entry:" + id};
        };

        std::string reference;
        bool guarded = true;
        if (const char* id = std::getenv("FLATPAK_ID"); id && *id) {
            reference = id;
            guarded = false;
        } else {
            reference = launched_from_desktop_entry();
        }
        if (reference.empty()) {
            return searched("nothing");
        }
        const std::string path = detail::find_desktop_entry(reference);
        const detail::Entry entry = path.empty() ? detail::Entry{} : detail::read_entry(path);
        // No entry, or a file that is not one: the file manager hands over
        // `GIO_LAUNCHED_DESKTOP_FILE` pointing at the executable itself for a
        // program that has no entry at all, and that is a different answer from
        // "somebody else's entry".
        if (!entry.found) {
            return searched("no-entry:" + reference);
        }

        if (guarded && !detail::exec_names(entry.exec, binary, process_name()) &&
            !detail::exec_names(entry.try_exec, binary, process_name())) {
            // The entry the session pointed at belongs to whoever started us --
            // which is exactly the shape a game launched from a terminal has, so
            // this is where looking for our own is worth the pass over the disk.
            return searched("not-ours:" + reference);
        }

        if (!detail::categories_say_game(entry.categories)) {
            return Verdict{false, "not-a-game:" + reference};
        }
        return Verdict{true, (guarded ? "desktop:" : "flatpak:") + reference};
    }();
    return answer;
}

inline bool looks_like_game() {
    return game_verdict().game;
}

// Whether the overlay draws in this process at all.
//
// A game by default and nothing else, which is the only policy that does not need
// a list of every application on the machine. The two lists are the deviations
// from that: something detected as a game that the user does not want it in, and
// something the detection cannot see is a game -- launched from a script, an
// emulator, a binary somebody downloaded -- that they do.
inline bool draw_here(const std::string& hidden, const std::string& shown) {
    if (is_own_process(process_name()) ||
        is_own_process(detail::basename_of(process_executable()))) {
        return false;
    }
    const std::string binary = detail::basename_of(process_executable());
    if (listed(hidden, process_name()) || listed(hidden, binary)) {
        return false;
    }
    if (listed(shown, process_name()) || listed(shown, binary)) {
        return true;
    }
    return looks_like_game();
}

inline std::string apps_directory() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/vocem/apps";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.cache/vocem/apps";
}

// Write one record. Defined below, beside the reader that is the only other place
// knowing the file's shape.
inline void write_application_record(const Application& application);

// Write down that the overlay was loaded here. Once per process, and expensive
// exactly once: /proc/self/comm, /proc/self/exe, /proc/self/cmdline, a desktop
// entry, a mkdir and a file written and renamed.
//
// The Vulkan layer calls it after the present has returned. The OpenGL side calls
// it from inside the swap hook, on the first presented frame, which is not "off
// the present path" however the sentence above used to read: it is one frame that
// costs more than the rest, in the frame where a game is still starting up.
//
// Recorded whether or not the overlay is allowed to draw here, because a process
// that has been excluded is exactly the one somebody may want to find in the list
// and let back in.
inline void record_application(const char* api) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    const std::string& name = process_name();
    if (name.empty() || is_own_process(name) ||
        is_own_process(detail::basename_of(process_executable()))) {
        return;
    }

    write_application_record(
        {name, process_executable(), api, launched_from_desktop_entry(), steam_app_id(), 0,
         game_verdict().game, game_verdict().reason});

    // Inside a Flatpak the file that was just written is in the sandbox's own
    // cache, where the window that exists to show it cannot reach: the same
    // record goes to the daemon across the bridge, and the daemon writes it on
    // the host's side. A no-op everywhere else.
    flatpak_bridge_record(name.c_str(), process_executable().c_str(), api,
                          game_verdict().game, game_verdict().reason.c_str());
}

// Write one record, for this process or -- when the daemon is doing it on behalf
// of a sandbox that cannot reach the host's cache -- for somebody else's.
//
// One spelling of the file's shape, because two would drift: the reader above is
// the only other place that knows it.
inline void write_application_record(const Application& application) {
    const std::string directory = apps_directory();
    // One mkdir -p for the whole project (vocem/paths.h) -- this used to be its
    // own loop at 0755 while every sibling created 0700, for no reason anybody
    // could name.
    make_directories(directory);

    const std::string path = directory + "/" + detail::sanitised(application.key);
    // The temporary carries the pid, and is created exclusively.
    //
    // The record's name is the process name, so two writers of one name are
    // ordinary rather than exotic: every Minecraft is `java` and every Chromium
    // GPU process -- including the ones inside Proton -- is `CrGpuMain`, and both
    // are in this machine's registry. A shared temporary name is two processes
    // writing one file, which is the thing rename() was there to prevent.
    //
    // O_EXCL|O_NOFOLLOW rather than fopen: this is a path in a directory anything
    // in the session can write to, and a symlink left at that name would send the
    // write elsewhere while a FIFO would block open() -- inside somebody else's
    // first frame, which is the one place this code must never wait.
    const std::string temporary = path + ".tmp." + std::to_string(::getpid());
    int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            0600);
    if (descriptor < 0) {
        // Ours, from a process with this pid that died before the rename.
        ::unlink(temporary.c_str());
        descriptor = ::open(temporary.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    if (descriptor < 0) {
        return;
    }
    std::FILE* file = ::fdopen(descriptor, "w");
    if (!file) {
        ::close(descriptor);
        ::unlink(temporary.c_str());
        return;
    }
    std::fprintf(file, "name = %s\n", application.key.c_str());
    std::fprintf(file, "executable = %s\n", application.executable.c_str());
    std::fprintf(file, "api = %s\n", application.api.c_str());
    std::fprintf(file, "game = %s\n", application.looks_like_game ? "true" : "false");
    // Beside the verdict, the evidence for it. A game the overlay stayed out of is
    // then a record somebody can read -- `not-ours:minecraft-launcher` says which
    // entry was found and why it was not believed -- rather than a switch that has
    // to be flipped without knowing what it is working around.
    std::fprintf(file, "why = %s\n", application.reason.c_str());
    if (!application.desktop.empty()) {
        std::fprintf(file, "desktop = %s\n", application.desktop.c_str());
    }
    if (!application.steam_app_id.empty()) {
        std::fprintf(file, "steam = %s\n", application.steam_app_id.c_str());
    }
    std::fprintf(file, "seen = %ld\n",
                 application.seen > 0 ? application.seen : static_cast<long>(std::time(nullptr)));
    std::fclose(file);
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
    }
}

// Everything written down so far, for the configuration window. Not for the
// injected code: it reads a directory and opens every file in it.
inline std::vector<Application> known_applications() {
    std::vector<Application> applications;
    const std::string directory = apps_directory();
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return applications;
    }
    while (const dirent* entry = ::readdir(handle)) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == "..") {
            continue;
        }
        // A record being written right now, which rename() is about to replace --
        // `<name>.tmp.<pid>`, and `<name>.tmp` from the versions that had one
        // temporary per name and could leave one behind.
        if (file_name.find(".tmp.") != std::string::npos ||
            (file_name.size() > 4 && file_name.compare(file_name.size() - 4, 4, ".tmp") == 0)) {
            continue;
        }

        // A record is a regular file the overlay wrote. Anything else here --
        // a symlink above all -- is not ours and is not opened. The journal
        // scanner grew this guard after it followed one and put its contents in
        // the window; this directory is the same tree, on the same four-second
        // tick from the same window, and never got it.
        const std::string path = directory + "/" + file_name;
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }

        std::FILE* file = std::fopen(path.c_str(), "r");
        if (!file) {
            continue;
        }
        Application application;
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            char* equals = std::strchr(line, '=');
            if (!equals) {
                continue;
            }
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            while (*key == ' ' || *key == '\t') ++key;
            for (char* end = key + std::strlen(key); end > key && (end[-1] == ' ' || end[-1] == '\t');
                 --end) {
                end[-1] = '\0';
            }
            while (*value == ' ' || *value == '\t') ++value;
            for (char* end = value + std::strlen(value);
                 end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ');
                 --end) {
                end[-1] = '\0';
            }
            if (std::strcmp(key, "name") == 0) {
                application.key = value;
            } else if (std::strcmp(key, "executable") == 0) {
                application.executable = value;
            } else if (std::strcmp(key, "api") == 0) {
                application.api = value;
            } else if (std::strcmp(key, "desktop") == 0) {
                application.desktop = value;
            } else if (std::strcmp(key, "steam") == 0) {
                application.steam_app_id = value;
            } else if (std::strcmp(key, "game") == 0) {
                application.looks_like_game = std::strcmp(value, "true") == 0;
            } else if (std::strcmp(key, "why") == 0) {
                application.reason = value;
            } else if (std::strcmp(key, "seen") == 0) {
                application.seen = std::atol(value);
            }
        }
        std::fclose(file);
        // Records written before our own processes were excluded, which the list
        // should not go on offering a switch for. And a key longer than the
        // kernel's fifteen characters, which no record of ours can have: what
        // reads this is the window, and the key is what a flipped switch writes
        // into the settings file, so a corrupt file is not given a row a
        // thousand characters wide to put there.
        if (!application.key.empty() && application.key.size() <= kCommLength &&
            !is_own_process(application.key)) {
            applications.push_back(application);
        }
    }
    ::closedir(handle);
    return applications;
}

// Empty the list. The window offers this because the list is a history: an
// application uninstalled a year ago has no business still being in it.
inline void forget_applications() {
    const std::string directory = apps_directory();
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return;
    }
    while (const dirent* entry = ::readdir(handle)) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == "..") {
            continue;
        }
        std::remove((directory + "/" + file_name).c_str());
    }
    ::closedir(handle);
}

}  // namespace vocem

#endif  // VOCEM_APPS_H
