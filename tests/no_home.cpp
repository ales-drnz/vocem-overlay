// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// With no HOME, the settings and the token are nowhere -- never in the
// working directory of whatever process is asking.
//
// Config::path() and paths.h's state_home() fell back to "." when neither
// HOME nor the XDG variable was set, the pattern entry 135 removed from the
// record and journal directories. Config::path() runs inside games: a game
// started without HOME (by a service, a script with `env -i`) from a
// directory somebody else can write read ./.config/vocem/config.ini --
// which names a font file for stb_truetype and switches the overlay off.
//
// Held here: HOME and the XDG variables unset, the working directory holding
// a config.ini that switches the overlay off and a token; the config is not
// honoured, neither path points into the working directory, and a save puts
// nothing there.

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "probe_alarm.h"
#include "vocem/config.h"
#include "vocem/paths.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

void write_file(const std::string& path, const char* text) {
    vocem::make_directories(path.substr(0, path.rfind('/')));
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fputs(text, file);
        std::fclose(file);
    }
}

int entries_in(const std::string& dir) {
    int count = 0;
    if (DIR* handle = opendir(dir.c_str())) {
        while (const dirent* entry = readdir(handle)) {
            if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) {
                ++count;
            }
        }
        closedir(handle);
    }
    return count;
}

}  // namespace

int main() {
    vocem_test::set_alarm(30, "reading settings with no HOME");
    char scratch_template[] = "/tmp/vocem-no-home-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch || chdir(scratch) != 0) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string cwd = scratch;
    write_file(cwd + "/.config/vocem/config.ini", "[general]\nenabled = false\n");
    write_file(cwd + "/.local/state/vocem/token", "planted-token\n");
    const int before = entries_in(cwd);

    unsetenv("HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
    unsetenv("FLATPAK_ID");

    const std::string config_path = vocem::Config::path();
    const std::string token_path = vocem::token_path();
    std::printf("     settings: %s\n     token: %s\n", config_path.c_str(), token_path.c_str());
    check(!config_path.empty() && config_path[0] == '/', "the settings path is absolute");
    check(!token_path.empty() && token_path[0] == '/', "the token path is absolute");

    vocem::Config config;
    config.load();
    check(config.enabled, "a config.ini in the working directory is not honoured");
    std::FILE* token = std::fopen(token_path.c_str(), "r");
    check(token == nullptr, "a token in the working directory is not read");
    if (token) {
        std::fclose(token);
    }
    check(!config.save(), "a save with nowhere to go says it failed");
    check(entries_in(cwd) == before, "and put nothing in the working directory");

    if (chdir("/") != 0) {
        std::printf("     (could not leave %s)\n", scratch);
    }
    std::string cleanup = std::string("rm -rf '") + scratch + "'";
    if (std::system(cleanup.c_str()) != 0) {
        std::printf("     (could not remove %s)\n", scratch);
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
