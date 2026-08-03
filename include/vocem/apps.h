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
// at once would otherwise interleave into one corrupt line.
//
// Header-only and dependency-free, like the rest of what the injected code shares:
// the in-game side must not pull in a library to do this.

#ifndef VOCEM_APPS_H
#define VOCEM_APPS_H

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

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
// MangoHud carries a list of fifty-seven names for the same reason and calls it a
// blacklist; there it *is* the policy, because MangoHud draws everywhere else.
// Here it only takes back what the detection wrongly gave. It is code rather than
// a default in the settings file for the same reason `is_own_process` is: there is
// no configuration in which the overlay belongs on the Steam client's own window,
// and a default that can be edited away is not a guarantee. `shown_apps` still
// overrides it, because the user has the last word about their own machine.
inline bool is_launcher(const std::string& name) {
    static const char* const names[] = {
        // Launchers whose own entry is in the games section.
        "steam", "steamwebhelper", "lutris", "heroic", "bottles", "prismlauncher",
        "minecraft-launcher", "goverlay",
        // The nested compositor, which would draw a second copy over the game's.
        "gamescope",
        // Wine's own, running under the game's Proton environment.
        "Steam.exe", "steamwebhelper.exe", "explorer.exe", "rundll32.exe", "iexplore.exe",
        "tabtip.exe",
    };
    for (const char* known : names) {
        if (name == known) {
            return true;
        }
        // The kernel cuts /proc/self/comm to fifteen characters, and two of the
        // names above are longer: `minecraft-launcher` arrives as
        // `minecraft-launc` and `steamwebhelper.exe` as `steamwebhelper.`. The
        // executable's basename usually stands in for the full name, but not for a
        // Proton process, whose executable is the wine loader -- so the cut form has
        // to be compared as well, and only against a name long enough to have been
        // cut, so that no short name gains a prefix match it should not have.
        constexpr size_t comm_length = 15;
        if (std::strlen(known) > comm_length && name.size() == comm_length &&
            name.compare(0, comm_length, known, comm_length) == 0) {
            return true;
        }
    }
    return false;
}

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
//      the id is in /proc/self/cgroup. This one is inherited by children with no
//      pid to check against -- a game started by a launcher reports the launcher
//      -- so what comes out of here is a candidate, and it counts only once the
//      entry it names has been shown to point back at this executable.
inline std::string launched_from_desktop_entry() {
    if (const char* file = std::getenv("GIO_LAUNCHED_DESKTOP_FILE"); file && *file) {
        const char* pid = std::getenv("GIO_LAUNCHED_DESKTOP_FILE_PID");
        if (pid && std::atoi(pid) == static_cast<int>(::getpid())) {
            return file;
        }
    }

    const std::string cgroup = detail::read_first_line("/proc/self/cgroup");
    const size_t app = cgroup.rfind("/app-");
    if (app == std::string::npos) {
        return {};
    }
    std::string unit = cgroup.substr(app + 5);
    // app-<id>-<random>.scope, app-<id>@<random>.service, and the same with a
    // launcher in front of the id. What is wanted is everything before the random
    // part; which of the leading pieces is the id is for the reader to work out,
    // since a launcher's name and a reverse-DNS id both contain dashes.
    const size_t at = unit.find('@');
    if (at != std::string::npos) {
        unit = unit.substr(0, at);
    } else {
        const size_t last = unit.rfind('-');
        if (last != std::string::npos) {
            unit = unit.substr(0, last);
        }
    }
    const size_t dot = unit.rfind('.');
    if (dot != std::string::npos &&
        (unit.compare(dot, std::string::npos, ".scope") == 0 ||
         unit.compare(dot, std::string::npos, ".service") == 0)) {
        unit = unit.substr(0, dot);
    }
    return detail::unescaped_unit(unit);
}

// Steam puts the app id in the environment of everything it starts. Read here
// because here is where the environment is; what it is good for is finding the
// entry Steam itself writes for that game, which names it rather than naming
// Steam.
inline std::string steam_app_id() {
    for (const char* name : {"SteamAppId", "SteamGameId"}) {
        if (const char* value = std::getenv(name); value && *value) {
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

// The value of one key in the [Desktop Entry] group of a file, or an empty string.
inline std::string desktop_field(const std::string& path, const char* key) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return {};
    }
    std::string found;
    bool inside = false;
    char line[1024];
    const size_t key_length = std::strlen(key);
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (!text.empty() && text.front() == '[') {
            inside = text == "[Desktop Entry]";
            continue;
        }
        if (!inside || text.size() <= key_length || text.compare(0, key_length, key) != 0 ||
            text[key_length] != '=') {
            continue;
        }
        found = text.substr(key_length + 1);
        break;
    }
    std::fclose(file);
    return found;
}

