// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which applications the overlay is loaded into, and which of them it must leave
// alone.
//
// Nothing is scanned for: the overlay is already inside every Vulkan and OpenGL
// process, so each process writes down once that it was here, and the
// configuration window reads the result.
//
// One small file per application under $XDG_CACHE_HOME/vocem/apps, written by
// temporary-and-rename so a half-written file is never read. One file each,
// because the writers are other people's processes, concurrent and unlocked.
//
// This header is what the injected code calls per frame or once (the cached
// process name, the verdict, draw_here, the list helpers: inline and
// allocation-free after the first call). common/src/apps.cpp is what those call
// once, plus the registry's writer and reader.

#ifndef VOCEM_APPS_H
#define VOCEM_APPS_H

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vocem {

// What one application is known by.
struct Application {
    // The key a rule is written against, and the file's name: the process name,
    // which is what /proc/self/comm reports.
    std::string key;
    // The executable behind it, in full: comm is cut to fifteen characters by the
    // kernel, too short to tell two long names apart or to show anybody.
    std::string executable;
    // "vulkan" or "opengl": which of the two paths drew here.
    std::string api;
    // The desktop entry this process was launched from, when the session said so:
    // the file's path or its id.
    std::string desktop;
    // Steam's app id from the process's environment: the number the game was
    // started with, which Steam's own desktop entries name.
    std::string steam_app_id;
    // When it was last seen, as a unix timestamp.
    long seen = 0;
    // The detection's verdict, which decides whether the overlay draws unless the
    // user's lists say otherwise.
    bool looks_like_game = false;
    // The signal that decided it: `steam:2357570`, `desktop:Overwatch.desktop`,
    // `not-ours:minecraft-launcher`, `nothing`. Kept beside the verdict so a game
    // the detection misses stays a case somebody can explain.
    std::string reason;
};

// The pieces the verdict is assembled from, defined in common/src/apps.cpp and
// reachable from the tests (tests/apps.cpp holds each to real-world cases).
// Nothing here is asked at frame rate.
namespace detail {

// The first line of a file, without its line ending; empty when it cannot be read.
std::string read_first_line(const char* path);
// What follows the last slash.
std::string basename_of(const std::string& path);
// A process name made safe to be a file name: [A-Za-z0-9._-] kept, anything
// else an underscore, `.`/`..`/empty replaced by `unknown`.
std::string sanitised(const std::string& name);
// A systemd unit name with its `\xNN` escaping taken back out (entry 32).
std::string unescaped_unit(const std::string& name);
// Two process names that are the same name, allowing for the kernel's
// fifteen-character cut of one of them.
bool same_name(const char* candidate, const std::string& name);
// The application ids a `/proc/self/cgroup` line can be read as, best first
// (entries 95 and 122).
std::vector<std::string> desktop_ids_from_cgroup(const std::string& cgroup);
// The XDG data roots, the user's first, in the Base Directory order.
std::vector<std::string> desktop_roots();
// The path of the entry with this id, or an absolute reference as it stands;
// dashes tried as directories left to right. Empty when no file has it.
std::string find_desktop_entry(const std::string& reference);

// The fields of a desktop entry the verdict and the window turn on, read in one
// pass. `found` means a [Desktop Entry] group was there, not that the file
// opened: GIO_LAUNCHED_DESKTOP_FILE can name the executable itself (a program
// started from the file manager), and that is "no entry", not `not-ours:`.
struct Entry {
    bool found = false;
    std::string exec;
    std::string try_exec;
    std::string categories;
    // Read for the settings window's icon beside a row (Icon=, StartupWMClass=,
    // NoDisplay=/Hidden=), so the project has one desktop-entry parser.
    std::string icon;
    std::string wm_class;
    bool no_display = false;
};

// The most a desktop entry may be: real ones, with every locale's Name and
// Comment, stay under 100 KB.
inline constexpr long kMaxEntryBytes = 1024 * 1024;

// The entry at this path, opened with the care a file in somebody else's
// directory needs (see the definition).
Entry read_entry(const std::string& path);
// The words of an `Exec`/`TryExec` line that could be a program, each reduced
// to its file name (see the definition for what is left out). What exec_names()
// walks, and what the window indexes its entries by.
std::vector<std::string> exec_program_names(const std::string& exec);
// Whether an `Exec`/`TryExec` line names this program -- any word of it, not
// the first (entry 96).
bool exec_names(const std::string& exec, const std::string& binary, const std::string& comm);
// One argument as it is written into an `Exec=` line: quoted where the
// specification reserves a character in it, its `%` doubled so it is not read
// as a field code.
std::string desktop_exec_quoted(const std::string& argument);
// Whether a `Categories=` line puts this entry in the games section: whole
// entries, `Game` (or Discord's `Games`) required, `LauncherStore`/`GameTool`
// refused.
bool categories_say_game(const std::string& categories);
// The one installed entry that names this program, when exactly one does and it
// says Game; the id, or empty. One pass over the installed entries.
std::string entry_that_runs_this(const std::string& binary, const std::string& comm);
// Minecraft under Mojang's launcher, read off `--assetIndex` + `--gameDir`.
bool cmdline_says_minecraft(const std::string& cmdline);
// /proc/self/cmdline whole, NUL-separated as it comes.
std::string read_cmdline();

}  // namespace detail

// Ours, and never to be drawn in: the shim is preloaded session-wide, so without
// this the overlay loads into its own settings window and paints a voice panel
// over it. Not a setting: no configuration wants it.
inline bool is_own_process(const std::string& name) {
    return name == "vocem-config" || name == "vocemd" || name == "vocem" ||
           name == "vocem-run";
}

// Things that carry a game's signals without being the game -- a correction to
// the detection below, not a policy of its own:
//
//   * launchers' own entries say `Categories=Game` (steam, PrismLauncher, the
//     Minecraft launcher, GOverlay), and none uses `LauncherStore`/`GameTool`;
//   * wine services such as `explorer.exe` carry the game's `SteamAppId`, which
//     Proton sets for the whole prefix;
//   * gamescope is a Vulkan client with the game's `SteamAppId`, and would draw
//     a second overlay over the game's.
//
// Code rather than an editable default, like is_own_process; `shown_apps` still
// overrides it.
//
// launcher_name() returns which name matched, or nullptr: the name goes into the
// `why` field so a refusal can be read.
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

// The executable behind it. Under Proton it is the wine loader for every game,
// which is why a rule matches either this or the process name.
inline const std::string& process_executable() {
    static const std::string path = [] {
        char buffer[4096] = {};
        const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string();
    }();
    return path;
}

// Walks a delimited list, calling `visit(from, to)` for every trimmed,
// non-empty entry until it returns false. The one walker for the user's
// comma-separated lists and the desktop entries' semicolon categories.
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

// The list with one entry taken out, rebuilt rather than cut: removing "name"
// from " name , other" by index leaves a stray comma. Every entry that is not
// `name` survives, trimmed.
inline std::string list_without(const std::string& list, const std::string& name) {
    std::string rebuilt;
    each_entry(list, ',', [&](size_t from, size_t to) {
        if (list.compare(from, to - from, name) != 0) {
            if (!rebuilt.empty()) {
                rebuilt.push_back(',');
            }
            rebuilt.append(list, from, to - from);
        }
        return true;
    });
    return rebuilt;
}

// Whether `name` can be an entry of these lists and read back as itself: not
// empty, no comma, no space or tab at either end. Process and executable names
// may carry any byte but '/' and NUL.
inline bool list_entry_fits(const std::string& name) {
    if (name.empty() || name.find(',') != std::string::npos) {
        return false;
    }
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    return !blank(name.front()) && !blank(name.back());
}

// The list with one entry added once, or the list unchanged for a name it cannot
// hold (list_entry_fits): "Foo, Bar" would read back as two other entries
// (entry 229). Refused rather than escaped, because every overlay already
// installed or running, at both widths, would have to read the escape.
inline std::string list_with(const std::string& list, const std::string& name) {
    if (!list_entry_fits(name) || listed(list, name)) {
        return list;
    }
    return list.empty() ? name : list + "," + name;
}

// The desktop entry this process was launched from, if the session said so:
// GIO_LAUNCHED_DESKTOP_FILE when GIO_LAUNCHED_DESKTOP_FILE_PID is our pid (the
// variable is inherited by everything the application starts), else the id in
// the systemd unit of /proc/self/cgroup. The unit is inherited too, with no pid
// to check, so that answer is only a candidate until the entry is shown to
// name this executable. Cached: the verdict and the record both ask.
inline const std::string& launched_from_desktop_entry() {
    static const std::string reference = [] {
        if (const char* file = std::getenv("GIO_LAUNCHED_DESKTOP_FILE"); file && *file) {
            const char* pid = std::getenv("GIO_LAUNCHED_DESKTOP_FILE_PID");
            if (pid && std::atoi(pid) == static_cast<int>(::getpid())) {
                return std::string(file);
            }
        }
        // The first reading that names an existing entry; failing that, the
        // first reading, so `no-entry:` names something a person can look for.
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

// Steam's app id, when this process has one that means anything: digits, not
// all zero. umu sets `SteamAppId` from `STEAM_COMPAT_APP_ID`, which is "0" or a
// word such as `default` when there is no id (Heroic launches every game with
// `GAMEID=umu-0`), and `steam:0` names no game (entry 97).
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

// What the detection concluded, and what it concluded it from.
struct Verdict {
    bool game = false;
    // A short token, written into the record and into the log. Positive: which
    // signal fired and what it said. Negative: which of them nearly fired, so that
    // a game the overlay stays out of is a case that can be read rather than a
    // mystery to be argued about.
    std::string reason;
};

// Whether this process is a game, from what only it can see: its environment,
// its arguments, and the desktop entry it was started from. Worked out once: a
// process does not become a game halfway through, and this gates resources that
// cannot be built mid-frame. The signals, best first:
//
//   1. A launcher's variable, each read from that launcher's own source. First
//      the ones that name the game (`LUTRIS_GAME_UUID`, `HEROIC_APP_NAME`,
//      `INST_MC_DIR` from Prism and the MultiMC forks), then
//      `SteamAppId`/`SteamGameId` (Steam, and every umu game), then the ones
//      that only say a game runs (`GAMEID` from umu, `ITCHIO_APP=1`,
//      `ENABLE_GAMESCOPE_WSI=1`). `PROTONPATH` is not one: it names a
//      directory, and is plausibly exported in a shell profile.
//   2. The game's own arguments, for Minecraft under Mojang's launcher
//      (detail::cmdline_says_minecraft).
//   3. The desktop entry's categories. The systemd scope it comes from is
//      inherited (a program started from a terminal reports the terminal's
//      entry), so the entry counts only when a word of its Exec/TryExec names
//      this executable (detail::exec_names), else `not-ours:`. Inside a
//      Flatpak, `FLATPAK_ID` names the application and needs no guard.
//   4. Last, the one installed game entry that runs this program
//      (detail::entry_that_runs_this).
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

        // What the launcher told the process it is. The value is kept: the record
        // shows it, the window puts it on the row, and Steam's icon is looked up
        // by it. These name the game, so they come before Steam's id, which umu
        // sets even when it has none.
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

        // These only say that a game runs here, with the same value for every
        // game, so a fixed value is not repeated into the reason.
        // `ENABLE_GAMESCOPE_WSI` counts only at `1`: it is the gamescope WSI
        // layer's `enable_environment`, so `=0` is how that layer is kept out.
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
        // is a candidate that has to name this executable back. When that comes
        // to nothing, look for the entry that runs this program.
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

// Whether the overlay draws in this process at all: in a game by default and
// nothing else; the hidden and shown lists are the user's deviations.
//
// Not the per-frame ask itself: vocem/draw_decision.h calls this only when a
// list changed, and tests/apps_cost.cpp holds the steady state to zero
// allocations, which this function (two strings out of /proc/self/exe) breaks.
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

// --- the registry, in common/src/apps.cpp ---------------------------------

// Write down that the overlay was loaded here: once per process, expensive
// exactly once (/proc reads, a desktop entry, a file written and renamed), and
// whether or not the overlay may draw, since an excluded process is the one
// somebody may want to find and let back in. The Vulkan layer calls it after
// the present has returned; the OpenGL side from the swap hook on the first
// presented frame, which makes that one frame cost more. Inside a Flatpak the
// same record also crosses the bridge.
void record_application(const char* api);

// Write one record, for this process or for a sandbox the daemon writes on
// behalf of. The one writer of the file's shape; the reader below is the only
// other place that knows it.
void write_application_record(const Application& application);
// The same record under a file name the caller chooses. The daemon's bridge
// writes a sandbox's record under a name made from its application id, which no
// host record can have, so a sandbox cannot overwrite a host application's
// record by claiming its process name. `file_name` is one path component;
// anything else is refused and nothing is written.
void write_application_record(const Application& application, const std::string& file_name);

// Everything written down so far, for the configuration window. Not for the
// injected code: it reads a directory and opens every file in it.
std::vector<Application> known_applications();

// Empty the list. The window offers this because the list is a history: an
// application uninstalled a year ago has no business still being in it.
void forget_applications();

}  // namespace vocem

#endif  // VOCEM_APPS_H
