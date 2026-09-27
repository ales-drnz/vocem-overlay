// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A line the reader refused is not the window's to erase.
//
// load() drops a line longer than 64 KiB whole (config.h, read_line: half a
// list is not a list, and the reader lives inside games). The window reads
// the file through the same load(), so its copy of that key is the default --
// and save() rewrote every known key's line with the window's value: one
// click on a switch turned an 8000-entry hidden_apps into `hidden_apps = `,
// and the list was gone from the file for good. The refused line is kept as
// it is now. The window's own value goes on a line after it only when there
// is one to write (the reader then takes that line, the long one being
// dropped), so an edit made in the window still reaches the overlay.
//
// XDG_CONFIG_HOME is a scratch directory; nothing here reaches the owner's
// settings.

#include <sys/stat.h>
#include <unistd.h>

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

size_t count_of(const std::string& text, const std::string& needle) {
    size_t found = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char scratch_template[] = "/tmp/vocem-config-long-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string root = scratch;
    const std::string config_home = root + "/config";
    ::mkdir(config_home.c_str(), 0700);
    ::mkdir((config_home + "/vocem").c_str(), 0700);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    const std::string path = config_home + "/vocem/config.ini";

    std::string list;
    for (int i = 0; i < 8000; ++i) {
        list += (i ? "," : "") + std::string("SomeGameBinary") + std::to_string(i);
    }
    const std::string long_line = "hidden_apps = " + list;
    const std::string original = "[behaviour]\n" + long_line + "\nenabled = true\n";
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fwrite(original.data(), 1, original.size(), file);
        std::fclose(file);
    }
    std::printf("--  the hidden_apps line is %zu bytes\n", long_line.size());

    vocem::Config before;
    before.load();
    check(before.hidden_apps.empty(), "load() refuses the line (the premise)");

    // One click on the overlay's switch.
    check(vocem::Config::write_switch(&vocem::Config::enabled, false), "the switch is written");
    std::string text = read_all(path);
    const bool kept = text.find(long_line + "\n") != std::string::npos;
    if (!kept) {
        std::printf("--  the file is now %zu bytes, %zu hidden_apps lines\n", text.size(),
                    count_of(text, "hidden_apps"));
    }
    check(kept, "the 8000-entry line is still in the file, byte for byte");
    check(count_of(text, "hidden_apps =") == 1, "and no second, empty hidden_apps line joined it");
    check(text.find("enabled = false") != std::string::npos, "and the switch is in the file");

    // An edit made in the window still reaches the reader.
    vocem::Config window;
    window.load();
    window.hidden_apps = "OneGame";
    check(window.save(), "a save with a hidden_apps of the window's own succeeds");
    text = read_all(path);
    check(text.find(long_line + "\n") != std::string::npos, "the long line is still there");
    vocem::Config after;
    after.load();
    check(after.hidden_apps == "OneGame", "and the reader takes the window's value");

    std::string command = "rm -rf '" + root + "'";
    if (std::system(command.c_str()) != 0) {
        std::printf("--  could not remove %s\n", root.c_str());
    }
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
