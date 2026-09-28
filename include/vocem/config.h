// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// User settings, shared by everything that needs them.
//
// The file is a plain INI at $XDG_CONFIG_HOME/vocem/config.ini, written by the
// GUI and read by the injected code. Header-only and dependency-free, because the
// in-game side must not pull in a parser library.
//
// Read at initialisation and at most every couple of seconds afterwards, never
// per frame. Unknown keys are ignored and missing ones take the defaults below,
// so a file written by a newer version never breaks an older overlay.
//
// Numbers are written and parsed without LC_NUMERIC: Qt6 leaves it at the
// user's locale while the injected code and the daemon run in the C locale, so
// "0,82" and "0.82" would each read as 0 on the other side.

#ifndef VOCEM_CONFIG_H
#define VOCEM_CONFIG_H

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vocem/flatpak.h"
#include "vocem/paths.h"

namespace vocem {

struct Config {
    // The panel's top-left corner, as a fraction of the output, so it means the
    // same thing at any resolution. A fraction is a place between the margins
    // (vocem/placement.h), clamped at draw time so the panel stays on screen:
    // 1.0 is "against the far edge", and 0.5 centres the panel on that side.
    // Left side, half way down, is the owner's chosen default.
    float position_x = 0.0f;
    float position_y = 0.5f;

    // Appearance
    float scale = 1.0f;           // user scale on top of the automatic DPI scale

    // Which way the people are laid out inside the panel: one under the next
    // (the default: a participant list, the same width whoever joins), or one
    // beside the next, for an overlay along an edge of the screen. Only the
    // direction the cells advance and the panel runs out of room in changes.
    // Written as a word; a file without the key gets the column.
    static constexpr int kLayoutVertical = 0;
    static constexpr int kLayoutHorizontal = 1;
    int panel_layout = kLayoutVertical;

    // The layout, by its name, or a number as a script is likely to write it;
    // anything else keeps the current value rather than turning a panel
    // sideways over a typo. Public so the file, the window's bridge and the
    // geometry measurement's labels spell it the same way.
    static int to_layout(const char* text, int fallback) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        // The whole word, not a prefix, so a typo ("horizontalq") keeps the
        // current value. trim() has already taken the trailing side off.
        if (std::strcmp(text, "horizontal") == 0 || std::strcmp(text, "1") == 0) {
            return kLayoutHorizontal;
        }
        if (std::strcmp(text, "vertical") == 0 || std::strcmp(text, "0") == 0) {
            return kLayoutVertical;
        }
        return fallback;
    }

    static const char* layout_text(int layout) {
        return layout == kLayoutHorizontal ? "horizontal" : "vertical";
    }

    // Where the panel's surface is drawn: around everything, or behind each name
    // only -- a pill under the text, the pictures straight on the game. Same
    // surface and opacity either way; only the shape changes, hence one setting
    // beside the colour.
    //
    // "Behind the names" is the default and the preset the defaults are built on
    // (kPresets in theme.h): it keeps the contrast floor where the text is (88%
    // of #17181c under the glyphs, held to 4.5:1 by tests/theme_contrast.cpp)
    // and leaves the rest of the picture to the player. Written as a word; a
    // file without the key gets the pill, as a fresh install does.
    static constexpr int kBoxPanel = 0;
    static constexpr int kBoxNames = 1;
    int panel_box = kBoxNames;

