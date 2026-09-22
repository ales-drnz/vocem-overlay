// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The settings parser on the file a person writes, not only on the file the
// window writes (config_roundtrip is the latter). The README invites editing
// config.ini by hand, and the reader is compiled into every game: what it
// makes of a stray quote, a capital letter or a word where a number goes is
// the difference between an overlay that follows the file and one that
// silently does something else. Each case is a file written to a scratch
// XDG_CONFIG_HOME and loaded through the same Config::load() the games run.
//
// And the three switches, written on top of the file as it stands:
// Config::write_switch() loads fresh, sets one, saves -- the window used to
// write them over a copy it had loaded at startup, and every key edited since
// was put back (entry 136).

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

std::string g_root;

vocem::Config loaded_from(const char* contents) {
    if (std::FILE* file = std::fopen(vocem::Config::path().c_str(), "w")) {
        std::fputs(contents, file);
        std::fclose(file);
    }
    vocem::Config config;
    config.load();
    return config;
}

std::string file_text() {
    std::string text;
    if (std::FILE* file = std::fopen(vocem::Config::path().c_str(), "r")) {
        char buffer[4096];
        size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, got);
        }
        std::fclose(file);
    }
    return text;
}

bool near(float a, double b) { return a > b - 0.0005 && a < b + 0.0005; }

}  // namespace

