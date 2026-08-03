// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The application list, held to what actually comes out of a game.
//
// Matching a process name is the one decision the injected code makes about
// whether to draw at all, and it is made inside somebody else's process where
// nothing can be observed. A list edited by hand has spaces in it; a name from the
// kernel is truncated to fifteen characters; a name is a prefix of another name.
// Each of those is a way to draw in a game the user excluded, or to stay out of
// one they did not.

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "vocem/apps.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    using vocem::listed;

    // The plain cases.
    check(listed("steam,discord", "steam"), "the first entry matches");
    check(listed("steam,discord", "discord"), "the last entry matches");
    check(!listed("steam,discord", "steamwebhelper"), "an entry is not a prefix match");
    check(!listed("steamwebhelper", "steam"), "nor the other way round");
    check(!listed("", "steam"), "an empty list matches nothing");
    check(!listed("steam", ""), "an empty name matches nothing -- /proc/self/comm can fail");

    // A list somebody typed.
    check(listed("steam, discord , code", "discord"), "spaces around an entry are ignored");
    check(listed("steam,,discord", "discord"), "an empty entry does not end the search");

    // What the kernel gives us. TASK_COMM_LEN is 16 including the terminator, so a
    // long executable arrives cut to fifteen characters and the rule has to be
    // written against the cut name -- which is why the window shows that name.
    check(listed("verylongprocess", "verylongprocess"), "a name at the fifteen-character limit");

    // Never in our own processes, whatever the list says. The configuration
    // window is an OpenGL application and the shim is preloaded session-wide, so
    // without this the overlay drew itself into its own settings window.
    check(vocem::is_own_process("vocem-config"), "the configuration window is ours");
    check(vocem::is_own_process("vocemd"), "the daemon is ours");
    check(!vocem::is_own_process("vkcube"), "somebody else's process is not");
    check(!vocem::is_own_process("Overwatch.exe"), "nor is a game");

    // The policy: a game by default and nothing else, with the two lists as the
    // deviations from it. This process is not a game -- no launcher in its
    // environment, no desktop entry naming it -- which is what makes it a decent
    // stand-in for the screenshot tool that used to end up in the list.
    check(!vocem::looks_like_game(), "a test binary does not look like a game");
    check(!vocem::draw_here("", ""), "and so nothing is drawn in it");
    check(vocem::draw_here("", vocem::process_name()), "unless it is asked for by name");
    check(!vocem::draw_here(vocem::process_name(), vocem::process_name()),
          "and the exclusion still wins over the exception");

    ::setenv("SteamAppId", "2357570", 1);
    check(!vocem::looks_like_game(),
          "the answer is worked out once: a process does not become a game halfway through");
    ::unsetenv("SteamAppId");

    // And it says why, which is the whole point of keeping the evidence: a game the
    // overlay stays out of has to be a case somebody can read.
    check(!vocem::game_verdict().reason.empty(), "the verdict carries its evidence");

    // The categories, which is the freedesktop answer to what kind of application
    // this is. Whole entries between the semicolons and never a substring -- every
    // game subcategory in the registry names Game as the one it goes with, so
    // requiring the main category loses nothing and refuses everything else.
    using vocem::detail::categories_say_game;
    check(categories_say_game("Game;"), "the plain case");
    check(categories_say_game("Network;FileTransfer;Game;"), "Game anywhere in the list");
    check(categories_say_game("Game;Emulator;Qt;"),
          "an emulator -- which is how dolphin-emu is told from KDE's dolphin, with no "
          "rule about either name");
    check(!categories_say_game("Utility;"), "Spectacle is a utility");
    check(!categories_say_game("Settings;"), "System Settings is settings");
    check(!categories_say_game("Qt;KDE;System;TerminalEmulator;"),
          "a terminal is not a game, whatever was started from it");
    check(!categories_say_game(""), "an entry with no categories at all");
    check(!categories_say_game("Development;GameFramework;"),
          "a name that merely contains the four letters is not the category");
    check(categories_say_game("Discord;Games;"),
          "Games, in the plural, is not in the registry and is what Discord writes");
    check(!categories_say_game("Game;LauncherStore;"),
          "a store front says so about itself, and is believed");
    check(!categories_say_game("Game;GameTool;"), "so does a companion tool");
    check(categories_say_game(" Game ; Emulator "), "spaces around an entry are ignored");

    // The one signal that comes off the command line, because there is nowhere
    // else: a Minecraft client is a JVM, whose comm is `java` and whose executable
    // is the runtime. Two vanilla client arguments together and not one, because
    // this is the signal that was read rather than measured.
    using vocem::detail::cmdline_says_minecraft;
    const auto arguments = [](std::initializer_list<const char*> parts) {
        std::string out;
        for (const char* part : parts) {
            out.append(part);
            out.push_back('\0');
        }
        return out;
    };
    check(cmdline_says_minecraft(arguments({"java", "-cp", "...", "net.minecraft.client.main.Main",
                                            "--assetIndex", "17", "--gameDir",
                                            "/home/a/.minecraft"})),
          "the official launcher's JVM");
    check(cmdline_says_minecraft(arguments({"java", "cpw.mods.bootstraplauncher.BootstrapLauncher",
                                            "--gameDir", "/i", "--assetIndex", "17"})),
          "and a modded one, whose main class is somebody else's");
    check(!cmdline_says_minecraft(arguments({"java", "-cp", ".", "Probe"})),
          "an ordinary Java program is not Minecraft");
    check(!cmdline_says_minecraft(arguments({"tool", "--gameDir", "/somewhere"})),
          "nor is one argument on its own");
    check(!cmdline_says_minecraft(""), "nor an empty command line");
    check(!vocem::detail::read_cmdline().empty(), "this process can read its own arguments");

    // The name systemd puts in the cgroup, which is not the name of any file.
    // A dash separates the parts of a unit name, so a dash inside a name is escaped
    // -- and every application id with a dash in it was therefore looked for under a
    // spelling no entry has. Found in the field, not here: a record read
    // `no-entry:io.github.plrigaux.sysd\x2dmanager`.
    using vocem::detail::unescaped_unit;
    check(unescaped_unit("io.github.plrigaux.sysd\\x2dmanager") ==
              "io.github.plrigaux.sysd-manager",
          "a dash in an application id comes back a dash");
    check(unescaped_unit("minecraft\\x2dlauncher") == "minecraft-launcher",
          "and so does one in a plain name");
    check(unescaped_unit("org.kde.spectacle") == "org.kde.spectacle",
          "a name with nothing to unescape is untouched");
    check(unescaped_unit("a\\x2Db") == "a-b", "the digits may be upper case");
    check(unescaped_unit("trailing\\x2") == "trailing\\x2",
          "an escape cut short at the end is left as it stands");
    check(unescaped_unit("\\xzz") == "\\xzz", "and one that is not hexadecimal at all");
    check(unescaped_unit("") == "", "an empty name stays empty");

    // Things that carry a game's signals without being the game. In code rather
    // than in the settings file, for the same reason our own processes are: the
    // Steam client's entry says Game, and there is no configuration in which the
    // overlay belongs on it.
    check(vocem::is_launcher("steam"), "the Steam client");
    check(vocem::is_launcher("gamescope"), "the nested compositor, which would draw a second copy");
    check(vocem::is_launcher("explorer.exe"), "wine's desktop, running under the game's Proton");
    check(vocem::is_launcher("prismlauncher"), "a launcher whose own entry says Game");
    check(!vocem::is_launcher("Overwatch.exe"), "a game is not");
    // Both of the long names on that list arrive cut by the kernel, and one of them
    // is a Proton process whose executable is the wine loader and cannot stand in.
    check(vocem::is_launcher("minecraft-launc"),
          "minecraft-launcher, as fifteen characters of it");
    check(vocem::is_launcher("steamwebhelper."), "and steamwebhelper.exe, likewise");
    check(!vocem::is_launcher("steam-run"),
          "but a short name gains no prefix match from that");
    check(!vocem::is_launcher("steamwebhelper.e"),
          "nor does a name cut anywhere other than where the kernel cuts it");

    // The record a process writes of itself.
    const std::string directory =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/vocem-apps-test-" + std::to_string(::getpid());
    ::mkdir(directory.c_str(), 0700);
    ::setenv("XDG_CACHE_HOME", directory.c_str(), 1);

    check(vocem::known_applications().empty(), "nothing is known before anything has run");

    vocem::record_application("vulkan");
    const std::vector<vocem::Application> after = vocem::known_applications();
    check(after.size() == 1, "one record after one process");
    if (after.size() == 1) {
        check(after[0].key == vocem::process_name(), "recorded under the process's own name");
        check(after[0].api == "vulkan", "the api it was drawn with");
        check(after[0].executable == vocem::process_executable(), "and the executable behind it");
        check(after[0].seen > 0, "with a timestamp");
        check(after[0].looks_like_game == vocem::looks_like_game(), "the verdict it acted on");
        check(after[0].reason == vocem::game_verdict().reason,
              "and the evidence for it, so the window can show what it went on");
    }

    // Once per process, however many times it is called: this sits in a draw path.
    vocem::record_application("vulkan");
    check(vocem::known_applications().size() == 1, "recorded once, not once per frame");

    vocem::forget_applications();
    check(vocem::known_applications().empty(), "the list can be emptied");

    ::rmdir((directory + "/vocem/apps").c_str());
    ::rmdir((directory + "/vocem").c_str());
    ::rmdir(directory.c_str());

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