    // The same reading as to_layout(): the whole word, or a number, and anything
    // else keeps the current value.
    static int to_box(const char* text, int fallback) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        if (std::strcmp(text, "names") == 0 || std::strcmp(text, "1") == 0) {
            return kBoxNames;
        }
        if (std::strcmp(text, "panel") == 0 || std::strcmp(text, "0") == 0) {
            return kBoxPanel;
        }
        return fallback;
    }

    static const char* box_text(int box) { return box == kBoxNames ? "names" : "panel"; }

    // The two boxes have their own colour and opacity: the voice panel stays for
    // the whole session and can be quiet, while a message has to be read once,
    // quickly. 0xRRGGBB; transparency is the separate opacity, so a colour never
    // carries an alpha.
    //
    // A neutral dark surface in the family of Discord's chat surfaces, not its
    // blurple: on the blurple the palette's names fall below the 4.5:1 floor
    // (tests/theme_contrast.cpp holds every boxed preset to it). The blurple is an
    // accent here, like the bar down the message box's edge.
    uint32_t panel_colour = 0x17181c;
    // The same surface as the panel's. Unlike the panel's, this box's surface is
    // drawn at full opacity by default: a message has to be read at first glance.
    uint32_t notification_colour = 0x17181c;
    // The ring drawn around whoever is speaking.
    uint32_t speaking_colour = 0x23a55a;     // Discord's green

    // The three text colours a user may pin: an idle name, the speaking name,
    // and the message's text. kColourAuto, the default, keeps theme.h's ramp,
    // chosen by the box's own lightness; a picked value overrides that one role
    // on any box, because the user has the last word about legibility.
    static constexpr uint32_t kColourAuto = 0xffffffffu;
    uint32_t text_idle_colour = kColourAuto;
    uint32_t text_speaking_colour = kColourAuto;
    uint32_t notification_text_colour = kColourAuto;

    // Drawn on the pill behind each name, not on a box around the whole panel
    // (panel_box above); the composite tables in tests/theme_contrast.cpp are
    // about this surface at this opacity.
    float opacity = 0.88f;        // voice panel background opacity

    // Below this the background is effectively gone and only the text is left.
    // A legitimate choice, but one the interface says out loud: a zero opacity
    // looks exactly like a broken overlay.
    static constexpr float kFaintBackground = 0.15f;

    // Half again as large as the text line: at 1.0 a face is too small to
    // recognise at a glance, which is the only thing an avatar is there for.
    float avatar_size = 1.5f;     // multiplier on the avatar diameter

    // How strongly the picture of somebody who is not talking shows, quieted
    // with the name as Discord's own overlay does. 55% is the step the name
    // takes on the default palette: idle #b5bac1 carries 54% of the light of
    // speaking #f2f3f5. 1.0 lights every picture. Not below 10%: ImGui culls
    // alpha 0, a missing face reads as a broken avatar, and hiding whoever is
    // quiet is only_speaking.
    float avatar_idle_opacity = 0.55f;

    // Whether the text carries its outline, permanently while on (see
    // kTextOutlineStrength in theme.h). Off by default on the owner's judgement:
    // clean glyphs straight on the game, with this switch and the presets for a
    // scene that washes the names out. The INI key keeps its old name, so older
    // files keep their answer.
    bool text_shadow = false;
    // Off by default, same judgement: out of the box the overlay is the people,
    // not the room they are in.
    bool show_channel_name = false;

    // Notifications. Direct messages and mentions appear as a toast in one of
    // the four corners: something that comes and goes should always appear in
    // the same place.
    bool notifications_enabled = true;
    int notification_corner = 1;      // 0 TL, 1 TR, 2 BL, 3 BR
    float notification_seconds = 5.0f;
    // Its own opacity, solid by default: a message is there to be read once,
    // quickly, over whatever is behind it. Still adjustable.
    float notification_opacity = 1.0f;
    // The message's own size, so it can be readable without making the voice panel
    // large as well.
    float notification_scale = 1.0f;
    // The message's text has no setting: it is always drawn. The words travel in
    // their own segment, created when a message arrives and removed when the
    // toast ends (vocem/note.h).

    // Spacing, in pixels at the reference text size of 16, scaled with everything
    // else at draw time.
    float screen_margin = 16.0f;    // the panel, from the edge of the display
    // The message's own distance from the edge, placed by the same arithmetic as
    // the panel's (vocem/placement.h). One setting each, because the two boxes
    // are positioned independently (entry 17).
    float notification_margin = 16.0f;
    float box_padding_x = 4.0f;     // inside either box, left and right
    float box_padding_y = 4.0f;     // inside either box, top and bottom
    float avatar_gap = 4.0f;        // between a picture and the name beside it
    float row_spacing = 0.0f;       // between one person and the next

    // The height of a line of text, in the same units. Separate from `scale`,
    // which scales the whole box: this changes the text against the pictures and
    // the padding. It is the reference the layout is expressed in, so the overlay
    // divides by it rather than by a constant (ui_scale() in fonts.h).
    float font_size = 16.0f;

    // The typeface. Empty, the default, is the carried Inter.
    //
    // The *window* resolves a family to files and writes both here; the injected
    // code only ever opens a path, because a game's process may not ask
    // fontconfig anything (a library, a cache, a config parse and file syscalls
    // inside somebody else's renderer). `font_family` is the name the window
    // shows; a path that has gone away falls back to Inter and says so in the log.
    std::string font_family;
    std::string font_path;
    std::string font_path_strong;

    // Voice
    bool only_speaking = false;   // hide participants who are silent
    bool hide_self = false;
    bool show_muted_state = true;

    // Behaviour. Two switches rather than one master: the voice panel and the
    // messages are separate features.
    bool panel_enabled = true;
    bool enabled = true;          // master switch, honoured by the injected code

    // What the configuration window does with itself; not read by the injected
    // code. Closing the window puts it away behind the tray icon by default: the
    // overlay is set up once and left running.
    bool keep_running = true;
    // Whether the window starts hidden with the session, through a desktop entry
    // under $XDG_CONFIG_HOME/autostart; this is the intent, that file the effect.
    bool start_at_login = false;

    // Whether the tray icon shows the user's own voice state (speaking, muted,
    // deafened, silent in a channel, not in one) or the application's picture.
    // On by default: "am I still muted" answered at a glance. Off is for a tray
    // that looks like every other, or a voice state not legible on a stream.
    bool tray_voice_icon = true;

    // Applications the user wants the overlay in although the detection cannot
    // tell they are games: launched from a script or a terminal, an emulator
    // started by hand.
    std::string shown_apps;

    // Applications the overlay must stay out of although they look like games.
    //
    // Both lists: comma separated, matched against the process name or the
    // executable's (whichever the window showed), empty by default so the file
    // records decisions rather than the machine. Launchers that need taking
    // back are in is_launcher() in apps.h, in code. The old key `gl_blacklist`
    // is still read as this one.
    std::string hidden_apps;

    // Flatpak application ids the daemon may serve the voice channel to, beside
    // the ones whose exported desktop entry says Game. Same list syntax, matched
    // against the id exactly (`org.example.Game`). Read by the daemon alone: a
    // sandbox's own request is its word, and this is the user's
    // (daemon/src/flatpak_bridge.cpp).
    std::string flatpak_apps;

    // Which display each map in the window depicts, as the connector's name
    // ("DP-2", the card<N>- prefix stripped). Empty, the default, is the largest
    // connected display, the one the overlay is sized for. Read by the window
    // only; one key per map, because the panel and the messages may live on
    // different screens.
    std::string preview_display_panel;
    std::string preview_display_notification;

    // Inside a Flatpak game this file is unreachable: XDG_CONFIG_HOME there is
    // the application's own directory under ~/.var/app and the user's settings
    // are not in it. The overlay reads the copy the daemon mirrored across the
    // bridge instead, and only the injected code asks for that (vocem/flatpak.h).
    static std::string path() {
        if (bridge_in_use()) {
            char mirrored[512];
            if (bridge_path(mirrored, sizeof(mirrored), kBridgeConfigName)) {
                return mirrored;
            }
        }
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            return std::string(xdg) + "/vocem/config.ini";
        }
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            // Nowhere, never the game's working directory (paths.h says why).
            return std::string(kNoHomeDirectory) + "/vocem/config.ini";
        }
        return std::string(home) + "/.config/vocem/config.ini";
    }

    // The file's modification time in nanoseconds, or 0 when it does not exist;
    // decides whether a reload is worth doing. Nanoseconds, because two writes in
    // one second must not look like one; `long long`, because `long` is 32 bits
    // in the 32-bit games this header is compiled into (entry 59).
    static long long mtime() {
        struct stat info{};
        if (stat(path().c_str(), &info) != 0) {
            return 0;
        }
        return static_cast<long long>(info.st_mtim.tv_sec) * 1000000000LL +
               static_cast<long long>(info.st_mtim.tv_nsec);
    }

    // Every numeric setting, with its bounds and the precision it is written at:
    // the one place the bounds are stated. load() and the window's setters
    // (Config::clamped) clamp by it, and tests/slider_bounds.cmake holds every
    // slider to it (entry 136).
    struct Number {
        const char* key;
        float Config::*member;
        double low;
        double high;
        int decimals;
    };
    static const Number* numbers(size_t& count) {
        static const Number table[] = {
            {"position_x", &Config::position_x, 0.0, 1.0, 4},
            {"position_y", &Config::position_y, 0.0, 1.0, 4},
            {"scale", &Config::scale, 0.5, 3.0, 2},
            {"opacity", &Config::opacity, 0.0, 1.0, 2},
            {"avatar_size", &Config::avatar_size, 0.5, 2.0, 2},
            {"avatar_idle_opacity", &Config::avatar_idle_opacity, 0.1, 1.0, 2},
            {"notification_seconds", &Config::notification_seconds, 1.0, 30.0, 1},
            {"notification_opacity", &Config::notification_opacity, 0.0, 1.0, 2},
            {"notification_scale", &Config::notification_scale, 0.5, 3.0, 2},
            {"screen_margin", &Config::screen_margin, 0.0, 120.0, 1},
            {"notification_margin", &Config::notification_margin, 0.0, 120.0, 1},
            {"font_size", &Config::font_size, 8.0, 48.0, 1},
            {"box_padding_x", &Config::box_padding_x, 0.0, 48.0, 1},
            {"box_padding_y", &Config::box_padding_y, 0.0, 48.0, 1},
            {"avatar_gap", &Config::avatar_gap, 0.0, 48.0, 1},
            {"row_spacing", &Config::row_spacing, 0.0, 48.0, 1},
        };
        count = sizeof(table) / sizeof(table[0]);
        return table;
    }

    // `value` held to the bounds of the numeric setting `key`. An unknown key
    // is a programming error and is answered with the value untouched, so the
    // caller's own test sees the number it did not expect.
    static double clamped(const char* key, double value) {
        size_t count = 0;
        const Number* table = numbers(count);
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(table[i].key, key) == 0) {
                return clamp(value, table[i].low, table[i].high);
            }
        }
        return value;
    }

    // One of the three switches the window writes the moment it is clicked, put
    // on top of the file as it stands on disk: that one key changed and nothing
    // else, so keys edited outside the window since startup, and the other two
    // switches, are not put back (entries 136, 203). `which` is
    // &Config::enabled, &Config::panel_enabled or &Config::notifications_enabled;
    // `written`, when given, receives what the file now says. Through
    // edit_file(), like every write.
    static bool write_switch(bool Config::*which, bool value, Config* written = nullptr,
                             std::string* why = nullptr) {
        return edit_file([which, value](Config& disk) { disk.*which = value; }, written, why);
    }

    // The window's Apply: the settings the window changed (`mine` against
    // `followed`, the file as the window last followed it) put on top of the
    // file as it stands, every other key keeping the file's value
    // (merged(), entry 272). start_at_login is the window's whatever the file
    // says: it is read from the autostart entry, not from here.
    static bool write_edit(const Config& followed, const Config& mine, Config* written = nullptr,
                           std::string* why = nullptr) {
        return edit_file(
            [&followed, &mine](Config& disk) {
                disk = merged(followed, mine, disk);
                disk.start_at_login = mine.start_at_login;
            },
            written, why);
    }

    // The settings in the file, over the defaults. False when there is a file
    // and it could not be read whole: not a regular file (a FIFO would hold
    // the reader for ever, /dev/zero never ends), not readable, or a read that
    // failed part way. What was read before the failure has been applied; a
    // caller that goes on to write must not trust it, and edit_file() does
    // not. A missing file is not a failure: defaults are a perfectly good
    // configuration.
    bool load() {
        std::FILE* file = nullptr;
        const int found = open_settings(path(), file, nullptr, nullptr);
        if (found <= 0) {
            return found == 0;
        }
        std::string line;
        while (read_line(file, line)) {
            take_line(line);
        }
        const bool whole = !std::ferror(file);
        std::fclose(file);
        return whole;
    }

    // The settings in `text`, a whole file's bytes, over the current values:
    // exactly what load() makes of the same bytes (the same lines dropped for
    // length, the same reading of each). The writer reads the file once and
    // parses what it read with this.
    void parse(const std::string& text) {
        std::string line;
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            end = end == std::string::npos ? text.size() : end + 1;  // with its newline
            if (end - at <= kMaxLineBytes) {
                line.assign(text, at, end - at);
                take_line(line);
            }
            at = end;
        }
    }

    // One setting, from the text a file line carries for it (already trimmed).
    // An unknown key changes nothing. Every line read goes through this, and
    // merged() puts one key of another copy through it.
    void assign(const char* key, const char* value) {
        // The numbers, by the table above.
        {
            size_t count = 0;
            const Number* table = numbers(count);
            for (size_t i = 0; i < count; ++i) {
                if (std::strcmp(key, table[i].key) == 0) {
                    this->*(table[i].member) =
                        clamp(to_number(value), table[i].low, table[i].high);
                    return;
                }
            }
        }
        if (std::strcmp(key, "panel_colour") == 0) {
            panel_colour = to_colour(value, panel_colour);
        } else if (std::strcmp(key, "notification_colour") == 0) {
            notification_colour = to_colour(value, notification_colour);
        } else if (std::strcmp(key, "text_idle_colour") == 0) {
            text_idle_colour = to_colour_or_auto(value, text_idle_colour);
        } else if (std::strcmp(key, "text_speaking_colour") == 0) {
            text_speaking_colour = to_colour_or_auto(value, text_speaking_colour);
        } else if (std::strcmp(key, "notification_text_colour") == 0) {
            notification_text_colour = to_colour_or_auto(value, notification_text_colour);
        } else if (std::strcmp(key, "speaking_colour") == 0) {
            speaking_colour = to_colour(value, speaking_colour);
        } else if (std::strcmp(key, "accent_background") == 0) {
            // Retired and deliberately not migrated: it switched between two
            // fixed colours, and both boxes now have a colour of their own.
        } else if (std::strcmp(key, "panel_layout") == 0) {
            panel_layout = to_layout(value, panel_layout);
        } else if (std::strcmp(key, "panel_box") == 0) {
            panel_box = to_box(value, panel_box);
        } else if (std::strcmp(key, "keep_running") == 0) {
            keep_running = as_bool(value);
        } else if (std::strcmp(key, "start_at_login") == 0) {
            start_at_login = as_bool(value);
        } else if (std::strcmp(key, "tray_voice_icon") == 0) {
            tray_voice_icon = as_bool(value);
        } else if (std::strcmp(key, "text_shadow") == 0) {
            text_shadow = as_bool(value);
        } else if (std::strcmp(key, "show_channel_name") == 0) {
            show_channel_name = as_bool(value);
        } else if (std::strcmp(key, "notifications_enabled") == 0) {
            notifications_enabled = as_bool(value);
        } else if (std::strcmp(key, "notification_corner") == 0) {
            // A number 0-3, or the current value kept: atoi() would read a word
            // as 0, top-left.
            notification_corner = to_corner(value, notification_corner);
        } else if (std::strcmp(key, "font_family") == 0) {
            font_family = value;
        } else if (std::strcmp(key, "font_path") == 0) {
            font_path = value;
        } else if (std::strcmp(key, "font_path_strong") == 0) {
            font_path_strong = value;
        } else if (std::strcmp(key, "only_speaking") == 0) {
            only_speaking = as_bool(value);
        } else if (std::strcmp(key, "hide_self") == 0) {
            hide_self = as_bool(value);
        } else if (std::strcmp(key, "show_muted_state") == 0) {
            show_muted_state = as_bool(value);
        } else if (std::strcmp(key, "panel_enabled") == 0) {
            panel_enabled = as_bool(value);
        } else if (std::strcmp(key, "enabled") == 0) {
            enabled = as_bool(value);
        } else if (std::strcmp(key, "shown_apps") == 0) {
            shown_apps = value;
        } else if (std::strcmp(key, "hidden_apps") == 0 ||
                   std::strcmp(key, "gl_blacklist") == 0) {
            hidden_apps = value;
        } else if (std::strcmp(key, "flatpak_apps") == 0) {
            flatpak_apps = value;
        } else if (std::strcmp(key, "preview_display_panel") == 0) {
            preview_display_panel = value;
        } else if (std::strcmp(key, "preview_display_notification") == 0) {
            preview_display_notification = value;
        }
    }

    // One line of the file: the section it is written under when the file
    // does not already carry it, the key, and the value as text.
    struct Entry {
        const char* section;
        const char* key;
        std::string value;
    };

    // Everything the window writes, in the order and under the sections a new
    // file gets them.
    std::vector<Entry> entries() const {
        const auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
        return {
            {"position", "position_x", decimal(position_x, 4)},
            {"position", "position_y", decimal(position_y, 4)},
            {"appearance", "scale", decimal(scale, 2)},
            {"appearance", "panel_layout", layout_text(panel_layout)},
            {"appearance", "panel_box", box_text(panel_box)},
            {"appearance", "panel_colour", colour_text(panel_colour)},
            {"appearance", "speaking_colour", colour_text(speaking_colour)},
            {"appearance", "text_idle_colour", colour_or_auto_text(text_idle_colour)},
            {"appearance", "text_speaking_colour", colour_or_auto_text(text_speaking_colour)},
            {"appearance", "opacity", decimal(opacity, 2)},
            {"appearance", "avatar_size", decimal(avatar_size, 2)},
            {"appearance", "avatar_idle_opacity", decimal(avatar_idle_opacity, 2)},
            {"appearance", "text_shadow", flag(text_shadow)},
            {"appearance", "show_channel_name", flag(show_channel_name)},
            {"notifications", "notifications_enabled", flag(notifications_enabled)},
            {"notifications", "notification_corner", std::to_string(notification_corner)},
            {"notifications", "notification_seconds", decimal(notification_seconds, 1)},
            {"notifications", "notification_opacity", decimal(notification_opacity, 2)},
            {"notifications", "notification_colour", colour_text(notification_colour)},
            {"notifications", "notification_text_colour",
             colour_or_auto_text(notification_text_colour)},
            {"notifications", "notification_scale", decimal(notification_scale, 2)},
            {"spacing", "screen_margin", decimal(screen_margin, 1)},
            {"spacing", "notification_margin", decimal(notification_margin, 1)},
            {"spacing", "font_size", decimal(font_size, 1)},
            {"spacing", "font_family", font_family},
            {"spacing", "font_path", font_path},
            {"spacing", "font_path_strong", font_path_strong},
            {"spacing", "box_padding_x", decimal(box_padding_x, 1)},
            {"spacing", "box_padding_y", decimal(box_padding_y, 1)},
            {"spacing", "avatar_gap", decimal(avatar_gap, 1)},
            {"spacing", "row_spacing", decimal(row_spacing, 1)},
            {"voice", "only_speaking", flag(only_speaking)},
            {"voice", "hide_self", flag(hide_self)},
            {"voice", "show_muted_state", flag(show_muted_state)},
            {"behaviour", "panel_enabled", flag(panel_enabled)},
            {"behaviour", "enabled", flag(enabled)},
            {"behaviour", "keep_running", flag(keep_running)},
            {"behaviour", "start_at_login", flag(start_at_login)},
            {"behaviour", "tray_voice_icon", flag(tray_voice_icon)},
            {"behaviour", "hidden_apps", hidden_apps},
            {"behaviour", "shown_apps", shown_apps},
            {"behaviour", "flatpak_apps", flatpak_apps},
            {"behaviour", "preview_display_panel", preview_display_panel},
            {"behaviour", "preview_display_notification", preview_display_notification},
        };
    }

    // The window's copy with the file brought in under it: `mine` is what the
    // window shows, `base` the file as it was when that copy last followed it,
    // `disk` the file now. A setting the window changed (its text in `mine` is
    // not its text in `base`) keeps the window's value; every other takes the
    // file's, so keys changed outside the window while an edit waits for Apply
    // (flatpak_apps has no control at all) are not put back. Compared as the
    // text the file would carry, the precision a value has once written.
    static Config merged(const Config& base, const Config& mine, const Config& disk) {
        Config result = mine;
        const std::vector<Entry> before = base.entries();
        const std::vector<Entry> window = mine.entries();
        const std::vector<Entry> now = disk.entries();
        for (size_t i = 0; i < window.size(); ++i) {
            if (window[i].value == before[i].value && now[i].value != window[i].value) {
                result.assign(now[i].key, now[i].value.c_str());
            }
        }
        return result;
    }

    // ---- The one writer. -------------------------------------------------
    //
    // Only the window writes, and every write it makes -- Apply, the three
    // instant switches, save() -- goes through edit_file(), which holds the
    // whole policy in one place:
    //   * The file is read whole, once, or not written. A file that is there
    //     and cannot be read (mode 000, a directory, a FIFO, a device, a read
    //     that fails part way) is not an empty one (entry 273), and a
    //     read-only file says it is not to be written.
    //   * `edit` gets the settings that read holds and changes what it means
    //     to; nothing else is taken from anywhere.
    //   * The file is rewritten IN PLACE (rewritten()): only the lines of the
    //     settings whose value changed are touched; comments, blank lines,
    //     sections, keys of a newer version, bytes that are not UTF-8, a
    //     known key's line too long for the reader (entry 277), and every
    //     setting that did not change stay byte for byte.
    //   * A link stays a link, however it ends (write_target, entry 276); the
    //     file at its end is what is written.
    //   * Written to a temporary beside that file and renamed over it: a game
    //     reloads on the mtime, which would already be final while it read a
    //     half-written file. The temporary takes the file's mode and is
    //     fsync'd, so a crash leaves the old file or the new one.
    // `written` receives the settings the file now holds; `why`, when the
    // write is refused, says why in words the window can show.
    template <class Edit>
    static bool edit_file(Edit edit, Config* written = nullptr, std::string* why = nullptr) {
        const std::string file_path = path();
        const size_t slash = file_path.rfind('/');
        if (slash != std::string::npos) {
            make_directories(file_path.substr(0, slash));
        }
        const std::string target = write_target(file_path);
        if (target.empty()) {
            return refuse(why, "it is a loop of links");
        }
        std::FILE* file = nullptr;
        struct stat existing{};
        const int found = open_settings(target, file, &existing, why);
        if (found < 0) {
            return false;
        }
        std::string current;
        if (found > 0) {
            char chunk[4096];
            size_t got = 0;
            while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                current.append(chunk, got);
            }
            const bool whole = !std::ferror(file);
            std::fclose(file);
            if (!whole) {
                return refuse(why, "it could not be read to its end");
            }
        }
        Config result;
        result.parse(current);
        edit(result);
        if (written) {
            *written = result;
        }
        const std::string text = result.rewritten(current);
        if (found > 0 && text == current) {
            return true;  // nothing to change: the file already says it
        }
        if (found > 0 && ::access(target.c_str(), W_OK) != 0) {
            return refuse(why, "it is read-only");
        }

        const std::string temporary_path = target + ".tmp";
        int fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0 && errno == EEXIST) {
            // Left by a write that died before its rename.
            ::unlink(temporary_path.c_str());
            fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        }
        if (fd < 0) {
            return refuse(why, "no file could be created beside it");
        }
        bool complete = found == 0 || ::fchmod(fd, existing.st_mode & 07777) == 0;
        for (size_t done = 0; complete && done < text.size();) {
            const ssize_t wrote = ::write(fd, text.data() + done, text.size() - done);
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            complete = wrote > 0;
            done += complete ? static_cast<size_t>(wrote) : 0;
        }
        complete = complete && ::fsync(fd) == 0;
        complete = ::close(fd) == 0 && complete;
        if (!complete || std::rename(temporary_path.c_str(), target.c_str()) != 0) {
            ::unlink(temporary_path.c_str());
            return refuse(why, "the new text could not be written");
        }
        const size_t target_slash = target.rfind('/');
        const std::string directory = target_slash == std::string::npos
                                          ? std::string(".")
                                          : target.substr(0, target_slash ? target_slash : 1);
        const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
        return true;
    }

    // This whole copy, written over the file: every setting takes this copy's
    // value. Through edit_file(), so the file's own lines stay as they are.
    bool save(std::string* why = nullptr) const {
        return edit_file([this](Config& disk) { disk = *this; }, nullptr, why);
    }

    // `current` -- the file as it stands, possibly empty -- with this
    // configuration written into it: see edit_file(). A setting whose value
    // the file already carries (as the reader takes it) is not touched. One
    // that changed is written over the LAST line the reader takes for it,
    // which is the one that decides, keeping that line's CR if it had one;
    // earlier copies are left, overruled as they already were. A setting with
    // no line at all is added at the end of its section, or in a section of
    // its own at the end; one whose only line is too long for the reader gets
    // this copy's value after that line, and nothing when the value is empty
    // (entry 277). Public so a test can hold the rewrite to its promise
    // without a file.
    std::string rewritten(const std::string& current) const {
        static const char* const kHeader = "# Written by vocem-config. Edits are picked up live.";
        const std::vector<Entry> wanted = entries();
        Config disk;
        disk.parse(current);
        const std::vector<Entry> has = disk.entries();
        const auto index_of = [&wanted](const std::string& key) -> int {
            // `gl_blacklist` is the old name of the same setting.
            const std::string name = key == "gl_blacklist" ? std::string("hidden_apps") : key;
            for (size_t i = 0; i < wanted.size(); ++i) {
                if (name == wanted[i].key) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        // A value the reader would trim -- a space or tab at either end, or a
        // pair of quotes around it -- is written inside quotes, which the
        // reader takes off again (trim()).
        const auto line_for = [](const Entry& entry) {
            const std::string& value = entry.value;
            const auto edge = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            const bool quoted = !value.empty() &&
                                (edge(value.front()) || edge(value.back()) ||
                                 (value.size() >= 2 && value.front() == '"' && value.back() == '"'));
            return std::string(entry.key) + " = " + (quoted ? "\"" + value + "\"" : value);
        };

        // The lines (without their newline), with the section each belongs to.
        std::vector<std::string> lines;
        std::vector<std::string> sections;
        bool final_newline = true;
        // Per setting: the last line the reader takes for it, and the last
        // line it refuses for length.
        std::vector<int> taken(wanted.size(), -1);
        std::vector<int> refused(wanted.size(), -1);
        std::string section;
        for (size_t at = 0; at < current.size();) {
            size_t end = current.find('\n', at);
            final_newline = end != std::string::npos;
            if (!final_newline) {
                end = current.size();
            }
            std::string line = current.substr(at, end - at);
            // The bytes read_line() counts: the line and its newline.
            const size_t read_length = line.size() + (final_newline ? 1 : 0);
            at = end + 1;
            std::string trimmed = trim_copy(line);
            if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
                section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            } else {
                std::string key;
                if (setting_key(line, key)) {
                    const int known = index_of(key);
                    if (known >= 0) {
                        (read_length > kMaxLineBytes ? refused : taken)[static_cast<size_t>(known)] =
                            static_cast<int>(lines.size());
                    }
                }
            }
            lines.push_back(line);
            sections.push_back(section);
        }

        // The changes: a line rewritten, or a line to add after line j
        // (after[j + 1]) or at the end (tail).
        std::vector<std::vector<std::string>> after(lines.size() + 1);
        std::vector<std::string> tail;
        for (size_t i = 0; i < wanted.size(); ++i) {
            const bool same = wanted[i].value == has[i].value;
            if (taken[i] >= 0) {
                if (!same) {
                    std::string& line = lines[static_cast<size_t>(taken[i])];
                    const bool cr = !line.empty() && line.back() == '\r';
                    line = line_for(wanted[i]) + (cr ? "\r" : "");
                }
                continue;
            }
            if (refused[i] >= 0) {
                // The reader takes the default for this key, so a line is
                // added only when this copy's value is not that.
                if (!same) {
                    after[static_cast<size_t>(refused[i]) + 1].push_back(line_for(wanted[i]));
                }
                continue;
            }
            // No line at all: added, so the file says everything the window
            // knows.
            size_t anchor = lines.size();
            bool found = false;
            for (size_t j = lines.size(); j-- > 0;) {
                if (sections[j] == wanted[i].section) {
                    if (!found) {
                        anchor = j;
                        found = true;
                    }
                    if (!trim_copy(lines[j]).empty()) {
                        anchor = j;
                        break;
                    }
                }
            }
            if (found) {
                after[anchor + 1].push_back(line_for(wanted[i]));
                continue;
            }
            const std::string header = std::string("[") + wanted[i].section + "]";
            if (std::find(tail.begin(), tail.end(), header) == tail.end()) {
                if (lines.empty() && tail.empty()) {
                    tail.push_back(kHeader);
                } else {
                    tail.push_back(std::string());
                }
                tail.push_back(header);
            }
            // Keys of one section arrive together (entries() is grouped), so
            // the end of the tail is this section.
            tail.push_back(line_for(wanted[i]));
        }

        std::string text;
        for (size_t j = 0; j <= lines.size(); ++j) {
            for (const std::string& extra : after[j]) {
                text += extra;
                text += '\n';
            }
            if (j < lines.size()) {
                text += lines[j];
                // The last line keeps its lack of a newline when nothing follows it.
                const bool last = j + 1 == lines.size() && after[j + 1].empty() && tail.empty();
                if (!last || final_newline) {
                    text += '\n';
                }
            }
        }
        for (const std::string& line : tail) {
            text += line;
            text += '\n';
        }
        return text;
    }

    // True when the background has been turned down far enough to disappear. The
    // interface uses it to explain what is about to happen rather than leaving the
    // user with a panel that looks broken.
    bool background_is_faint() const { return opacity < kFaintBackground; }

    // The same question about the message box, which has an opacity of its own:
    // the sentence under each slider comes from that box's value.
    bool notification_background_is_faint() const {
        return notification_opacity < kFaintBackground;
    }

private:
    // The longest single line this reader keeps: not a defence against a hostile
    // file (it is the user's own) but a bound on the memory a line can take
    // inside somebody's game. 64 KiB is about four thousand process names.
    static constexpr size_t kMaxLineBytes = 64 * 1024;

    // Reads one whole line, however long it is, NUL bytes included. A line
    // past the cap is consumed to its end and dropped rather than kept in
    // pieces: a truncated value is worse than none, because the window would
    // save the truncation back (a short `hidden_apps` gives hidden
    // applications the overlay again). Byte by byte rather than fgets():
    // fgets() cannot say where a line holding a NUL ends, and one did not --
    // the line after it was read as part of it.
    static bool read_line(std::FILE* file, std::string& line) {
        line.clear();
        bool any = false;
        size_t length = 0;
        for (int c = getc_unlocked(file); c != EOF; c = getc_unlocked(file)) {
            any = true;
            if (++length <= kMaxLineBytes) {
                line.push_back(static_cast<char>(c));
            } else if (!line.empty()) {
                line.clear();
                line.shrink_to_fit();
            }
            if (c == '\n') {
                break;
            }
        }
        return any;
    }

    // One line as the reader takes it: a setting is the text before the first
    // '=' on a line that does not open with '#', '[' or ';', read as a C
    // string (a NUL ends it). The one reading of a line, used by the reader
    // (take_line) and by the rewrite (setting_key), so the two cannot
    // disagree about which line carries which key.
    static bool split_setting(char* text, const char** key, const char** value) {
        char* equals = std::strchr(text, '=');
        if (!equals || text[0] == '#' || text[0] == '[' || text[0] == ';') {
            return false;
        }
        *equals = '\0';
        *key = trim(text);
        *value = trim(equals + 1);
        return true;
    }

    void take_line(std::string& line) {
        // `data()` is non-const from C++17; the split writes terminators.
        const char* key = nullptr;
        const char* value = nullptr;
        if (split_setting(line.data(), &key, &value)) {
            assign(key, value);
        }
    }

    static bool setting_key(std::string line, std::string& key) {
        const char* name = nullptr;
        const char* value = nullptr;
        if (!split_setting(line.data(), &name, &value)) {
            return false;
        }
        key = name;
        return true;
    }

    // Opens the settings file for reading, or says why not: 1 and `file` set
    // when it is a regular file that could be opened; 0 when there is none
    // (errno ENOENT, `file` null); -1 otherwise, `why` saying what is there.
    // O_NONBLOCK is load-bearing: the S_ISREG check comes after the open, and
    // opening a FIFO without it waits for a writer that never comes -- in the
    // window, in the daemon, and in every game that reads the file.
    static int open_settings(const std::string& file_path, std::FILE*& file, struct stat* info,
                             std::string* why) {
        file = nullptr;
        const int fd = ::open(file_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) {
                return 0;
            }
            refuse(why, errno == EACCES ? "it cannot be read" : std::strerror(errno));
            return -1;
        }
        struct stat own{};
        if (::fstat(fd, &own) != 0 || !S_ISREG(own.st_mode)) {
            ::close(fd);
            refuse(why, "it is not a regular file");
            errno = EINVAL;
            return -1;
        }
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        file = ::fdopen(fd, "r");
        if (!file) {
            ::close(fd);
            refuse(why, std::strerror(errno));
            return -1;
        }
        if (info) {
            *info = own;
        }
        return 1;
    }

    static bool refuse(std::string* why, const char* reason) {
        if (why) {
            *why = reason;
        }
        return false;
    }

    // The file a write to `file_path` must land on: the path itself, or the end
    // of the links it is. realpath() fails for a link whose target does not
    // exist yet (dotfiles linked before the first write), so such a link is
    // followed here, a relative target against the link's own directory. Empty
    // for a chain that does not end (a loop), which save() refuses.
    static std::string write_target(const std::string& file_path) {
        if (char* resolved = ::realpath(file_path.c_str(), nullptr)) {
            std::string target = resolved;
            std::free(resolved);
            return target;
        }
        std::string current = file_path;
        for (int hop = 0; hop < 40; ++hop) {
            char buffer[4096];
            const ssize_t length = ::readlink(current.c_str(), buffer, sizeof(buffer) - 1);
            if (length < 0) {
                return current;  // not a link (or not there): written as it is
            }
            std::string next(buffer, static_cast<size_t>(length));
            if (next.empty() || next[0] != '/') {
                const size_t slash = current.rfind('/');
                next = (slash == std::string::npos ? std::string() : current.substr(0, slash + 1)) +
                       next;
            }
            current = next;
        }
        return std::string();
    }

    static std::string trim_copy(const std::string& text) {
        size_t from = 0;
        size_t to = text.size();
        while (from < to && (text[from] == ' ' || text[from] == '\t')) ++from;
        while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r'))
            --to;
        return text.substr(from, to - from);
    }

    static const char* trim(char* text) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        char* end = text + std::strlen(text);
        while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                              end[-1] == '\t')) {
            --end;
        }
        // A quoted value loses both quotes, not only the closing one.
        if (end - text >= 2 && text[0] == '"' && end[-1] == '"') {
            ++text;
            --end;
        }
        *end = '\0';
        return text;
    }

    // A corner: 0 to 3, or the current value for anything else.
    static int to_corner(const char* text, int fallback) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        if (text[0] >= '0' && text[0] <= '3' && text[1] == '\0') {
            return text[0] - '0';
        }
        return fallback;
    }

    // Decimal parsing that ignores LC_NUMERIC (entry 15). Both separators are accepted on
    // the way in, so files written with a comma keep their values. No exponents:
    // no setting is ever written in that form.
    static double to_number(const char* text) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        const bool negative = (*text == '-');
        if (*text == '-' || *text == '+') {
            ++text;
        }
        double whole = 0.0;
        while (*text >= '0' && *text <= '9') {
            whole = whole * 10.0 + static_cast<double>(*text - '0');
            ++text;
        }
        double fraction = 0.0;
        if (*text == '.' || *text == ',') {
            ++text;
            double weight = 0.1;
            while (*text >= '0' && *text <= '9') {
                fraction += static_cast<double>(*text - '0') * weight;
                weight *= 0.1;
                ++text;
            }
        }
        const double value = whole + fraction;
        return negative ? -value : value;
    }

    // The matching direction: a fixed-point decimal built with integer arithmetic, so
    // there is no printf conversion for a locale to reinterpret.
    static std::string decimal(double value, int decimals) {
        const bool negative = value < 0.0;
        if (negative) {
            value = -value;
        }
        long divisor = 1;
        for (int i = 0; i < decimals; ++i) {
            divisor *= 10;
        }
        // Half away from zero, which is what %f does for the values kept here.
        const long scaled = static_cast<long>(value * static_cast<double>(divisor) + 0.5);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s%ld.%0*ld", negative ? "-" : "",
                      scaled / divisor, decimals, scaled % divisor);
        return buffer;
    }

    // "#rrggbb", or the same without the hash. Anything that is not six hex
    // digits keeps the current value rather than turning the overlay an
    // unexplained colour.
    static uint32_t to_colour(const char* text, uint32_t fallback) {
        while (*text == ' ' || *text == '\t' || *text == '#') {
            ++text;
        }
        uint32_t value = 0;
        int digits = 0;
        for (; digits < 6; ++digits) {
            const char c = text[digits];
            uint32_t nibble;
            if (c >= '0' && c <= '9') {
                nibble = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                nibble = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fallback;
            }
            value = (value << 4) | nibble;
        }
        return value;
    }

    static std::string colour_text(uint32_t colour) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%06x", colour & 0xffffffu);
        return buffer;
    }

    // "auto" round-trips as the word, not as a colour: masking the sentinel to
    // 24 bits would turn "follow the ramp" into white and lose the automatism.
    static uint32_t to_colour_or_auto(const char* text, uint32_t fallback) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        // The whole word, as to_layout() reads its words: "automatic" and
        // "autox" are not it.
        if (std::strcmp(text, "auto") == 0) {
            return kColourAuto;
        }
        return to_colour(text, fallback);
    }

    static std::string colour_or_auto_text(uint32_t colour) {
        return colour == kColourAuto ? "auto" : colour_text(colour);
    }

    // true/yes/on/1, in any case: `enabled = True` is written by hand and by
    // scripts, and must not switch the overlay off.
    static bool as_bool(const char* value) {
        char lowered[8] = {0};
        for (size_t i = 0; i < sizeof(lowered) - 1 && value[i]; ++i) {
            lowered[i] = static_cast<char>(
                value[i] >= 'A' && value[i] <= 'Z' ? value[i] - 'A' + 'a' : value[i]);
        }
        if (value[0] && std::strlen(value) > sizeof(lowered) - 1) {
            return false;  // longer than any spelling of yes
        }
        return std::strcmp(lowered, "true") == 0 || std::strcmp(lowered, "1") == 0 ||
               std::strcmp(lowered, "yes") == 0 || std::strcmp(lowered, "on") == 0;
    }

    static float clamp(double value, double low, double high) {
        if (value < low) return static_cast<float>(low);
        if (value > high) return static_cast<float>(high);
        return static_cast<float>(value);
    }
};

}  // namespace vocem

#endif  // VOCEM_CONFIG_H