// The entry with this id, looked for where the Base Directory Specification says
// entries live: the user's directory first, then the system ones.
inline std::string find_desktop_entry(const std::string& reference) {
    if (!reference.empty() && reference.front() == '/') {
        return reference;
    }
    const std::string file =
        reference.size() > 8 && reference.compare(reference.size() - 8, 8, ".desktop") == 0
            ? reference
            : reference + ".desktop";

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

    for (const std::string& root : roots) {
        const std::string path = root + "/applications/" + file;
        struct stat info {};
        if (::stat(path.c_str(), &info) == 0) {
            return path;
        }
    }
    return {};
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
// this machine carries `SteamAppId=2357570`:
//
//   * `SteamAppId` / `SteamGameId` -- Steam, for native and Proton games alike.
//     Also every umu game, since umu sets both itself (`umu/umu_run.py`), and umu
//     is what Lutris, Heroic and Bottles run Windows games through.
//   * `GAMEID` -- umu's own id, which it always sets, defaulting to `umu-default`
//     when the caller gives none (`docs/umu.1.scd`). Heroic sets it to `umu-0`.
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
//   * `ENABLE_GAMESCOPE_WSI` -- gamescope sets it on everything it launches
//     (`src/steamcompmgr.cpp`), so a game inside the nested compositor is known
//     even when it came from nowhere else.
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
//
// Inside a Flatpak there is no guard to apply and none needed: `FLATPAK_ID` names
// the sandbox, the sandbox is the application, and everything running in it belongs
// to that application. Measured: a Flatpak's environment carries
// `FLATPAK_ID=sh.cider.Cider`, `XDG_DATA_DIRS` begins with `/app/share`, and the
// entry is where that says it is.
inline const Verdict& game_verdict() {
    static const Verdict answer = [] {
        const std::string binary = detail::basename_of(process_executable());
        if (is_launcher(process_name()) || is_launcher(binary)) {
            return Verdict{false, "launcher"};
        }

        // What the launcher told the process it is. The value is kept and not only
        // tested for: it is what the record shows, what the window puts on the row,
        // and -- for Steam -- what the game's own icon is looked up by.
        static const struct {
            const char* variable;
            const char* label;
        } launchers[] = {
            {"SteamAppId", "steam"},        {"SteamGameId", "steam"},
            {"GAMEID", "umu"},              {"LUTRIS_GAME_UUID", "lutris"},
            {"HEROIC_APP_NAME", "heroic"},  {"INST_MC_DIR", "minecraft"},
            {"ENABLE_GAMESCOPE_WSI", "gamescope"},
        };
        for (const auto& launcher : launchers) {
            const char* value = std::getenv(launcher.variable);
            if (value && *value) {
                return Verdict{true, std::string(launcher.label) + ":" + value};
            }
        }

        if (detail::cmdline_says_minecraft(detail::read_cmdline())) {
            return Verdict{true, "minecraft:arguments"};
        }

        // The entry, and how it was found. A Flatpak names its own; anything else
        // is a candidate that has to name this executable back.
        std::string reference;
        bool guarded = true;
        if (const char* id = std::getenv("FLATPAK_ID"); id && *id) {
            reference = id;
            guarded = false;
        } else {
            reference = launched_from_desktop_entry();
        }
        if (reference.empty()) {
            return Verdict{false, "nothing"};
        }
        const std::string path = detail::find_desktop_entry(reference);
        if (path.empty()) {
            return Verdict{false, "no-entry:" + reference};
        }

        if (guarded) {
            const std::string try_exec = detail::desktop_field(path, "TryExec");
            std::string exec = detail::desktop_field(path, "Exec");
            const size_t space = exec.find(' ');
            if (space != std::string::npos) {
                exec = exec.substr(0, space);
            }
            const bool ours = (!binary.empty() && (detail::basename_of(try_exec) == binary ||
                                                   detail::basename_of(exec) == binary)) ||
                              detail::basename_of(try_exec) == process_name() ||
                              detail::basename_of(exec) == process_name();
            if (!ours) {
                return Verdict{false, "not-ours:" + reference};
            }
        }

        if (!detail::categories_say_game(detail::desktop_field(path, "Categories"))) {
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

    const std::string directory = apps_directory();
    // One mkdir -p for the whole project (vocem/paths.h) -- this used to be its
    // own loop at 0755 while every sibling created 0700, for no reason anybody
    // could name.
    make_directories(directory);

    const std::string path = directory + "/" + detail::sanitised(name);
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");
    if (!file) {
        return;
    }
    std::fprintf(file, "name = %s\n", name.c_str());
    std::fprintf(file, "executable = %s\n", process_executable().c_str());
    std::fprintf(file, "api = %s\n", api);
    std::fprintf(file, "game = %s\n", game_verdict().game ? "true" : "false");
    // Beside the verdict, the evidence for it. A game the overlay stayed out of is
    // then a record somebody can read -- `not-ours:minecraft-launcher` says which
    // entry was found and why it was not believed -- rather than a switch that has
    // to be flipped without knowing what it is working around.
    std::fprintf(file, "why = %s\n", game_verdict().reason.c_str());
    if (const std::string entry = launched_from_desktop_entry(); !entry.empty()) {
        std::fprintf(file, "desktop = %s\n", entry.c_str());
    }
    if (const std::string steam = steam_app_id(); !steam.empty()) {
        std::fprintf(file, "steam = %s\n", steam.c_str());
    }
    std::fprintf(file, "seen = %ld\n", static_cast<long>(std::time(nullptr)));
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
        // A record being written right now, which rename() is about to replace.
        if (file_name.size() > 4 && file_name.compare(file_name.size() - 4, 4, ".tmp") == 0) {
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
        // should not go on offering a switch for.
        if (!application.key.empty() && !is_own_process(application.key)) {
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
