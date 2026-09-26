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
// Two halves. This header is what the injected code calls at frame rate and
// once: the cached process name, the verdict, the per-frame `draw_here`, and the
// list helpers -- inline, allocation-free after the first call, dependency-free.
// common/src/apps.cpp (compiled into vocem_common) is what those call once and
// what the window and the daemon call to read and write the registry: the
// desktop-entry search, the record writer, the record reader. The reasoning
// behind each of those lives beside its definition there.

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

// The pieces the verdict is assembled from, defined in common/src/apps.cpp and
// reachable from the tests (tests/apps.cpp holds each to the cases measured in
// the field). Nothing here is asked at frame rate.
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
// (entries 95 and 122 are the shapes it covers).
std::vector<std::string> desktop_ids_from_cgroup(const std::string& cgroup);
// The XDG data roots, the user's first, in the Base Directory order.
std::vector<std::string> desktop_roots();
// The path of the entry with this id, or an absolute reference as it stands;
// dashes tried as directories left to right. Empty when no file has it.
std::string find_desktop_entry(const std::string& reference);

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
    // The three the settings window reads for the icon beside a row (Icon=,
    // StartupWMClass=, NoDisplay=/Hidden=). Read here so that there is one
    // parser of a desktop entry in the project: the window used to carry a
    // second one, whose "the program is the first word of Exec" rule was the
    // one entry 96 retired here (44 of this machine's 53 Game entries start
    // with a wrapper or an interpreter).
    std::string icon;
    std::string wm_class;
    bool no_display = false;
};

// The most a desktop entry may be. The largest on this machine, with every
// locale's Name and Comment in it, is under 100 KB; a megabyte is not one.
inline constexpr long kMaxEntryBytes = 1024 * 1024;

// The entry at this path, opened the way a file in somebody else's directory is
// opened (O_NOFOLLOW, O_NONBLOCK, regular files only, kMaxEntryBytes; entry 135).
Entry read_entry(const std::string& path);
// The words of an `Exec`/`TryExec` line that could be a program, each reduced
// to its file name: options, the specification's field codes and the
// `VAR=value` assignments `env` takes are left out, a quoted word is unquoted
// and never split. What exec_names() walks, and what the window indexes its
// entries by.
std::vector<std::string> exec_program_names(const std::string& exec);
// Whether an `Exec`/`TryExec` line names this program -- any word of it, not
// the first (entry 96).
bool exec_names(const std::string& exec, const std::string& binary, const std::string& comm);
// One argument as it is written into an `Exec=` line: quoted where the
// specification reserves a character in it (a space above all), its `%`
// doubled so it is not read as a field code. The window's autostart entry
// wrote the executable's path bare, which broke on a space or a `%` in it.
std::string desktop_exec_quoted(const std::string& argument);
// Whether a `Categories=` line puts this entry in the games section: whole
// entries, `Game` (or Discord's `Games`) required, `LauncherStore`/`GameTool`
// refused.
bool categories_say_game(const std::string& categories);
// The one installed entry that names this program, when exactly one does and it
// says Game; the id, or empty. One pass over the installed entries, ~2.5 ms.
std::string entry_that_runs_this(const std::string& binary, const std::string& comm);
// Minecraft under Mojang's launcher, read off `--assetIndex` + `--gameDir`.
bool cmdline_says_minecraft(const std::string& cmdline);
// /proc/self/cmdline whole, NUL-separated as it comes.
std::string read_cmdline();

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
// point of the `why` field is that a refusal can be read. (The long rationale
// above documents the LIST; this sentence is this function's own contract --
// the two ran together as one block once, and a reader had to reach this line
// to learn the block ends in a function returning a string.)
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

// The list with one entry taken out, rebuilt rather than cut: an entry can
// carry spaces around it, and removing "name" from " name , other" by index is
// how a list ends up with a stray comma in it. Every entry that is not `name`
// survives, trimmed. Lived in the window's bridge as a second tokeniser of the
// same list beside each_entry(); one walker now.
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

// Whether `name` can be an entry of these lists and read back as itself:
// not empty, no comma (the separator every reader splits on), and no space or
// tab at either end (every reader trims them away). A process name or an
// executable's may carry any byte but '/' and NUL, so this is not a question
// only a hand-edited file raises.
inline bool list_entry_fits(const std::string& name) {
    if (name.empty() || name.find(',') != std::string::npos) {
        return false;
    }
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    return !blank(name.front()) && !blank(name.back());
}

// The list with one entry added, once -- or the list as it was, for a name it
// cannot hold (list_entry_fits). It used to store any name, so "Foo, Bar" went
// in whole, read back as the two entries Foo and Bar, and hid two other
// applications while the one asked about stayed as it was. Refused rather than
// escaped: an escape would have to be read the same way by the overlay already
// inside every running game, at both widths, and by every older one installed.
inline std::string list_with(const std::string& list, const std::string& name) {
    if (!list_entry_fits(name) || listed(list, name)) {
        return list;
    }
    return list.empty() ? name : list + "," + name;
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
// `detail::cmdline_says_minecraft`.
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
//
// Not the per-frame ask itself: vocem/draw_decision.h calls this only when a
// list changed, and tests/apps_cost.cpp holds the steady state to zero
// allocations -- which this function, building two strings out of
// /proc/self/exe, would break if it were asked every frame.
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
// exactly once, whether or not the overlay is allowed to draw. The Vulkan layer
// calls it after the present has returned; the OpenGL side from the first
// presented frame. Inside a Flatpak the same record also crosses the bridge.
void record_application(const char* api);

// Write one record -- for this process, or for a sandboxed one the daemon is
// writing on behalf of. One spelling of the file's shape; the reader below is
// the only other place that knows it.
void write_application_record(const Application& application);

// Everything written down so far, for the configuration window. Not for the
// injected code: it reads a directory and opens every file in it.
std::vector<Application> known_applications();

// Empty the list. The window offers this because the list is a history: an
// application uninstalled a year ago has no business still being in it.
void forget_applications();

}  // namespace vocem

#endif  // VOCEM_APPS_H
