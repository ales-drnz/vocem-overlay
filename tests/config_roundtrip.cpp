// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The settings file has to mean the same thing in every process that reads it.
// That was not true: the GUI runs with the user's LC_NUMERIC (Qt6 sets it and does
// not put it back), the layer and the daemon run in the C locale, and a value
// written as "0,82" came back as 0 on the other side -- an invisible panel with no
// error printed anywhere.
//
// So the test runs the same round-trip under both locales and, more importantly,
// checks that a file written under one is read correctly under the other. CTest
// runs it twice, once with LC_ALL=C and once with LC_ALL=it_IT.UTF-8.
//
// No test framework on purpose: the rest of the project has no dependencies either.

#include <sys/stat.h>
#include <unistd.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vocem/config.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++failures;
    }
}

void check_close(float actual, float expected, const char* what) {
    // Well under the resolution the file is written at, so a genuine mismatch
    // (a comma read as zero) cannot hide inside the tolerance.
    if (actual < expected - 0.0005f || actual > expected + 0.0005f) {
        std::printf("  FAIL  %s: got %.6f, expected %.6f\n", what,
                    static_cast<double>(actual), static_cast<double>(expected));
        ++failures;
    }
}

vocem::Config sample() {
    vocem::Config config;
    config.position_x = 0.0116f;
    config.position_y = 0.4083f;
    config.scale = 1.25f;
    config.opacity = 0.82f;
    config.avatar_size = 1.35f;
    config.avatar_idle_opacity = 0.37f;
    config.notification_seconds = 7.5f;
    config.panel_colour = 0x1a2b3c;
    config.notification_colour = 0xffcc00;
    config.speaking_colour = 0x00ff80;
    // One pinned, two left on the automatism: the auto sentinel must survive
    // the trip as the WORD -- masked to 24 bits it would come back as white and
    // silently kill the ramp -- and a picked colour must come back as itself.
    config.text_idle_colour = 0x9a9b9c;
    config.text_speaking_colour = vocem::Config::kColourAuto;
    config.notification_text_colour = vocem::Config::kColourAuto;
    config.notification_opacity = 0.9f;
    config.show_channel_name = false;
    config.notification_corner = 2;
    // The one setting written as a word rather than a number, which is exactly
    // why it is here: a save() that printed the integer would still parse back
    // (to_layout takes both), and only a round trip through the file catches a
    // word that goes out and does not come home.
    config.panel_layout = vocem::Config::kLayoutHorizontal;
    // The second setting written as a word, off its default for the same reason
    // every value here is: a key that fell out of save() would read back as
    // exactly what was expected of it.
    config.panel_box = vocem::Config::kBoxPanel;
    // Every spacing key, non-default for the same reason: these sat at their
    // defaults here for as long as the file has existed, so a spacing key that
    // fell out of save() would have read back as exactly the value expected.
    config.screen_margin = 23.0f;
    config.notification_margin = 41.0f;
    config.box_padding_x = 7.0f;
    config.box_padding_y = 11.0f;
    config.avatar_gap = 13.0f;
    config.row_spacing = 17.0f;
    config.font_size = 19.0f;
    // Non-default, as everything here is: the default is empty, and a key that
    // fell out of save() or load() would read back as exactly the empty string
    // expected of it. Two different names, so a save() writing one value under
    // both keys cannot pass either.
    config.preview_display_panel = "HDMI-A-1";
    config.preview_display_notification = "DP-3";
    // Every boolean, flipped off its default, and the last keys that sat at
    // theirs. Thirteen keys had never been round-tripped here or in
    // config_long_line -- and a key dropped from save() is invisible at its
    // default: the value loads from a hand-edited file and is silently reset
    // the next time the window Applies, which is the hidden_apps failure
    // config.h documents, waiting on twelve booleans.
    config.enabled = false;
    config.panel_enabled = false;
    config.notifications_enabled = false;
    config.only_speaking = true;
    config.hide_self = true;
    config.show_muted_state = false;
    config.text_shadow = true;
    config.keep_running = false;
    config.start_at_login = true;
    config.tray_voice_icon = false;
    config.notification_scale = 1.85f;
    config.font_family = "Test Family";
    config.font_path = "/tmp/nowhere/regular.ttf";
    config.font_path_strong = "/tmp/nowhere/bold.ttf";
    return config;
}

