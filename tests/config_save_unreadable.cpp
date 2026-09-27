// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A settings file the window cannot read is not a settings file it may write.
//
// Config::save() rewrites the file in place: it reads what is there and puts
// the window's values into it. On a config.ini that exists but cannot be read
// (mode 0200 here -- a chmod gone wrong, a file owned by somebody else) the
// read came back empty, the empty text was taken for "no file", and a fresh
// file of the window's values was renamed over it: every comment, every key
// from a newer version and every value not in the window's copy destroyed, by
// one click on a switch, while the mode that caused it was carried over. The
// save must fail instead -- the window says so (reportSave's message) and the
// edit stays pending.
//
// XDG_CONFIG_HOME is a scratch directory; nothing here reaches the owner's
// settings. Root reads a 0200 file, so as root the case cannot be built: skip.

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

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (::geteuid() == 0) {
        std::printf("-- skip: root reads a 0200 file, so an unreadable one cannot be made\n");
        return 77;
    }
    char scratch_template[] = "/tmp/vocem-config-unreadable-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string config_home = std::string(scratch) + "/config";
    ::mkdir(config_home.c_str(), 0700);
    ::mkdir((config_home + "/vocem").c_str(), 0700);
    setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
    const std::string path = config_home + "/vocem/config.ini";

    const std::string original =
        "# kept by hand\n"
        "[behaviour]\n"
        "flatpak_apps = com.example.SomeGame\n"
        "future_key = 42\n";
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fwrite(original.data(), 1, original.size(), file);
        std::fclose(file);
    }
    ::chmod(path.c_str(), 0200);
    std::FILE* probe = std::fopen(path.c_str(), "r");
    check(probe == nullptr && ::access(path.c_str(), F_OK) == 0,
          "the file exists and cannot be read");
    if (probe) {
        std::fclose(probe);
    }

    // Apply's road and a switch's road.
    vocem::Config config;
    config.text_shadow = true;
    const bool saved = config.save();
    check(!saved, "save() on an unreadable file fails rather than writing over it");
    const bool switched = vocem::Config::write_switch(&vocem::Config::enabled, false);
    check(!switched, "and so does a switch");

    ::chmod(path.c_str(), 0600);
    const std::string after = read_all(path);
    const bool kept = after == original;
    if (!kept) {
        std::printf("--  the file now says:\n%s\n", after.c_str());
    }
    check(kept, "the file's contents are what they were");

    check(::access((path + ".tmp").c_str(), F_OK) != 0, "and no temporary is left beside it");

    std::remove(path.c_str());
    ::rmdir((config_home + "/vocem").c_str());
    ::rmdir(config_home.c_str());
    ::rmdir(scratch);
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
