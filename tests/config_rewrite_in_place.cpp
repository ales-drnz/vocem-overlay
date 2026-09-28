// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A settings file the window writes is still the file somebody else wrote.
//
// Config::save() is the window's only way to the file -- Apply, and every
// click on one of the three instant switches through write_switch() -- and it
// used to print a fresh file of its own and rename it over config.ini. One
// click on a switch, measured on 0.1.10:
//   * a symlinked config.ini (dotfiles kept in a repository, the common way to
//     keep them) became a regular file; the target kept the old values and no
//     longer reached the overlay;
//   * `#` comments and a key this version does not know (`future_key = 42`,
//     which is what a newer version's setting looks like to an older window)
//     were gone -- DESIGN "Settings" promises unknown keys are ignored so newer
//     files never break older overlays, and the window was deleting them;
//   * a file kept at 0600 came back 0644.
// Each of those is a check below, and each fails against that save().
//
// XDG_CONFIG_HOME is a scratch directory; nothing here reaches the owner's
// settings.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vocem/config.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

std::string read_all(const std::string& path) {
    std::string text;
    if (std::FILE* file = std::fopen(path.c_str(), "r")) {
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            text.append(chunk, got);
        }
        std::fclose(file);
    }
    return text;
}

void write_all(const std::string& path, const std::string& text) {
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t found = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

// Every entry of `directory`, but . and .., so a temporary left behind shows.
int entries_in(const std::string& directory) {
    const std::string command = "ls -A '" + directory + "' | wc -l";
    int count = -1;
    if (std::FILE* pipe = popen(command.c_str(), "r")) {
        if (std::fscanf(pipe, "%d", &count) != 1) {
            count = -1;
        }
        pclose(pipe);
    }
    return count;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char scratch_template[] = "/tmp/vocem-config-rewrite-XXXXXX";
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
    const std::string target = dotfiles + "/vocem.ini";

    // The file as a person keeps it: readable by nobody else, with notes in
    // it and a key from a newer version of this program.
    const std::string original =
        "# my overlay, synced from the laptop\n"
        "[appearance]\n"
        "scale = 1.25\n"
        "; the key below is from a newer vocem\n"
        "future_key = 42\n"
        "\n"
        "[behaviour]\n"
        "panel_enabled = true\n"
        "enabled = true\n"
        "# hidden because it stutters\n"
        "hidden_apps = somegame\n";

    // ---- first, a plain file: one click on the panel's switch.
    write_all(link, original);
    ::chmod(link.c_str(), 0600);
    check(vocem::Config::write_switch(&vocem::Config::panel_enabled, false),
          "the switch was written");
    const std::string after = read_all(link);
    std::printf("--  the file now reads:\n%s--  (end)\n", after.c_str());
    check(after.find("panel_enabled = false") != std::string::npos, "the switch is in the file");
    check(after.find("# my overlay, synced from the laptop\n") == 0,
          "the first comment is still the first line");
    check(after.find("# hidden because it stutters\n") != std::string::npos,
          "a comment between two keys survives");
    check(after.find("; the key below is from a newer vocem\n") != std::string::npos,
          "a ';' comment survives");
    check(after.find("future_key = 42\n") != std::string::npos,
          "a key this version does not know survives");
    check(after.find("# hidden because it stutters\nhidden_apps = somegame\n") != std::string::npos,
          "and the lines keep their order: the note still sits above the key it is about");
    check(after.find("scale = 1.25\n") != std::string::npos &&
              after.find("scale = 1.25\n") < after.find("future_key = 42\n"),
          "a known key is rewritten where it stood");
    check(count_of(after, "panel_enabled =") == 1, "the switch is written once");
    // Keys the file did not carry are added, so the file still says everything
    // the window knows; the round trip at the end holds their values.
    check(after.find("font_size =") != std::string::npos, "a known key the file lacked is added");
    struct stat plain_info{};
    ::stat(link.c_str(), &plain_info);
    check((plain_info.st_mode & 07777) == 0600, "the file is still 0600");
    if ((plain_info.st_mode & 07777) != 0600) {
        std::printf("--  mode is now %04o\n", plain_info.st_mode & 07777);
    }
    check(entries_in(config_home + "/vocem") == 1, "no temporary is left beside it");

    vocem::Config read;
    read.load();
    check(!read.panel_enabled, "the overlay reads the switch off");
    check(read.hidden_apps == "somegame", "and still hides the game");
    check(read.scale > 1.24f && read.scale < 1.26f, "and keeps the scale");

    // ---- then the same file kept in a dotfiles directory and linked in.
    ::unlink(link.c_str());
    write_all(target, original);
    ::chmod(target.c_str(), 0600);
    if (::symlink(target.c_str(), link.c_str()) != 0) {
        std::printf("FAIL could not make the symlink\n");
        return 1;
    }
    check(vocem::Config::write_switch(&vocem::Config::panel_enabled, false),
          "the switch was written through the link");
    struct stat link_info{};
    const bool still_link = ::lstat(link.c_str(), &link_info) == 0 && S_ISLNK(link_info.st_mode);
    check(still_link, "config.ini is still the symlink it was, not a file put in its place");
    const std::string through = read_all(target);
    check(through.find("panel_enabled = false") != std::string::npos,
          "the switch reached the file the link points at");
    check(through.find("future_key = 42\n") != std::string::npos, "which keeps its unknown key");
    struct stat target_info{};
    ::stat(target.c_str(), &target_info);
    check((target_info.st_mode & 07777) == 0600, "and its mode");
    check(entries_in(dotfiles) == 1, "no temporary is left beside the target");
    check(entries_in(config_home + "/vocem") == 1, "nor beside the link");

    // A key written twice by hand: load() takes the last one, so a rewrite
    // that replaced only the first would leave the old value in charge.
    write_all(target, "enabled = false\n# again\nenabled = false\n");
    vocem::Config::write_switch(&vocem::Config::enabled, true);
    vocem::Config twice;
    twice.load();
    check(twice.enabled, "a key present twice ends up with the value written");
    check(read_all(target).find("# again\n") != std::string::npos, "and the note between survives");

    // The file that does not exist yet is created, with its sections, and
    // reads back as the values written.
    ::unlink(link.c_str());
    vocem::Config fresh;
    fresh.font_size = 21.0f;
    fresh.row_spacing = 7.0f;
    fresh.hidden_apps = "a,b";
    check(fresh.save(), "a new file is written");
    vocem::Config back;
    back.load();
    check(back.font_size > 20.9f && back.font_size < 21.1f && back.row_spacing > 6.9f &&
              back.row_spacing < 7.1f && back.hidden_apps == "a,b",
          "and reads back as written");
    const std::string created = read_all(link);
    check(created.find("[position]") != std::string::npos &&
              created.find("[behaviour]") != std::string::npos,
          "with its sections");

    std::system(("rm -rf '" + root + "'").c_str());
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