void expect_sample(const vocem::Config& config, const char* context) {
    std::printf("  in %s\n", context);
    check_close(config.position_x, 0.0116f, "position_x");
    check_close(config.position_y, 0.4083f, "position_y");
    check_close(config.scale, 1.25f, "scale");
    check_close(config.opacity, 0.82f, "opacity");
    check_close(config.avatar_size, 1.35f, "avatar_size");
    check_close(config.avatar_idle_opacity, 0.37f, "avatar_idle_opacity");
    check_close(config.notification_seconds, 7.5f, "notification_seconds");
    check(config.panel_colour == 0x1a2b3c, "panel_colour");
    check(config.notification_colour == 0xffcc00, "notification_colour");
    check(config.speaking_colour == 0x00ff80, "speaking_colour");
    check(config.text_idle_colour == 0x9a9b9c, "text_idle_colour");
    check(config.text_speaking_colour == vocem::Config::kColourAuto,
          "text_speaking_colour stays auto");
    check(config.notification_text_colour == vocem::Config::kColourAuto,
          "notification_text_colour stays auto");
    check_close(config.notification_opacity, 0.9f, "notification_opacity");
    check(!config.show_channel_name, "show_channel_name");
    check(config.notification_corner == 2, "notification_corner");
    check(config.panel_layout == vocem::Config::kLayoutHorizontal, "panel_layout");
    check(config.panel_box == vocem::Config::kBoxPanel, "panel_box");
    check_close(config.screen_margin, 23.0f, "screen_margin");
    check_close(config.notification_margin, 41.0f, "notification_margin");
    check_close(config.box_padding_x, 7.0f, "box_padding_x");
    check_close(config.box_padding_y, 11.0f, "box_padding_y");
    check_close(config.avatar_gap, 13.0f, "avatar_gap");
    check_close(config.row_spacing, 17.0f, "row_spacing");
    check_close(config.font_size, 19.0f, "font_size");
    check(config.preview_display_panel == "HDMI-A-1", "preview_display_panel");
    check(config.preview_display_notification == "DP-3", "preview_display_notification");
    check(!config.enabled, "enabled");
    check(!config.panel_enabled, "panel_enabled");
    check(!config.notifications_enabled, "notifications_enabled");
    check(config.only_speaking, "only_speaking");
    check(config.hide_self, "hide_self");
    check(!config.show_muted_state, "show_muted_state");
    check(config.text_shadow, "text_shadow");
    check(!config.keep_running, "keep_running");
    check(config.start_at_login, "start_at_login");
    check(!config.tray_voice_icon, "tray_voice_icon");
    check_close(config.notification_scale, 1.85f, "notification_scale");
    check(config.font_family == "Test Family", "font_family");
    check(config.font_path == "/tmp/nowhere/regular.ttf", "font_path");
    check(config.font_path_strong == "/tmp/nowhere/bold.ttf", "font_path_strong");
}

// Reads the file back as text, because the point is not only that the values
// survive but that what lands on disk is unambiguous whoever opens it next.
std::string read_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return {};
    }
    std::string contents;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file)) {
        contents += buffer;
    }
    std::fclose(file);
    return contents;
}

}  // namespace

int main(int argc, char** argv) {
    // The locale the file is written under. Anything the C library refuses to set
    // is reported rather than silently skipped, or the test would pass by not
    // running.
    const char* wanted = argc > 1 ? argv[1] : "C";
    if (!std::setlocale(LC_ALL, wanted)) {
        std::printf("locale %s is not available on this system\n", wanted);
        return 77;  // CTest treats this as skipped
    }
    std::printf("writing under LC_NUMERIC=%s\n", std::setlocale(LC_NUMERIC, nullptr));

    const std::string directory = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                                  "/vocem-config-test-" + std::to_string(getpid());
    ::mkdir(directory.c_str(), 0700);
    ::setenv("XDG_CONFIG_HOME", directory.c_str(), 1);

    check(sample().save(), "save() succeeded");

    const std::string text = read_file(vocem::Config::path());
    check(!text.empty(), "the file exists and is not empty");
    check(text.find(',') == std::string::npos ||
              text.find("hidden_apps") < text.find(','),
          "no decimal comma anywhere before the comma-separated list of applications");
    check(text.find("opacity = 0.82") != std::string::npos, "opacity written as 0.82");
    check(text.find("position_x = 0.0116") != std::string::npos,
          "position_x written as 0.0116");

    // Read it back in the same locale.
    {
        vocem::Config config;
        config.load();
        expect_sample(config, wanted);
    }

    // And in the other one, which is the case that was broken: the GUI writes in
    // the user's locale, the game process reads in C.
    {
        const char* other = std::strcmp(wanted, "C") == 0 ? "it_IT.UTF-8" : "C";
        if (std::setlocale(LC_ALL, other)) {
            vocem::Config config;
            config.load();
            expect_sample(config, other);
        } else {
            std::printf("  (locale %s unavailable, cross-locale read not exercised)\n", other);
        }
    }

    // A file already written with commas by the broken version still has to load,
    // rather than resetting every number to zero the first time it is opened.
    {
        std::setlocale(LC_ALL, "C");
        std::FILE* file = std::fopen(vocem::Config::path().c_str(), "w");
        check(file != nullptr, "legacy file created");
        if (file) {
            std::fprintf(file, "opacity = 0,82\nscale = 1,25\nposition_x = 0,0116\n");
            std::fclose(file);
        }
        vocem::Config config;
        config.load();
        std::printf("  in a legacy comma-separated file\n");
        check_close(config.opacity, 0.82f, "opacity");
        check_close(config.scale, 1.25f, "scale");
        check_close(config.position_x, 0.0116f, "position_x");
    }

    std::remove(vocem::Config::path().c_str());
    ::rmdir((directory + "/vocem").c_str());
    ::rmdir(directory.c_str());

    std::printf(failures == 0 ? "OK\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