int main() {
    char root[] = "/tmp/vocem-config-parse-XXXXXX";
    if (!mkdtemp(root)) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    g_root = root;
    setenv("XDG_CONFIG_HOME", root, 1);
    vocem::make_directories(g_root + "/vocem");

    // Numbers: the table's bounds, both separators, spaces, a word.
    {
        const vocem::Config c = loaded_from(
            "scale = 9\nopacity = 0,42\nfont_size=7\nnotification_seconds = 30\n"
            "row_spacing = many\navatar_size =   1.25  \n");
        check(near(c.scale, 3.0), "a number over the bound is held to it");
        check(near(c.opacity, 0.42), "a comma is a decimal separator (entry 15)");
        check(near(c.font_size, 8.0), "and one under the bound is raised to it");
        check(near(c.notification_seconds, 30.0), "the top of a range is allowed");
        check(near(c.row_spacing, 0.0), "a word where a number goes reads as zero, clamped");
        check(near(c.avatar_size, 1.25), "spaces around a value are ignored");
    }
    // The table and the clamp helper agree, for every key.
    {
        size_t count = 0;
        const vocem::Config::Number* table = vocem::Config::numbers(count);
        // Against the bound as the float the setting is: every member is a
        // float and clamp() answers in one, so avatar_idle_opacity's 0.1 comes
        // back as 0.100000001 -- the first bound in the table that a float
        // cannot hold exactly, and the one that showed this compared doubles.
        bool agree = count == 16;
        for (size_t i = 0; i < count; ++i) {
            agree = agree &&
                    vocem::Config::clamped(table[i].key, -1e9) ==
                        static_cast<double>(static_cast<float>(table[i].low)) &&
                    vocem::Config::clamped(table[i].key, 1e9) ==
                        static_cast<double>(static_cast<float>(table[i].high));
        }
        check(agree, "Config::clamped answers every table row with that row's bounds");
        check(vocem::Config::clamped("no_such_key", 42.0) == 42.0,
              "and an unknown key with the value untouched");
    }

    // Booleans, in the cases a person writes them.
    {
        const vocem::Config c = loaded_from(
            "enabled = True\npanel_enabled = YES\nnotifications_enabled = on\n"
            "only_speaking = 1\nhide_self = false\nshow_muted_state = off\ntext_shadow = maybe\n");
        check(c.enabled, "True with a capital is true: it used to switch the overlay OFF");
        check(c.panel_enabled, "YES too");
        check(c.notifications_enabled, "and on");
        check(c.only_speaking, "and 1");
        check(!c.hide_self, "false is false");
        check(!c.show_muted_state, "off is false");
        check(!c.text_shadow, "and a word that is neither is false");
    }

    // Quotes, and the words that keep their value on a typo.
    {
        const vocem::Config c = loaded_from(
            "font_path = \"/a b/x.ttf\"\nfont_family = \"Fira Sans\"\nshown_apps = \"one, two\"\n"
            "panel_layout = sideways\npanel_box = names\nnotification_corner = top\n"
            "text_idle_colour = automatic\ntext_speaking_colour = auto\n"
            "panel_colour = #12345\n");
        check(c.font_path == "/a b/x.ttf",
              "a quoted path loses both quotes, not only the closing one");
        check(c.font_family == "Fira Sans", "and so does a quoted family");
        check(c.shown_apps == "one, two", "and a quoted list");
        check(c.panel_layout == vocem::Config::kLayoutVertical,
              "a layout that is not a layout keeps the current one");
        check(c.panel_box == vocem::Config::kBoxNames, "the box's word is read whole");
        check(c.notification_corner == 1,
              "a corner that is not a number keeps the current one rather than going top-left");
        check(c.text_speaking_colour == vocem::Config::kColourAuto, "'auto' is auto");
        check(c.panel_colour == vocem::Config{}.panel_colour,
              "five hex digits are not a colour: the current one is kept");
    }
    {
        const vocem::Config c = loaded_from("notification_corner = 3\n");
        check(c.notification_corner == 3, "a corner in range is taken");
    }
    {
        // A pinned colour first, then a word that is not 'auto': the pin
        // stays. (On a fresh Config the fallback IS auto, which is why this
        // is asked of a loaded one.)
        loaded_from("text_idle_colour = #112233\n");
        vocem::Config c;
        c.load();
        if (std::FILE* file = std::fopen(vocem::Config::path().c_str(), "w")) {
            std::fputs("text_idle_colour = automatic\n", file);
            std::fclose(file);
        }
        c.load();
        check(c.text_idle_colour == 0x112233u,
              "'automatic' is not 'auto': the whole word or the current value");
    }

    // Comments, sections, CRLF and a key with no value.
    {
        const vocem::Config c = loaded_from(
            "# opacity = 0.1\r\n[appearance]\r\nopacity = 0.3\r\n; scale = 2\r\nscale =\r\n");
        check(near(c.opacity, 0.3), "a CRLF file reads like an LF one, comments skipped");
        check(near(c.scale, 0.5), "a key with nothing after the equals reads as zero, clamped");
    }

    // The switches, on top of the file as it stands.
    {
        loaded_from("opacity = 0.33\nhidden_apps = foreign_game\nenabled = true\n");
        vocem::Config written;
        check(vocem::Config::write_switch(&vocem::Config::enabled, false, &written),
              "a switch is written");
        const vocem::Config c = loaded_from(file_text().c_str());
        check(!c.enabled && c.panel_enabled && c.notifications_enabled,
              "and reads back as written, the other two as the file had them");
        check(near(c.opacity, 0.33) && c.hidden_apps == "foreign_game",
              "with every other key the file carried kept -- the file was loaded fresh, not "
              "written from a copy");
        check(written.hidden_apps == "foreign_game", "and the caller is told what was saved");
    }

    // A directory that cannot be written: save() says so.
    {
        const std::string sealed = g_root + "/sealed";
        vocem::make_directories(sealed + "/vocem");
        ::chmod((sealed + "/vocem").c_str(), 0500);
        setenv("XDG_CONFIG_HOME", sealed.c_str(), 1);
        vocem::Config c;
        const bool saved = c.save();
        if (::geteuid() == 0) {
            std::printf("     (running as root, where an unwritable directory is writable)\n");
        } else {
            check(!saved, "save() into an unwritable directory answers false, never true");
        }
        ::chmod((sealed + "/vocem").c_str(), 0700);
        setenv("XDG_CONFIG_HOME", root, 1);
    }

    char cleanup[600];
    std::snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    (void)!system(cleanup);
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
