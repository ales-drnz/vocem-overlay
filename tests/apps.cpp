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
#include <utility>

#include <sys/stat.h>
#include <sys/wait.h>
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

// The verdict a process would reach with these variables set, asked in a child.
//
// It has to be a child: the verdict is worked out once per process and cached
// there on purpose, so the first check in this file would decide it for all the
// rest. Everything a launcher might have left in this test's own environment is
// cleared first, so what comes back is the answer to the case and not to the
// shell this was started from.
std::string verdict_with(std::initializer_list<std::pair<const char*, const char*>> variables) {
    int channel[2] = {-1, -1};
    if (::pipe(channel) != 0) {
        return "<no pipe>";
    }
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(channel[0]);
        for (const char* name : {"SteamAppId", "SteamGameId", "GAMEID", "LUTRIS_GAME_UUID",
                                 "HEROIC_APP_NAME", "INST_MC_DIR", "ITCHIO_APP",
                                 "ENABLE_GAMESCOPE_WSI", "FLATPAK_ID"}) {
            ::unsetenv(name);
        }
        for (const auto& variable : variables) {
            ::setenv(variable.first, variable.second, 1);
        }
        const std::string answer = std::string(vocem::looks_like_game() ? "game " : "no ") +
                                   vocem::game_verdict().reason;
        (void)!::write(channel[1], answer.c_str(), answer.size());
        ::close(channel[1]);
        ::_exit(0);
    }
    ::close(channel[1]);
    std::string answer;
    char buffer[512];
    ssize_t read = 0;
    while ((read = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        answer.append(buffer, static_cast<size_t>(read));
    }
    ::close(channel[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    return answer;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

}  // namespace

int main() {
    using vocem::listed;

    // What a launcher's variables make of a process, each asked in its own child.
    //
    // Two of these were wrong in a way nothing here could have caught, because
    // they are about which variable is believed rather than about how one is
    // parsed. Heroic launches every game through umu with `GAMEID=umu-0`, umu sets
    // `SteamAppId` from an app id it does not have, and Steam's variable used to
    // be read first -- so every Heroic game was recorded as `steam:0`, a number
    // that names no game and looks up no icon, while `HEROIC_APP_NAME` sat in the
    // same environment saying which game it was.
    check(verdict_with({{"HEROIC_APP_NAME", "Cyberpunk2077"}, {"GAMEID", "umu-0"},
                        {"SteamAppId", "0"}, {"SteamGameId", "0"}}) == "game heroic:Cyberpunk2077",
          "a Heroic game is a Heroic game, not Steam app number zero");
    check(verdict_with({{"SteamAppId", "2357570"}}) == "game steam:2357570",
          "and a real Steam id is still the answer");
    check(verdict_with({{"LUTRIS_GAME_UUID", "b6f4"}, {"SteamAppId", "0"}}) == "game lutris:b6f4",
          "so is Lutris's uuid over the same placeholder");
    check(!starts_with(verdict_with({{"SteamAppId", "0"}}), "game steam:"),
          "SteamAppId=0 on its own is not a Steam game either");
    // gamescope's variable is the WSI layer's `enable_environment`, which the
    // Vulkan loader documents as "must be set to the given value or else the
    // implicit layer is not loaded" -- so `=0` is what somebody exports to keep
    // that layer out of their session, and reading it as a game turned every
    // process in that session into one.
    check(verdict_with({{"ENABLE_GAMESCOPE_WSI", "1"}}) == "game gamescope",
          "inside gamescope, which says nothing more than that");
    check(!starts_with(verdict_with({{"ENABLE_GAMESCOPE_WSI", "0"}}), "game"),
          "and switched off, which is not a game and never was");
    check(verdict_with({{"ITCHIO_APP", "1"}}) == "game itch",
          "the itch.io app, which sets it on everything it launches");
    check(verdict_with({{"GAMEID", "umu-default"}}) == "game umu:umu-default",
          "umu's own id, kept whole because it is what names the game to protonfixes");

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

    // The name systemd puts in the cgroup is not always the application id, and
    // more than one reading of it is possible. Every line below was read off a
    // live process on this machine, except the two GNOME ones, which are the
    // shapes systemd's own DESKTOP_ENVIRONMENTS.md documents
    // (`app[-<launcher>]-<ApplicationID>[@<RANDOM>].service`).
    using vocem::detail::desktop_ids_from_cgroup;
    const auto reads_as = [](const std::string& cgroup, const char* id) {
        for (const std::string& candidate : desktop_ids_from_cgroup(cgroup)) {
            if (candidate == id) {
                return true;
            }
        }
        return false;
    };
    const std::string user = "0::/user.slice/user-1000.slice/user@1000.service/";
    check(desktop_ids_from_cgroup(user + "app.slice/app-com.anthropic.Claude-5522.scope").front() ==
              "com.anthropic.Claude",
          "a plain scope: the random part comes off and the id is the first reading");
    check(desktop_ids_from_cgroup(user + "app.slice/app-discord-2909.scope").front() == "discord",
          "and one whose id is a plain name");
    check(desktop_ids_from_cgroup(user + "app.slice/app-arch-update-tray@autostart.service")
                  .front() == "arch-update-tray",
          "a service keeps everything before the @, dashes and all");
    check(desktop_ids_from_cgroup(user + "app.slice/app-steam@09756e45.service").front() == "steam",
          "the Steam client's own unit");
    // Found in the field: every D-Bus-activated application reported a name no
    // file has, and said `no-entry:` about it. Telegram's, and the logout
    // greeter's, verbatim.
    check(reads_as(user +
                       "app.slice/app-dbus-:1.2-org.telegram.desktop.slice/"
                       "dbus-:1.2-org.telegram.desktop@0.service",
                   "org.telegram.desktop"),
          "a D-Bus-activated application is reachable behind its slice");
    check(reads_as(user +
                       "app.slice/app-dbus-:1.2-org.kde.LogoutPrompt.slice/"
                       "dbus-:1.2-org.kde.LogoutPrompt@0.service",
                   "org.kde.LogoutPrompt"),
          "and so is the next one");
    // The random part is optional on a service (systemd spells it
    // `app[-<launcher>]-<ApplicationID>[@<RANDOM>].service`): a unit without it
    // used to lose everything after the id's first dash to the scope rule, and
    // the candidate list for the first of these read ["app"].
    check(reads_as(user + "app.slice/app-org.gnome.Evince.service", "org.gnome.Evince"),
          "a service with no random part keeps its whole id");
    check(reads_as(user + "app.slice/app-gnome-org.gnome.Evince.service", "org.gnome.Evince"),
          "with a launcher in front of it too");
    check(reads_as(user + "app.slice/app-gnome-org.gnome.Evince-12345.scope", "org.gnome.Evince"),
          "the launcher in the middle of the name comes off: GNOME's shape");
    check(reads_as(user + "app.slice/app-gnome-org.gnome.Evince@12345.service", "org.gnome.Evince"),
          "in its service spelling too");
    check(reads_as(user + "app.slice/app-flatpak-com.example.Game-1234.scope", "com.example.Game"),
          "and flatpak's");
    check(reads_as(user + "app.slice/app-io.github.plrigaux.sysd\\x2dmanager-1234.scope",
                   "io.github.plrigaux.sysd-manager"),
          "with the escaping taken back out, as before");
    check(desktop_ids_from_cgroup("0::/user.slice/user-1000.slice/session.slice/foo.service")
              .empty(),
          "a process in no application scope reads as nothing at all");
    check(desktop_ids_from_cgroup("").empty(), "and so does an unreadable cgroup");

    // The guard that stops a terminal's game reporting the terminal, held to the
    // entries this machine actually has. The first word of Exec is not the
    // program often enough to matter: it is quoted, or it is env, or sh, or
    // mangohud, or a wrapper script beside the binary.
    using vocem::detail::exec_names;
    check(exec_names("konsole", "glxgears", "glxgears") == false,
          "the case the guard exists for: a terminal's entry does not name the game in it");
    check(exec_names("\"/usr/lib/REAPER/reaper\" %F", "reaper", "reaper"),
          "a quoted path, which the first-word reading matched with a quote on the end");
    check(exec_names("/usr/bin/mangohud /usr/bin/steam %U", "steam", "steam"),
          "the Steam entry as this machine has it, behind mangohud");
    check(exec_names("env \"WINEPREFIX=/home/a/.wine\" wine start foo.exe %f", "wine", "wine"),
          "env, with an assignment in front of the command");
    check(!exec_names("env GAME=/usr/bin/quake quake2", "quake", "quake"),
          "and the assignment is not itself read as the program");
    check(exec_names("minecraft-launcher.sh", "minecraft-launcher", "minecraft-launc"),
          "a wrapper script beside its binary: the two agree as far as the kernel keeps the name");
    check(!exec_names("", "glxgears", "glxgears"), "an entry with no Exec names nobody");
    check(!exec_names("%U -f --long", "glxgears", "glxgears"),
          "field codes and options are not programs");

    // A desktop entry, and something that is not one. GIO_LAUNCHED_DESKTOP_FILE
    // arrives pointing at the executable itself for a program launched from the
    // file manager with no entry of its own -- read for `Exec=` it says nothing,
    // which is "no entry", not "somebody else's entry".
    using vocem::detail::read_entry;
    const std::string scratch = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                                "/vocem-entry-test-" + std::to_string(::getpid()) + ".desktop";
    if (std::FILE* written = std::fopen(scratch.c_str(), "w")) {
        std::fprintf(written,
                     "[Desktop Entry]\nType=Application\nName=Probe\n"
                     "TryExec=probe\nExec=env A=b probe %%U\nCategories=Game;ActionGame;\n"
                     "[Desktop Action New]\nExec=somebody-else\n");
        std::fclose(written);
    }
    const vocem::detail::Entry entry = read_entry(scratch);
    check(entry.found, "a desktop entry is recognised as one");
    check(entry.exec == "env A=b probe %U", "its Exec comes out whole");
    check(entry.try_exec == "probe", "and its TryExec");
    check(entry.categories == "Game;ActionGame;", "and its categories");
    check(exec_names(entry.exec, "probe", "probe"), "which together name the program");
    check(!read_entry("/bin/sh").found, "an executable is not a desktop entry");
    check(!read_entry(scratch + "-absent").found, "and neither is a file that is not there");
    std::remove(scratch.c_str());

    // The entry that runs this program, which is the one signal nobody hands
    // over: it is looked for, and it is what a game started from a terminal or a
    // file manager has instead of nothing. The rule that keeps it honest is that
    // exactly one entry may name the program -- measured, "every entry that names
    // it is a game" was true of the name `steam` itself, because the thirty
    // per-game entries Steam writes all read `Exec=steam steam://rungameid/…`.
    const std::string data = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                             "/vocem-entries-test-" + std::to_string(::getpid());
    const std::string applications = data + "/applications";
    ::mkdir(data.c_str(), 0700);
    ::mkdir(applications.c_str(), 0700);
    ::mkdir((applications + "/sub").c_str(), 0700);
    const auto write_entry = [](const std::string& path, const char* exec, const char* categories) {
        if (std::FILE* file = std::fopen(path.c_str(), "w")) {
            std::fprintf(file, "[Desktop Entry]\nType=Application\nName=X\nExec=%s\nCategories=%s\n",
                         exec, categories);
            std::fclose(file);
        }
    };
    write_entry(applications + "/alone.desktop", "/opt/alone/alone %U", "Game;ActionGame;");
    write_entry(applications + "/sub/nested.desktop", "nestedgame", "Game;");
    write_entry(applications + "/one.desktop", "python3 /usr/share/one/main.py", "Game;");
    write_entry(applications + "/two.desktop", "python3 /usr/share/two/main.py", "Utility;");
    write_entry(applications + "/tool.desktop", "/usr/bin/toolish", "Utility;");
    ::setenv("XDG_DATA_HOME", data.c_str(), 1);
    ::setenv("XDG_DATA_DIRS", (data + "-absent").c_str(), 1);

    using vocem::detail::entry_that_runs_this;
    check(entry_that_runs_this("alone", "alone") == "alone.desktop",
          "one entry names it and calls it a game, which is the whole case for looking");
    check(entry_that_runs_this("nestedgame", "nestedgame") == "sub-nested.desktop",
          "and an entry in a subdirectory is found under the id its path makes");
    check(entry_that_runs_this("python3", "python3").empty(),
          "an interpreter is named by more than one entry and settles nothing");
    check(entry_that_runs_this("toolish", "toolish").empty(),
          "the one entry that names this one does not call it a game");
    check(entry_that_runs_this("nobody", "nobody").empty(), "and nothing names this one at all");
    check(entry_that_runs_this("", "").empty(), "with no name to go on there is nothing to find");

    // The id of an entry is its path with the separators turned into dashes, so
    // going back the other way has to try both readings. Wine installs the
    // entries for the programs in a prefix under `applications/wine/Programs/`.
    using vocem::detail::find_desktop_entry;
    check(find_desktop_entry("sub-nested.desktop") == applications + "/sub/nested.desktop",
          "a dash in an id can be a directory");
    check(find_desktop_entry("alone.desktop") == applications + "/alone.desktop",
          "and the flat name is still tried first");
    check(find_desktop_entry("absent-entry").empty(), "an id with no file behind it finds nothing");

    std::remove((applications + "/alone.desktop").c_str());
    std::remove((applications + "/sub/nested.desktop").c_str());
    std::remove((applications + "/one.desktop").c_str());
    std::remove((applications + "/two.desktop").c_str());
    std::remove((applications + "/tool.desktop").c_str());
    ::rmdir((applications + "/sub").c_str());
    ::rmdir(applications.c_str());
    ::rmdir(data.c_str());
    ::unsetenv("XDG_DATA_HOME");
    ::unsetenv("XDG_DATA_DIRS");

    // Steam's id, which is not always an id. umu sets SteamAppId from
    // STEAM_COMPAT_APP_ID -- "0" unless the umu id ends in a number -- and Heroic
    // launches every game with GAMEID=umu-0, so every Heroic game arrived wearing
    // SteamAppId=0 and was recorded as `steam:0`.
    ::unsetenv("SteamAppId");
    ::unsetenv("SteamGameId");
    check(vocem::steam_app_id().empty(), "no Steam id when nothing is set");
    ::setenv("SteamAppId", "2357570", 1);
    check(vocem::steam_app_id() == "2357570", "a real one is kept");
    ::setenv("SteamAppId", "0", 1);
    check(vocem::steam_app_id().empty(), "umu's placeholder is not an id");
    ::setenv("SteamGameId", "1086940", 1);
    check(vocem::steam_app_id() == "1086940",
          "and the other variable is still read when the first says nothing");
    ::setenv("SteamAppId", "default", 1);
    ::unsetenv("SteamGameId");
    check(vocem::steam_app_id().empty(), "nor is the word umu-default leaves behind");
    ::unsetenv("SteamAppId");

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
    // Which one it was, because `launcher` on its own is a verdict with its
    // evidence thrown away, and the evidence is what the `why` field is for.
    check(vocem::launcher_name("minecraft-launc") == std::string("minecraft-launcher"),
          "and the refusal can say which name it matched, in full");
    check(vocem::launcher_name("Overwatch.exe") == nullptr, "with nothing to say about a game");

    // The record a process writes of itself.
    const std::string directory =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/vocem-apps-test-" + std::to_string(::getpid());
    ::mkdir(directory.c_str(), 0700);
    ::setenv("XDG_CACHE_HOME", directory.c_str(), 1);

    check(vocem::known_applications().empty(), "nothing is known before anything has run");

    // Two things left in the way of the write, in the directory anything in the
    // session can write to. The record's name is the process name, so two writers
    // of one name are ordinary rather than exotic -- every Minecraft is `java`,
    // every Chromium GPU process is `CrGpuMain`, and both are in this machine's
    // registry -- and the temporary used to be `<name>.tmp` for all of them.
    //
    // A FIFO at that name is the shape of the accident: fopen(..., "w") on a FIFO
    // with no reader blocks, and this code runs inside somebody else's first
    // frame. The defective version hangs here and the test dies on its timeout.
    // A symlink is the other: the write would have gone wherever it pointed.
    const std::string apps = directory + "/vocem/apps";
    vocem::make_directories(apps);
    const std::string in_the_way = apps + "/" + vocem::process_name();
    const std::string elsewhere = directory + "/written-through-the-symlink";
    check(::mkfifo((in_the_way + ".tmp").c_str(), 0600) == 0, "a FIFO where the temporary went");
    check(::symlink(elsewhere.c_str(),
                    (in_the_way + ".tmp." + std::to_string(::getpid())).c_str()) == 0,
          "and a symlink where it goes now");

    vocem::record_application("vulkan");

    struct stat followed {};
    check(::stat(elsewhere.c_str(), &followed) != 0, "nothing was written through the symlink");
    check(::unlink((in_the_way + ".tmp").c_str()) == 0, "and the FIFO was left where it was");
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

    // What the window is handed when a record is not one. These files are read
    // from a directory anything in the session can write to, at every tick, and
    // the key is what a flipped switch writes into the settings file.
    const std::string records = directory + "/vocem/apps";
    if (std::FILE* corrupt = std::fopen((records + "/corrupt").c_str(), "w")) {
        std::fprintf(corrupt, "name = ");
        for (int character = 0; character < 4000; ++character) {
            std::fputc('A', corrupt);
        }
        std::fprintf(corrupt, "\ngame = true\nseen = 1\n");
        std::fclose(corrupt);
    }
    if (std::FILE* leftover = std::fopen((records + "/dead.tmp.999999").c_str(), "w")) {
        std::fprintf(leftover, "name = dead\ngame = true\nseen = 1\n");
        std::fclose(leftover);
    }
    check(vocem::known_applications().size() == 1,
          "a name no kernel could have given is not a record, and neither is a temporary "
          "left behind by a writer that died");

    vocem::forget_applications();
    check(vocem::known_applications().empty(), "the list can be emptied");

    ::rmdir((directory + "/vocem/apps").c_str());
    ::rmdir((directory + "/vocem").c_str());
    ::rmdir(directory.c_str());

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
