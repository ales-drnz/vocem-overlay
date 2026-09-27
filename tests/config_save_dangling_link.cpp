// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A config.ini that is a link to a file not there yet stays a link.
//
// save() resolves the path with realpath() so the rename replaces the file a
// link points at and the link stays a link (config_rewrite_in_place). But
// realpath() fails when the link's target does not exist -- dotfiles linked
// in before the file they point at was ever written, the ordinary first run
// of a dotfiles repository -- and save() then fell back to the link's own
// path: the rename put a regular file where the link was, and the dotfiles
// file never received a setting. The link is followed by hand now, relative
// targets against the link's directory, and a loop of links is refused rather
// than broken.
//
// XDG_CONFIG_HOME is a scratch directory; nothing here reaches the owner's
// settings.

#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "vocem/config.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

bool is_link(const std::string& path) {
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

bool is_regular(const std::string& path) {
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string link_text(const std::string& path) {
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    return length < 0 ? std::string() : std::string(buffer, static_cast<size_t>(length));
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char scratch_template[] = "/tmp/vocem-config-dangling-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    const std::string config_home = root + "/config";
    const std::string dotfiles = root + "/dotfiles";
    ::mkdir(config_home.c_str(), 0700);
    ::mkdir((config_home + "/vocem").c_str(), 0700);
    ::mkdir(dotfiles.c_str(), 0700);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    const std::string link = config_home + "/vocem/config.ini";

    // A relative link, as `ln -s ../../dotfiles/vocem.ini` makes it, to a file
    // that is not there yet.
    const std::string relative = "../../dotfiles/vocem.ini";
    ::symlink(relative.c_str(), link.c_str());
    check(is_link(link) && ::access(link.c_str(), F_OK) != 0,
          "config.ini is a link whose target does not exist");

    vocem::Config config;
    config.text_shadow = true;
    check(config.save(), "save() succeeds");
    check(is_link(link), "config.ini is still a link after the save");
    check(link_text(link) == relative, "and still says what it said");
    check(is_regular(dotfiles + "/vocem.ini"), "the file it points at now exists");
    vocem::Config read_back;
    read_back.load();
    check(read_back.text_shadow, "and carries the setting, read back through the link");

    // A switch, on the same link, now that the target exists: the ordinary road.
    check(vocem::Config::write_switch(&vocem::Config::enabled, false), "a switch succeeds");
    check(is_link(link), "and leaves the link a link");

    // A loop of links has no file at its end: refused, and the links stay.
    const std::string loop_a = config_home + "/vocem/loop-a";
    ::unlink(link.c_str());
    ::symlink(loop_a.c_str(), link.c_str());
    ::symlink(link.c_str(), loop_a.c_str());
    check(!config.save(), "a loop of links is refused");
    check(is_link(link) && is_link(loop_a), "and both links are left as they were");

    std::string command = "rm -rf '" + root + "'";
    if (std::system(command.c_str()) != 0) {
        std::printf("--  could not remove %s\n", root.c_str());
    }
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
