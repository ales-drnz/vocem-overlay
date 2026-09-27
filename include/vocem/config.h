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
// Reading happens at initialisation and, at most, once every couple of seconds
// afterwards to pick up edits -- never per frame. Unknown keys are ignored and
// missing ones fall back to the defaults below, so a config written by a newer
// version never breaks an older overlay.
//
// Numbers are written and parsed without going through LC_NUMERIC. That is not a
// theoretical concern: Qt6 leaves LC_NUMERIC at the user's locale once
// QGuiApplication exists, while the injected code and the daemon run in the C
// locale, so on an Italian system the GUI wrote "opacity = 0,82" and the game
// process read it as 0 -- an invisible panel background with no error anywhere.
// The reverse direction corrupts just as quietly: a file written with a full stop
// comes back as 0 in the GUI, which then saves the zero. Both were observed.

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
    // The panel's top-left corner, as a fraction of the output, so it means the same
    // thing at any resolution. Values are clamped at draw time so the panel always
    // stays fully on screen whatever its height: 1.0 therefore reads as "against the
    // far edge" rather than "off the edge", which is what the corner presets use.
    // Left side, half way down: the owner's chosen default. A fraction is a place
    // between the margins (vocem/placement.h), so 0.5 centres the panel on that
    // side rather than putting its top edge at the middle.
    float position_x = 0.0f;
    float position_y = 0.5f;

    // Appearance
    float scale = 1.0f;           // user scale on top of the automatic DPI scale

    // Which way the people are laid out inside the panel: one under the next,
    // or one beside the next. A column is the default because that is what a
    // participant list is, and because it stays the same width whoever joins;
    // a row is for a player who wants the overlay along an edge of the screen,
    // out of the middle of the picture. It is a layout, not a second panel:
    // the same rows, the same settings, the same box -- only the direction the
    // cells advance in, and the direction the panel runs out of room in.
    //
    // Written as a word rather than a number, unlike notification_corner: this
    // one has an obvious spelling in the file and a number would have to be
    // looked up. A file from a version that had no layouts has no key and gets
    // the column, which is what it was drawing.
    static constexpr int kLayoutVertical = 0;
    static constexpr int kLayoutHorizontal = 1;
    int panel_layout = kLayoutVertical;

    // The layout, by its name. A number is accepted on the way in too, because
    // that is what a file written by a script is likely to carry, and anything
    // else keeps the current value rather than turning somebody's panel sideways
    // over a typo. Public because everything that names this setting -- the
    // file, the window's bridge, the geometry measurement's own labels -- must
    // spell it the same way.
    static int to_layout(const char* text, int fallback) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        // The whole word, not a prefix: the sentence above promises that a typo
        // keeps the current value, and a prefix match turns "horizontalq" -- and
        // "vertical panel" -- into a layout the user did not ask for. The
        // trailing side is already clean by the time this is called (trim()
        // takes it off), so a plain comparison is the strict reading.
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
    // and nothing else -- a pill under the text, the pictures straight on the
    // game. It is the same surface and the same opacity either way; what changes
    // is the shape they are put on, which is why this is one setting beside the
    // colour rather than a second colour of its own.
    //
    // "Behind the names" is the default, and the preset the defaults are built
    // on (kPresets in theme.h): it carries the measured floor where the text
    // actually is -- 88% of #17181c under the glyphs, the composite
    // tests/theme_contrast.cpp holds to 4.5:1 -- while leaving the rest of the
    // picture the player's. The whole-panel box is one chip away and is what
    // every release up to 0.1.6 drew.
    //
    // A word rather than a number, for panel_layout's reason: it has an obvious
    // spelling in the file. A file from a version that had no such setting has
    // no key, and gets the pill -- the same answer a fresh install gets, so the
    // default is one look and not two.
    static constexpr int kBoxPanel = 0;
    static constexpr int kBoxNames = 1;
    int panel_box = kBoxNames;

    // The same reading as to_layout(): the whole word, a number for a file
    // written by a script, and anything else keeps the current value rather than
    // moving somebody's box over a typo.
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

    // The two boxes are configured independently, in colour and in transparency,
    // because they are not the same thing: the voice panel sits there for the whole
    // session and can afford to be quiet, while a message arrives, has to be read
    // once, and leaves. Tying them together -- which an earlier version did, with a
    // single opacity and one switch choosing between two hardcoded colours -- meant
    // that making the panel discreet made the messages unreadable.
    //
    // 0xRRGGBB. Transparency is the separate opacity below, so a colour never
    // carries an alpha and the two can be changed without disturbing each other.
    //
    // A neutral dark surface, not Discord's blurple, which it was. The blurple is
    // what Discord paints *on* its near-black surfaces, never under this text: on
    // it the palette's dimmest name measured 1.02:1 -- invisible -- and the best
    // of the four roles 3.70:1, all under the 4.5:1 floor (WCAG 2.2, measured in
    // tests/theme_contrast.cpp, which holds every boxed preset to it). #17181c
    // sits in the family of Discord's own chat surfaces, so the overlay still
    // reads as a piece of that conversation; the blurple is demoted to an accent,
    // which is also what the application's own icon had been claiming all along.
    // The default *opacity* below is zero, so out of the box this colour decides
    // the text ramp and nothing else -- it is what the opacity slider raises.
    uint32_t panel_colour = 0x17181c;
    // The same surface as the panel's: the two boxes are one hand's work, and
    // the blurple the message box used to be is demoted to the bar down its left
    // edge. Unlike the panel this box *draws* its surface by default -- a
    // message arrives, is read once and leaves, and text that has to be read on
    // the first glance gets a box behind it.
    uint32_t notification_colour = 0x17181c;
    // The ring drawn around whoever is speaking.
    uint32_t speaking_colour = 0x23a55a;     // Discord's green

    // The three text colours a user may pin: an idle name, the speaking name,
    // and the message's text. kColourAuto -- the default -- keeps theme.h's
    // automatism, where the ramp is chosen by the box's own lightness; a picked
    // value overrides that one role on whatever box is set. Asked for by the
    // owner against a real scene: at the preview's true-to-display scale the
    // toast body's grey renders under 50% pixel coverage on the dark box and
    // reads as nothing, and the user has the last word about legibility.
    static constexpr uint32_t kColourAuto = 0xffffffffu;
    uint32_t text_idle_colour = kColourAuto;
    uint32_t text_speaking_colour = kColourAuto;
    uint32_t notification_text_colour = kColourAuto;

    // 88%, and it is the pill behind each name that is drawn at it, not a box
    // around the whole panel (panel_box above). It was zero -- names straight on
    // the game, the lightest possible presence -- and that traded the measured
    // floor away everywhere: the composite tables in tests/theme_contrast.cpp
    // are about this surface at this opacity, and now they are about what the
    // default actually draws under its text. The whole picture stays the
    // player's, because the surface only goes where the words are.
    float opacity = 0.88f;        // voice panel background opacity

    // Below this the background is effectively gone and only the text is left. A
    // legitimate choice, but one the interface has to say out loud: an opacity that
    // reached zero by accident used to look exactly like a broken overlay.
    static constexpr float kFaintBackground = 0.15f;

    // Half again as large as the text line. At 1.0 the picture is the height of a
    // line of text, which is small enough that a face is not recognisable at a
    // glance -- which is the only thing an avatar is there for.
    float avatar_size = 1.5f;     // multiplier on the avatar diameter

    // How strongly the picture of somebody who is not talking shows. Their name
    // has always been greyed (theme.h's text_idle) while their face stayed at
    // full strength, which is not what Discord's own overlay does: it quiets
    // the picture with the name. 55% because that is the step the name takes
    // on the default palette -- the idle grey #b5bac1 carries 0.488 of the
    // light the speaking #f2f3f5 carries (0.896), 54%, and over a dark scene a
    // picture at alpha a carries a of its own. 1.0 is every picture lit, which
    // is what every version up to 0.1.9 drew. Not below 10%: a picture at zero
    // is not drawn at all (ImGui culls alpha 0), a row whose face has gone
    // reads as a broken avatar, and hiding whoever is quiet is only_speaking.
    float avatar_idle_opacity = 0.55f;

    // Whether the text carries its outline -- permanent while on, not a remedy
    // that fades in when the background thins (see kTextOutlineStrength in
    // theme.h). Off by default on the owner's judgement: the default overlay is
    // as light as it can be, clean glyphs straight on the game, and whoever
    // finds their scene washing the names out has this switch and the preset
    // row. The INI key keeps its old name so files written before the outline
    // existed keep their answer.
    bool text_shadow = false;
    // Off by default, same judgement: out of the box the overlay is the people,
    // not the room they are in. The channel name remains one switch away.
    bool show_channel_name = false;

    // Notifications. Direct messages and mentions appear as a toast in one of the
    // four corners -- corners rather than free placement because a toast comes and
    // goes, and something that appears in an unexpected spot is worse than something
    // that always appears in the same one.
    bool notifications_enabled = true;
    int notification_corner = 1;      // 0 TL, 1 TR, 2 BL, 3 BR
    float notification_seconds = 5.0f;
    // Its own opacity, separate from the panel's, and solid by default. The two
    // boxes are read differently: the voice panel is furniture and the default
    // draws none of it, while a message is there to be read once, quickly, over
    // whatever happens to be behind it. Still adjustable, because someone who
    // wants it to sit lighter should be able to say so. (This said 94% for as
    // long as it took somebody to change the value and not the sentence.)
    float notification_opacity = 1.0f;
    // The message's own size, so it can be readable without making the voice panel
    // large as well.
    float notification_scale = 1.0f;
    // The message's text has no setting: it is always drawn. What used to be a
    // switch (off by default, so the feature was off) is a transport instead --
    // the words live in their own segment, created when a message arrives,
    // opened only by a process that is about to draw that toast, and removed
    // when the toast ends (vocem/note.h). A file written by a version that had
    // the switch still parses: unknown keys are ignored, which is the rule this
    // format has always had.

    // Spacing, in pixels at the reference text size of 16, scaled with everything
    // else at draw time. These are the numbers that were constants in the drawing
    // code: they are settings now because "a bit further from the edge" and "the
    // names are too close to the pictures" are real complaints with no other
    // answer.
    float screen_margin = 16.0f;    // the panel, from the edge of the display
    // The message's own distance from the edge, with exactly the panel's
    // behaviour: the anchor marks in the window move with it, and the box is
    // placed by the same arithmetic (vocem/placement.h). One setting each, because
    // the two boxes are positioned independently and a slider that moved both was
    // the shape of entry 17 in reverse.
    float notification_margin = 16.0f;
    float box_padding_x = 4.0f;     // inside either box, left and right
    float box_padding_y = 4.0f;     // inside either box, top and bottom
    float avatar_gap = 4.0f;        // between a picture and the name beside it
    float row_spacing = 0.0f;       // between one person and the next

    // The height of a line of text, in the same units. Separate from the panel's
    // size, which scales the whole box: this changes the text against the pictures
    // and the padding rather than with them, and is the setting for a panel whose
    // names are too small to read at a glance without everything else growing too.
    //
    // It is the reference the rest of the layout is expressed in, so the overlay
    // divides by it rather than by a constant -- see ui_scale() in fonts.h.
    float font_size = 16.0f;

    // The typeface. Empty -- the default -- is the carried Inter, which is what
    // every version before this one drew and the only thing that needs no
    // machine to be true of it.
    //
    // The *window* resolves a family to files and writes both here; the injected
    // code only ever opens a path. That split is the whole design: a game's
    // process may not ask fontconfig anything (a library, a cache, a config
    // parse and a handful of file syscalls, inside somebody else's renderer),
    // while the window already has Qt and the desktop's font machinery loaded.
    // `font_family` is the name the window shows; the two paths are what the
    // overlay reads, and a file that has gone away since it was chosen falls
    // back to Inter and says so in the log.
    std::string font_family;
    std::string font_path;
    std::string font_path_strong;

    // Voice
    bool only_speaking = false;   // hide participants who are silent
    bool hide_self = false;
    bool show_muted_state = true;

    // Behaviour. Two switches rather than one master: the voice panel and the
    // messages are separate features, and wanting one without the other is the
    // common case rather than an edge case.
    bool panel_enabled = true;
    bool enabled = true;          // master switch, honoured by the injected code

    // What the configuration window does with itself. Neither is read by the
    // injected code -- they are here because this is where the user's answers
    // live, and a second settings file for two switches would be one too many.
    //
    // Closing the window puts it away behind the tray icon by default: the overlay
    // is set up once and left running, and quitting it from the window is not
    // usually what a close button means. Somebody who wants the close button to
    // mean quit can say so.
    bool keep_running = true;
    // Whether the window starts with the session, hidden, so the tray icon is there
    // from login. Written out as a desktop entry under $XDG_CONFIG_HOME/autostart,
    // which is the freedesktop mechanism for it; this is the intent, that file is
    // the effect.
    bool start_at_login = false;

    // Whether the tray icon is the user's own voice state -- speaking, muted,
    // deafened, in a channel saying nothing, not in one -- or simply the
    // application's own picture.
    //
    // On by default, because the state is the reason there is an icon at all: a
    // glance at the panel answers "am I still muted" without opening anything.
    // The other answer is for somebody who wants their tray to look like every
    // other application in it, or who does not want their voice state legible
    // over their shoulder or on a stream.
    bool tray_voice_icon = true;

    // Applications the user has asked for the overlay in even though the detection
    // cannot tell they are games: something launched from a script or a terminal, an
    // emulator started by hand, a binary that came from nowhere in particular.
    std::string shown_apps;

    // Applications the overlay must stay out of although it can tell they are games.
    // Comma separated, matched against the process name or the executable's --
    // whichever the window showed.
    //
    // Both are empty by default, because the default is the detection, and a
    // settings file should be a record of decisions rather than a census of the
    // machine.
    //
    // It began as `gl_blacklist`, an OpenGL-only list carrying a dozen names --
    // `steam`, `minecraft-launcher`, `discord`, the browsers, plasmashell, kwin --
    // because the overlay drew everywhere except that list, and preloading a
    // launcher is often the only way to reach the game it starts. Those names are
    // gone from the default twice over: the detection leaves anything that is not a
    // game alone without being told (of that list only Steam and the two Minecraft
    // launchers have a `Categories=Game` entry at all), and the ones that do need
    // taking back are in `is_launcher()` in `apps.h`, in code, where a default that
    // can be edited away would have been a guarantee that is not one.
    //
    // Files written by the version that called this `gl_blacklist` are still read,
    // so a list somebody added to survives.
    std::string hidden_apps;

    // Flatpak application ids the daemon may serve the voice channel to, beside
    // the ones whose exported desktop entry says Game. Comma separated, the same
    // list syntax as the two above, matched against the id exactly
    // (`org.example.Game`). Read by the daemon alone: a sandbox's own request
    // is its word, and this is the user's (daemon/src/flatpak_bridge.cpp).
    std::string flatpak_apps;

    // Which display each map in the window depicts, as the connector's name
    // ("DP-2", "HDMI-A-1" -- the card<N>- prefix stripped, as the window lists
    // them). Empty -- the default -- means automatic: the largest connected
    // display, which is the one the overlay is sized for. Only the window reads
    // these; the injected code sizes from the daemon's published height and has
    // no use for a preview preference. One key per map, because the panel and
    // the messages are positioned independently and may live on different
    // screens.
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

    // Returns the file's modification time in nanoseconds, or 0 when it does not
    // exist. Used to decide whether a reload is worth doing. Nanoseconds because
    // whole seconds let two writes inside one second look like one: a poll landing
    // between them adopted the second write's stamp without reading it, and the
    // edit stayed invisible until the file moved again. `long long` and not
    // `long`, because this header is compiled into 32-bit games where `long` is
    // thirty-two bits (entries 30/33/34's width lesson).
    static long long mtime() {
        struct stat info{};
        if (stat(path().c_str(), &info) != 0) {
            return 0;
        }
        return static_cast<long long>(info.st_mtim.tv_sec) * 1000000000LL +
               static_cast<long long>(info.st_mtim.tv_nsec);
    }

    // Every numeric setting, with its bounds and the precision it is written at:
    // the one place the file's bounds are stated. load() clamps by this table,
    // the window's setters clamp by it (Config::clamped), and
    // tests/slider_bounds.cmake holds every slider on every page to it -- the
    // setters used to carry their own copies of these numbers, three of them
    // with no bound at all, which is the drift entry 121 found in the sliders
    // one file over (entry 136).
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

    // One of the three switches the window writes the moment it is clicked,
    // put on top of the file AS IT STANDS ON DISK: loaded fresh, that one key
    // changed, saved. The window used to write them over the copy it had
    // loaded at startup -- for a tray application started at login, a copy
    // from the morning -- so every key edited by hand or by a script since then
    // was silently put back the next time the overlay was switched off and on
    // (entry 136). And it used to write all THREE, the two nobody clicked
    // taken from the window's copy, so a switch turned off outside the window
    // was turned back on by a click on another one (entry 203). `which` is the
    // member (&Config::enabled, &Config::panel_enabled or
    // &Config::notifications_enabled); `written`, when given, receives what was
    // saved.
    static bool write_switch(bool Config::*which, bool value, Config* written = nullptr) {
        Config fresh;
        fresh.load();
        fresh.*which = value;
        const bool saved = fresh.save();
        if (written) {
            *written = fresh;
        }
        return saved;
    }

    void load() {
        std::FILE* file = std::fopen(path().c_str(), "r");
        if (!file) {
            return;  // defaults are a perfectly good configuration
        }
        std::string line;
        while (read_line(file, line)) {
            // `data()` is non-const from C++17, and the parsing below writes a
            // terminator over the '=' exactly as it did to a char array.
            char* text = line.data();
            char* equals = std::strchr(text, '=');
            if (!equals || text[0] == '#' || text[0] == '[' || text[0] == ';') {
                continue;
            }
            *equals = '\0';
            assign(trim(text), trim(equals + 1));
        }
        std::fclose(file);
    }

    // One setting, from the text a file line carries for it (already trimmed).
    // An unknown key changes nothing. load() reads every line through this, and
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
            // Retired, and deliberately not migrated. It was a switch between
            // two fixed colours, and both boxes now have a colour of their own;
            // carrying its "off" value across would start people on the
            // near-black without their having chosen it, in a version where the
            // choice is finally visible. Anyone who wants that colour can pick
            // it, and the default stays what the Discord client looks like.
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
            // A number 0-3, or the current value kept: atoi() answers 0 for
            // a word, and 0 is top-left, so a typo used to move the box to
            // a corner nobody chose -- unlike panel_layout and panel_box,
            // whose readers keep the value on a typo. Same rule now.
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
    // window shows, `base` is the file as it was when that copy last followed
    // it, `disk` is the file now. A setting the window changed (its text in
    // `mine` is not its text in `base`) keeps the window's value; every other
    // one takes the file's. The window saves this on Apply rather than
    // `mine`: `mine` does not follow the file while an edit waits for Apply,
    // so saving it put back every key changed outside the window since then --
    // flatpak_apps, which the README says to edit by hand and the window has
    // no control for, among them. Compared as the text the file would carry,
    // which is the precision a value has once written.
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

    // Only the GUI writes; the injected code never touches the file.
    //
    // The file is the user's, and the window rewrites it IN PLACE: each line
    // carrying a key the window knows gets that key's value, every other line
    // -- comments, blank lines, sections, keys from a newer version -- stays
    // where it was, and the known keys the file lacked are added at the end of
    // their section. It used to print a fresh file of its own and rename it over
    // config.ini, which, measured on 0.1.10 with one click on a switch, turned a
    // symlinked config.ini into a regular file (the target kept the old values
    // and stopped reaching the overlay), deleted every comment and an unknown
    // `future_key = 42` -- a newer version's setting, which DESIGN "Settings"
    // promises survives an older reader -- and put a 0600 file back at 0644.
    //
    // Written to a temporary file beside the TARGET and renamed over it: a game
    // reloads on the file's modification time, and truncating the real file
    // would let it read a half-written one -- which is worse than it sounds,
    // because the mtime would already be its final value and the bad read
    // would stick until the next save. A symlink is resolved first, so the
    // rename replaces the file it points at and the link stays a link -- one
    // whose target does not exist yet too (write_target); the
    // temporary takes the target's mode before it is renamed, and it is
    // fsync'd, so a crash leaves the old file or the new one.
    bool save() const {
        const std::string file_path = path();
        const size_t slash = file_path.rfind('/');
        if (slash != std::string::npos) {
            make_directories(file_path.substr(0, slash));
        }
        const std::string target = write_target(file_path);
        if (target.empty()) {
            return false;  // a loop of links: there is no file to write
        }
        struct stat existing{};
        const bool exists = ::stat(target.c_str(), &existing) == 0;
        // A file that is there and cannot be read is not an empty one: taken
        // for "no file", its contents were replaced by a fresh file of this
        // copy's values and its mode (0200, the case measured) carried over.
        std::string current;
        if (exists && !read_whole(target, current)) {
            return false;
        }
        const std::string text = rewritten(current);

        const std::string temporary_path = target + ".tmp";
        int fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0 && errno == EEXIST) {
            // Left by a write that died before its rename.
            ::unlink(temporary_path.c_str());
            fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        }
        if (fd < 0) {
            return false;
        }
        bool written = !exists || ::fchmod(fd, existing.st_mode & 07777) == 0;
        for (size_t done = 0; written && done < text.size();) {
            const ssize_t wrote = ::write(fd, text.data() + done, text.size() - done);
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            written = wrote > 0;
            done += written ? static_cast<size_t>(wrote) : 0;
        }
        written = written && ::fsync(fd) == 0;
        written = ::close(fd) == 0 && written;
        if (!written || std::rename(temporary_path.c_str(), target.c_str()) != 0) {
            ::unlink(temporary_path.c_str());
            return false;
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

    // `current` -- the file as it stands, possibly empty -- with this
    // configuration written into it: see save(). Public so a test can hold
    // the merge to its promise without a file.
    std::string rewritten(const std::string& current) const {
        static const char* const kHeader = "# Written by vocem-config. Edits are picked up live.";
        const std::vector<Entry> wanted = entries();
        std::vector<bool> placed(wanted.size(), false);
        const auto index_of = [&wanted](const std::string& key) -> int {
            // The name this setting had before 0.1.0 is the same setting.
            const std::string name = key == "gl_blacklist" ? std::string("hidden_apps") : key;
            for (size_t i = 0; i < wanted.size(); ++i) {
                if (name == wanted[i].key) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        const auto line_for = [](const Entry& entry) {
            return std::string(entry.key) + " = " + entry.value;
        };

        // The lines, with the section each belongs to.
        std::vector<std::string> lines;
        std::vector<std::string> sections;
        std::string section;
        for (size_t at = 0; at < current.size();) {
            size_t end = current.find('\n', at);
            if (end == std::string::npos) {
                end = current.size();
            }
            std::string line = current.substr(at, end - at);
            at = end + 1;
            // The same reading load() gives the line: a key is text before an
            // '=' on a line that does not open with '#', '[' or ';'.
            std::string trimmed = trim_copy(line);
            if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
                section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            } else if (!line.empty() && line[0] != '#' && line[0] != '[' && line[0] != ';') {
                const size_t equals = line.find('=');
                if (equals != std::string::npos) {
                    const int known = index_of(trim_copy(line.substr(0, equals)));
                    if (known >= 0) {
                        if (placed[static_cast<size_t>(known)]) {
                            // load() takes the LAST of two, so a second copy
                            // left behind would overrule the value written.
                            continue;
                        }
                        placed[static_cast<size_t>(known)] = true;
                        line = line_for(wanted[static_cast<size_t>(known)]);
                    }
                }
            }
            lines.push_back(line);
            sections.push_back(section);
        }

        // The keys the file lacked: after the last line of their section that
        // is not blank, or in a section of their own at the end.
        std::vector<std::vector<std::string>> after(lines.size() + 1);
        std::vector<std::string> tail;
        for (size_t i = 0; i < wanted.size(); ++i) {
            if (placed[i]) {
                continue;
            }
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
                text += '\n';
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

    // The same question about the message box, which carries an opacity of its
    // own: the two boxes are set independently, so the sentence the window puts
    // under each slider has to come from that box's own value. Its slider used
    // to stop at 20%, which meant the message box was the one surface in the
    // overlay a user could not turn off -- the answer to "I want the words and
    // nothing else" was a setting that refused to go there.
    bool notification_background_is_faint() const {
        return notification_opacity < kFaintBackground;
    }

private:
    // The longest single line this reader will keep. A settings file is the
    // user's own, so this is not a defence against a hostile one; it is the
    // bound that stops a line of any length turning into memory inside
    // somebody's game, which is where this header is compiled. 64 KiB is about
    // four thousand process names, and the lists are one entry per application
    // the user has decided about.
    static constexpr size_t kMaxLineBytes = 64 * 1024;

    // Reads one whole line, however long it is.
    //
    // This used to be `char line[256]` and a bare `fgets`, which takes 255
    // bytes and stops. The rest of a long line came back on the next iteration,
    // had no '=' in it, and was skipped -- so nothing errored, nothing was
    // logged, and the value was simply short. Two consequences, and the second
    // is the one that cost something: the injected code got a truncated
    // `hidden_apps`, so the applications the user hid last were given the
    // overlay again; and the settings window reads this same header, so the
    // next Apply wrote the truncation back and those entries were gone from the
    // file for good, without anyone touching that page. The last surviving name
    // was cut mid-word, so the list also gained an entry that is no
    // application. Measured on a 330-byte list: 241 bytes came back, ending
    // "...SomeGameBinary14,SomeGameBina", and the round trip kept them.
    // `font_path`, `font_path_strong` and `shown_apps` share the same road.
    //
    // A line past the cap is consumed to its end and dropped rather than kept
    // in pieces: half a value is not a value, and a caller reading a truncated
    // list would act on it.
    static bool read_line(std::FILE* file, std::string& line) {
        line.clear();
        char chunk[256];
        bool any = false;
        bool dropped = false;
        while (std::fgets(chunk, sizeof(chunk), file)) {
            any = true;
            const size_t length = std::strlen(chunk);
            if (!dropped && line.size() + length > kMaxLineBytes) {
                dropped = true;
                line.clear();
            }
            if (!dropped) {
                line.append(chunk, length);
            }
            // `fgets` stops at the newline or at the buffer; only the newline
            // ends the line.
            if (length > 0 && chunk[length - 1] == '\n') {
                break;
            }
        }
        return any;
    }

    // The whole file, however long its lines: the rewrite keeps every line
    // it does not own, and read_line's cap is for the reader inside a game.
    // The file a write to `file_path` must land on: the path itself, or the
    // end of the links it is. realpath() answers that for a link whose target
    // exists and fails for one whose target does not -- dotfiles linked in
    // before the file was first written -- and the rename then put a regular
    // file where the link was. So a link realpath() cannot resolve is followed
    // here, a relative target against the link's own directory; empty for a
    // chain that does not end (a loop), which save() refuses.
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

    // False when the file cannot be opened or a read fails part way: the
    // caller must not take that for an empty file.
    static bool read_whole(const std::string& file_path, std::string& text) {
        text.clear();
        std::FILE* file = std::fopen(file_path.c_str(), "r");
        if (!file) {
            return false;
        }
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            text.append(chunk, got);
        }
        const bool whole = !std::ferror(file);
        std::fclose(file);
        return whole;
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
        // A quoted value loses both quotes, not only the closing one: a
        // `font_path = "/a b/x.ttf"` written by hand used to come back as
        // `"/a b/x.ttf` and the font quietly fell back to Inter.
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

    // Decimal parsing that ignores LC_NUMERIC entirely, rather than trusting every
    // process that reads this file to be in the same locale as the one that wrote it.
    // Both separators are accepted on the way in: files already written with a comma
    // keep their values instead of quietly resetting to zero. Exponents are not
    // recognised, because no setting here is ever written in that form.
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

    // true/yes/on/1, in any case: `enabled = True` -- which a hand or a script
    // plausibly writes -- switched the overlay OFF.
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
