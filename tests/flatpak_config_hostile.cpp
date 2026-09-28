// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The settings' copy into a Flatpak sandbox, against both of its ends.
//
// FlatpakBridge::mirror_config() copies the host's config.ini into every
// sandbox the daemon serves (the overlay in there cannot reach ~/.config).
// Its source is the user's file, which may be a dotfiles link, a FIFO, a file
// nobody may read, or gone; its destination is a directory the sandbox owns
// and may fill with links, FIFOs and directories under the names the copy
// uses (entry 82). Held here, for each shape:
//   * nothing blocks (every sweep runs under the probe's alarm);
//   * nothing is written through a link the sandbox planted, and nothing
//     lands outside the sandbox's directory;
//   * a copy that failed is tried again on the next sweep without the host's
//     file changing (entry 248), and the failure is said once;
//   * a copy that could not be read whole is never published: the sandbox
//     keeps the last whole copy;
//   * a host file that is gone is gone in the sandbox too, so a game in
//     there reads the defaults a host game reads.
//
// Scratch XDG directories throughout; the bridge under test is an object in
// this process, no daemon, no /dev/shm.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>

#include "flatpak_bridge.h"
#include "probe_alarm.h"
#include "vocem/flatpak.h"
#include "vocem/shared_state.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::read_file;
using vocem_test::write_file;

