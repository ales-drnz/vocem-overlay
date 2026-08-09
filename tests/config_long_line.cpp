// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A settings value is as long as the user's list, not as long as a buffer.
//
// `Config::load()` read the file with `char line[256]` and a bare `fgets`, which
// takes 255 bytes and stops. The remainder of a longer line came back on the
// next iteration, had no '=' in it, and was skipped -- so nothing errored,
// nothing was logged, and the value was simply short. **The old limit was 255
// bytes per line**, and what it cost is two things, of which the second is the
// one that does not heal:
//
//   * the injected code got a truncated `hidden_apps`, so the applications the
//     user hid last were given the overlay again -- silently, in the games
//     they had just been taken out of;
//   * the settings window reads this same header (`ConfigBridge` calls
//     `config_.load()` and keeps the result), so the next Apply wrote the
//     truncation back. Those entries left the file for good, without the user
//     going anywhere near that page.
//
// And the last name to survive was cut mid-word, so the list gained an entry
// that is no application at all -- a rule against a process that does not
// exist, sitting in the file looking deliberate.
//
// Measured against the header as it shipped in 0.1.4: a 330-byte
// `hidden_apps` of twenty names went in, 241 bytes came back ending
// "...SomeGameBinary14,SomeGameBina", and `save()` then wrote those 241 bytes
// back, so the round trip lost five and a half names. Taken twice, identical.
//
// How close it was: the owner's own `~/.config/vocem/config.ini` line 50 is
// 179 characters with seventeen entries. Six or seven more hidden applications
// and it crosses. `font_path`, `font_path_strong` and `shown_apps` share the
// same road, and a font path is a thing the window writes without asking.
//
// The three properties held here are the ones the format promises and the one
// it was breaking: a long line survives the round trip whole, unknown keys are
// still ignored, missing keys still default. Plus the bound: a line past
// `kMaxLineBytes` is dropped rather than kept in pieces, because half a list is
// a list somebody will act on.
//
// No test framework on purpose: the rest of the project has no dependencies
// either.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void write_file(const std::string& path, const std::string& contents) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file) {
        std::fwrite(contents.data(), 1, contents.size(), file);
        std::fclose(file);
    }
}

// A comma-separated list of `count` plausible process names.
std::string names(int count, const char* stem) {
    std::string list;
    for (int i = 1; i <= count; ++i) {
        if (!list.empty()) {
            list += ',';
        }
        list += stem;
        list += std::to_string(i);
    }
    return list;
}

}  // namespace

int main() {
    const std::string directory =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/vocem-config-long-line-" + std::to_string(getpid());
    ::mkdir(directory.c_str(), 0700);
    ::setenv("XDG_CONFIG_HOME", directory.c_str(), 1);
    // The leaf too: `Config::path()` is <XDG_CONFIG_HOME>/vocem/config.ini and
    // only `save()` creates that directory. Without this the fixtures below
    // are written nowhere, every load returns the defaults, and the whole file
    // fails against a defective reader AND against a sound one -- for a reason
    // that has nothing to do with line length. It did, on the first run.
    ::mkdir((directory + "/vocem").c_str(), 0700);

    // Twenty names: 330 bytes, comfortably past the old 255-byte line and
    // exactly the case the owner's file is walking towards.
    const std::string hidden = names(20, "SomeGameBinary");
    // Different length and a different key, so a fix that happened to work for
    // one buffer boundary is not enough.
    const std::string shown = names(17, "AnotherGameExecutable");
    // A font path is written by the window without the user typing it, and
    // deep home directories are ordinary.
    std::string font_path = "/home/somebody/.local/share/fonts/";
    while (font_path.size() < 400) {
        font_path += "a-rather-deeply-nested-directory/";
    }
    font_path += "TheChosenFace-Regular.ttf";

    std::printf("--  hidden_apps %zu bytes, shown_apps %zu bytes, font_path %zu bytes\n",
                hidden.size(), shown.size(), font_path.size());

    {
        std::string text = "# a settings file with lines past any buffer\n";
        text += "[behaviour]\n";
        text += "hidden_apps = " + hidden + "\n";
        text += "shown_apps = " + shown + "\n";
        text += "[appearance]\n";
        text += "opacity = 0.75\n";
        text += "font_path = " + font_path + "\n";
        // An unknown key, on a long line of its own: the format promises these
        // are ignored, and a reader that resynchronises badly could take the
        // tail of one for the key of another.
        text += "a_key_no_version_of_this_program_has_ever_had = " + names(30, "Rubbish") + "\n";
        write_file(vocem::Config::path(), text);
    }

    vocem::Config first;
    first.load();
    std::printf("--  read back: hidden_apps %zu bytes, shown_apps %zu bytes, font_path %zu bytes\n",
                first.hidden_apps.size(), first.shown_apps.size(), first.font_path.size());
    check(first.hidden_apps == hidden, "a 330-byte hidden_apps is read whole");
    check(first.shown_apps == shown, "and a shown_apps of a different length with it");
    check(first.font_path == font_path, "and a font path past four hundred bytes");
    // The value on the line AFTER the long ones has to be right too: a reader
    // that loses its place in a long line reads the next one as its tail.
    check(first.opacity > 0.749f && first.opacity < 0.751f,
          "and the short line after them is still read correctly");
    // The two properties the format promises, which the fix may not cost.
    check(first.scale == vocem::Config{}.scale, "a key the file never mentions keeps its default");
    check(first.notification_corner == vocem::Config{}.notification_corner,
          "and so does another");

    // The round trip: this is the half that made the loss permanent, because
    // the window loads with this header and saves what it loaded.
    check(first.save(), "save() succeeded");
    vocem::Config second;
    second.load();
    check(second.hidden_apps == hidden, "hidden_apps survives the window's Apply");
    check(second.shown_apps == shown, "and so does shown_apps");
    check(second.font_path == font_path, "and so does the font path");
    check(second.hidden_apps.find("SomeGameBinary20") != std::string::npos,
          "including the last name, which is where a cut would show");

    // The bound. A line longer than the reader keeps is dropped whole: not
    // half a list, which is a list somebody would act on, and not a hang.
    {
        const std::string enormous = names(20000, "PaddingName");
        std::string text = "[behaviour]\n";
        text += "hidden_apps = " + enormous + "\n";
        text += "shown_apps = keep-me\n";  // the line after it must still arrive
        write_file(vocem::Config::path(), text);
        std::printf("--  an over-long line: %zu bytes\n", enormous.size());

        vocem::Config bounded;
        bounded.load();
        check(bounded.hidden_apps.empty(),
              "a line past the cap is dropped whole, not kept in pieces");
        check(bounded.shown_apps == "keep-me",
              "and the reader is still in the right place on the next line");
    }

    ::unlink(vocem::Config::path().c_str());
    ::unlink((vocem::Config::path() + ".tmp").c_str());
    ::rmdir((directory + "/vocem").c_str());
    ::rmdir(directory.c_str());

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