namespace {

struct Scene {
    std::string root;
    std::string host;      // $XDG_CONFIG_HOME/vocem/config.ini
    std::string sandbox;   // $XDG_RUNTIME_DIR/app/<id>/vocem
    std::string copy;      // the copy inside it
    std::string outside;   // a directory the sandbox must not reach
    std::string log;
    int saved_stderr = -1;
    int log_fd = -1;
    vocem::FlatpakBridge* bridge = nullptr;
};

vocem::SharedState g_state{};

bool is_regular(const std::string& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_link(const std::string& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

void sweep(Scene& scene, int times = 1) {
    for (int i = 0; i < times; ++i) {
        scene.bridge->rescan();
        scene.bridge->refresh_files(g_state);
    }
}

long said(const Scene& scene, const char* needle) {
    fflush(stderr);
    const std::string text = read_file(scene.log);
    long count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// A new scratch world with one adopted sandbox and a host config.ini holding
// `settings`, its stderr going to a log this scene reads back.
Scene open_scene(const char* name, const std::string& settings) {
    Scene scene;
    scene.root = vocem_test::scratch_dir("vocem-flatpak-config-hostile",
                                         {"/config", "/config/vocem", "/run", "/run/app",
                                          "/cache", "/data", "/outside"});
    setenv("XDG_RUNTIME_DIR", (scene.root + "/run").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (scene.root + "/config").c_str(), 1);
    setenv("XDG_CACHE_HOME", (scene.root + "/cache").c_str(), 1);
    setenv("XDG_DATA_HOME", (scene.root + "/data").c_str(), 1);
    setenv("XDG_DATA_DIRS", (scene.root + "/none/flatpak/exports/share").c_str(), 1);
    unsetenv("FLATPAK_ID");
    scene.host = scene.root + "/config/vocem/config.ini";
    if (!settings.empty()) {
        write_file(scene.host, settings);
    }
    const std::string app = scene.root + "/run/app/org.example.Game";
    mkdir(app.c_str(), 0700);
    scene.sandbox = app + "/vocem";
    mkdir(scene.sandbox.c_str(), 0700);
    write_file(scene.sandbox + "/" + vocem::kBridgeRequestName, "pid=1\ndrawing=0\n");
    scene.copy = scene.sandbox + "/" + vocem::kBridgeConfigName;
    scene.outside = scene.root + "/outside";
    scene.log = scene.root + "/bridge.log";
    fflush(stderr);
    scene.saved_stderr = dup(2);
    scene.log_fd = open(scene.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(scene.log_fd, 2);
    g_state.abi_version = vocem::kAbiVersion;
    scene.bridge = new vocem::FlatpakBridge;
    scene.bridge->start();
    scene.bridge->rescan();
    printf("-- %s\n", name);
    check(scene.bridge->served() == 1, "the sandbox is adopted");
    return scene;
}

void close_scene(Scene& scene) {
    scene.bridge->stop();
    delete scene.bridge;
    fflush(stderr);
    dup2(scene.saved_stderr, 2);
    close(scene.saved_stderr);
    close(scene.log_fd);
    (void)!system(("chmod -R u+rwx '" + scene.root + "' 2>/dev/null").c_str());
    vocem_test::remove_tree(scene.root);
}

// A later modification time on the host file, as an edit makes.
void touch_later(const std::string& path, int seconds) {
    struct timeval times[2];
    gettimeofday(&times[0], nullptr);
    times[0].tv_sec += seconds;
    times[1] = times[0];
    utimes(path.c_str(), times);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    vocem_test::set_alarm(20, "a hostile settings copy (the last scene printed is the one that blocked)");
    if (geteuid() == 0) {
        printf("-- skip: root reads a file of mode 000\n");
        return 77;
    }
    const std::string settings = "[appearance]\nscale = 1.75\n";
    const std::string secret = "the victim's own bytes\n";

    {
        // The sandbox put a link where the copy goes, pointing out of it.
        Scene scene = open_scene("config.ini is a link out of the sandbox", settings);
        write_file(scene.outside + "/victim", secret);
        unlink(scene.copy.c_str());
        symlink((scene.outside + "/victim").c_str(), scene.copy.c_str());
        sweep(scene);
        check(read_file(scene.outside + "/victim") == secret, "the link's target is not written");
        check(is_regular(scene.copy) && read_file(scene.copy) == settings,
              "the copy replaced the link with the settings");
        close_scene(scene);
    }
    {
        // The temporary's name, planted as a link: the copy must not follow
        // it, and must arrive once the sandbox takes it away.
        Scene scene = open_scene("config.ini.part is a link out of the sandbox", settings);
        write_file(scene.outside + "/victim", secret);
        symlink((scene.outside + "/victim").c_str(), (scene.copy + ".part").c_str());
        sweep(scene, 3);
        check(read_file(scene.outside + "/victim") == secret, "the link's target is not written");
        check(said(scene, "could not copy config.ini") <= 1, "the failure is said at most once");
        unlink((scene.copy + ".part").c_str());
        sweep(scene);
        check(read_file(scene.copy) == settings,
              "the copy arrives once the link is gone, with the host's file unchanged");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("config.ini.part is a FIFO, then a directory", settings);
        mkfifo((scene.copy + ".part").c_str(), 0600);
        sweep(scene, 2);  // under the alarm: a blocked open ends the test
        check(!is_regular(scene.copy), "no copy while the FIFO is there");
        unlink((scene.copy + ".part").c_str());
        mkdir((scene.copy + ".part").c_str(), 0700);
        sweep(scene, 2);
        rmdir((scene.copy + ".part").c_str());
        sweep(scene);
        check(read_file(scene.copy) == settings, "the copy arrives once the name is free");
        close_scene(scene);
    }
    {
        // The sandbox moved its directory away and put a link to somewhere
        // else in its place, after adoption.
        Scene scene = open_scene("the sandbox's vocem directory swapped for a link", settings);
        rename(scene.sandbox.c_str(), (scene.sandbox + ".old").c_str());
        symlink(scene.outside.c_str(), scene.sandbox.c_str());
        touch_later(scene.host, 5);
        sweep(scene, 2);
        check(access((scene.outside + "/" + vocem::kBridgeConfigName).c_str(), F_OK) != 0,
              "nothing lands in the directory the link names");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("the host's config.ini is a dotfiles link", "");
        write_file(scene.root + "/dot.ini", settings);
        symlink((scene.root + "/dot.ini").c_str(), scene.host.c_str());
        sweep(scene);
        check(read_file(scene.copy) == settings, "the link is followed on the host's side");
        check(is_link(scene.host), "and stays a link");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("the host's config.ini is a FIFO", "");
        mkfifo(scene.host.c_str(), 0600);
        sweep(scene, 3);  // under the alarm
        check(!is_regular(scene.copy), "nothing is copied from a FIFO");
        check(said(scene, "could not copy config.ini") <= 1, "said at most once");
        unlink(scene.host.c_str());
        write_file(scene.host, settings);
        sweep(scene);
        check(read_file(scene.copy) == settings, "a regular file in its place is copied");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("the host's config.ini cannot be read", settings);
        chmod(scene.host.c_str(), 0000);
        struct stat before {};
        stat(scene.host.c_str(), &before);
        sweep(scene, 3);
        check(!is_regular(scene.copy), "nothing is copied while it cannot be read");
        check(said(scene, "could not copy config.ini") == 1, "the failure is said once");
        chmod(scene.host.c_str(), 0600);  // the modification time does not move
        sweep(scene);
        check(read_file(scene.copy) == settings,
              "the copy arrives on the next sweep once it can be read");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("the host's config.ini is a dangling link", "");
        symlink((scene.root + "/dot.ini").c_str(), scene.host.c_str());
        sweep(scene, 2);
        check(!is_regular(scene.copy), "nothing to copy yet");
        write_file(scene.root + "/dot.ini", settings);
        sweep(scene);
        check(read_file(scene.copy) == settings, "the copy arrives when the target is written");
        close_scene(scene);
    }
    {
        // Past the copy's ceiling: not read whole, so not published, and the
        // last whole copy stays.
        Scene scene = open_scene("the host's config.ini grows past the ceiling", settings);
        sweep(scene);
        check(read_file(scene.copy) == settings, "the first copy arrives");
        write_file(scene.host, settings + "# " + std::string(2u << 20, 'x') + "\n");
        touch_later(scene.host, 5);
        sweep(scene, 3);
        check(read_file(scene.copy) == settings, "the sandbox keeps the last whole copy");
        check(said(scene, "could not copy config.ini") == 1, "the refusal is said once");
        close_scene(scene);
    }
    {
        Scene scene = open_scene("the host's config.ini is removed", settings);
        sweep(scene);
        check(read_file(scene.copy) == settings, "the copy arrives");
        unlink(scene.host.c_str());
        sweep(scene, 2);
        check(access(scene.copy.c_str(), F_OK) != 0,
              "the copy goes too, so the game in there reads the defaults a host game reads");
        write_file(scene.host, settings);
        sweep(scene);
        check(read_file(scene.copy) == settings, "and comes back with the file");
        close_scene(scene);
    }
    return vocem_test::finish();
}
