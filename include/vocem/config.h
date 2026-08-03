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

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    // Zero: the default overlay is the names straight on the game, no box at
    // all -- the owner's judgement, preferring the lightest possible presence
    // over the measured floor a surface can promise. The floor is not lost, it
    // moves: it stays asserted on every preset surface, one chip away in
    // Appearance, and the Dark chip is the 88% box the composite tables in
    // tests/theme_contrast.cpp are about.
    float opacity = 0.0f;         // voice panel background opacity

    // Below this the background is effectively gone and only the text is left. A
    // legitimate choice, but one the interface has to say out loud: an opacity that
    // reached zero by accident used to look exactly like a broken overlay.
    static constexpr float kFaintBackground = 0.15f;

    // Half again as large as the text line. At 1.0 the picture is the height of a
    // line of text, which is small enough that a face is not recognisable at a
    // glance -- which is the only thing an avatar is there for.
    float avatar_size = 1.5f;     // multiplier on the avatar diameter

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
    // is a thing you set up once and leave running, and quitting it from the window
    // is not usually what a close button means. Somebody who wants the close button
    // to mean quit can say so.
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

    static std::string path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            return std::string(xdg) + "/vocem/config.ini";
        }
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.config/vocem/config.ini";
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

    void load() {
        std::FILE* file = std::fopen(path().c_str(), "r");
        if (!file) {
            return;  // defaults are a perfectly good configuration
        }
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            char* equals = std::strchr(line, '=');
            if (!equals || line[0] == '#' || line[0] == '[' || line[0] == ';') {
                continue;
            }
            *equals = '\0';
            const char* key = trim(line);
            const char* value = trim(equals + 1);

            if (std::strcmp(key, "position_x") == 0) {
                position_x = clamp(to_number(value), 0.0, 1.0);
            } else if (std::strcmp(key, "position_y") == 0) {
                position_y = clamp(to_number(value), 0.0, 1.0);
            } else if (std::strcmp(key, "scale") == 0) {
                scale = clamp(to_number(value), 0.5, 3.0);
            } else if (std::strcmp(key, "panel_colour") == 0) {
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
            } else if (std::strcmp(key, "opacity") == 0) {
                opacity = clamp(to_number(value), 0.0, 1.0);
            } else if (std::strcmp(key, "avatar_size") == 0) {
                avatar_size = clamp(to_number(value), 0.5, 2.0);
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
                const int parsed = std::atoi(value);
                notification_corner = (parsed >= 0 && parsed <= 3) ? parsed : 1;
            } else if (std::strcmp(key, "notification_seconds") == 0) {
                notification_seconds = clamp(to_number(value), 1.0, 30.0);
            } else if (std::strcmp(key, "notification_opacity") == 0) {
                notification_opacity = clamp(to_number(value), 0.0, 1.0);
            } else if (std::strcmp(key, "notification_scale") == 0) {
                notification_scale = clamp(to_number(value), 0.5, 3.0);
            } else if (std::strcmp(key, "screen_margin") == 0) {
                screen_margin = clamp(to_number(value), 0.0, 120.0);
            } else if (std::strcmp(key, "notification_margin") == 0) {
                notification_margin = clamp(to_number(value), 0.0, 120.0);
            } else if (std::strcmp(key, "font_size") == 0) {
                font_size = clamp(to_number(value), 8.0, 48.0);
            } else if (std::strcmp(key, "box_padding_x") == 0) {
                box_padding_x = clamp(to_number(value), 0.0, 48.0);
            } else if (std::strcmp(key, "box_padding_y") == 0) {
                box_padding_y = clamp(to_number(value), 0.0, 48.0);
            } else if (std::strcmp(key, "avatar_gap") == 0) {
                avatar_gap = clamp(to_number(value), 0.0, 48.0);
            } else if (std::strcmp(key, "row_spacing") == 0) {
                row_spacing = clamp(to_number(value), 0.0, 48.0);
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
            } else if (std::strcmp(key, "preview_display_panel") == 0) {
                preview_display_panel = value;
            } else if (std::strcmp(key, "preview_display_notification") == 0) {
                preview_display_notification = value;
            }
        }
        std::fclose(file);
    }

    // Only the GUI writes; the injected code never touches the file.
    bool save() const {
        const std::string file_path = path();
        const size_t slash = file_path.rfind('/');
        if (slash != std::string::npos) {
            make_directories(file_path.substr(0, slash));
        }

        // Written to a temporary file and renamed into place. A game reloads on the
        // file's modification time, and truncating the real file would let it read a
        // half-written one -- which is worse than it sounds, because the mtime would
        // already be its final value and the bad read would stick until the next save.
        const std::string temporary_path = file_path + ".tmp";
        std::FILE* file = std::fopen(temporary_path.c_str(), "w");
        if (!file) {
            return false;
        }
        std::fprintf(file, "# Written by vocem-config. Edits are picked up live.\n");
        std::fprintf(file, "[position]\n");
        std::fprintf(file, "position_x = %s\n", decimal(position_x, 4).c_str());
        std::fprintf(file, "position_y = %s\n", decimal(position_y, 4).c_str());
        std::fprintf(file, "\n[appearance]\n");
        std::fprintf(file, "scale = %s\n", decimal(scale, 2).c_str());
        std::fprintf(file, "panel_colour = %s\n", colour_text(panel_colour).c_str());
        std::fprintf(file, "speaking_colour = %s\n", colour_text(speaking_colour).c_str());
        std::fprintf(file, "text_idle_colour = %s\n",
                     colour_or_auto_text(text_idle_colour).c_str());
        std::fprintf(file, "text_speaking_colour = %s\n",
                     colour_or_auto_text(text_speaking_colour).c_str());
        std::fprintf(file, "opacity = %s\n", decimal(opacity, 2).c_str());
        std::fprintf(file, "avatar_size = %s\n", decimal(avatar_size, 2).c_str());
        std::fprintf(file, "text_shadow = %s\n", text_shadow ? "true" : "false");
        std::fprintf(file, "show_channel_name = %s\n", show_channel_name ? "true" : "false");
        std::fprintf(file, "\n[notifications]\n");
        std::fprintf(file, "notifications_enabled = %s\n",
                     notifications_enabled ? "true" : "false");
        std::fprintf(file, "notification_corner = %d\n", notification_corner);
        std::fprintf(file, "notification_seconds = %s\n", decimal(notification_seconds, 1).c_str());
        std::fprintf(file, "notification_opacity = %s\n", decimal(notification_opacity, 2).c_str());
        std::fprintf(file, "notification_colour = %s\n", colour_text(notification_colour).c_str());
        std::fprintf(file, "notification_text_colour = %s\n",
                     colour_or_auto_text(notification_text_colour).c_str());
        std::fprintf(file, "notification_scale = %s\n", decimal(notification_scale, 2).c_str());
        std::fprintf(file, "\n[spacing]\n");
        std::fprintf(file, "screen_margin = %s\n", decimal(screen_margin, 1).c_str());
        std::fprintf(file, "notification_margin = %s\n",
                     decimal(notification_margin, 1).c_str());
        std::fprintf(file, "font_size = %s\n", decimal(font_size, 1).c_str());
        std::fprintf(file, "box_padding_x = %s\n", decimal(box_padding_x, 1).c_str());
        std::fprintf(file, "box_padding_y = %s\n", decimal(box_padding_y, 1).c_str());
        std::fprintf(file, "avatar_gap = %s\n", decimal(avatar_gap, 1).c_str());
        std::fprintf(file, "row_spacing = %s\n", decimal(row_spacing, 1).c_str());
        std::fprintf(file, "\n[voice]\n");
        std::fprintf(file, "only_speaking = %s\n", only_speaking ? "true" : "false");
        std::fprintf(file, "hide_self = %s\n", hide_self ? "true" : "false");
        std::fprintf(file, "show_muted_state = %s\n", show_muted_state ? "true" : "false");
        std::fprintf(file, "\n[behaviour]\n");
        std::fprintf(file, "panel_enabled = %s\n", panel_enabled ? "true" : "false");
        std::fprintf(file, "enabled = %s\n", enabled ? "true" : "false");
        std::fprintf(file, "keep_running = %s\n", keep_running ? "true" : "false");
        std::fprintf(file, "start_at_login = %s\n", start_at_login ? "true" : "false");
        std::fprintf(file, "tray_voice_icon = %s\n", tray_voice_icon ? "true" : "false");
        std::fprintf(file, "hidden_apps = %s\n", hidden_apps.c_str());
        std::fprintf(file, "shown_apps = %s\n", shown_apps.c_str());
        std::fprintf(file, "preview_display_panel = %s\n", preview_display_panel.c_str());
        std::fprintf(file, "preview_display_notification = %s\n",
                     preview_display_notification.c_str());

        const bool written = std::fflush(file) == 0;
        std::fclose(file);
        if (!written || std::rename(temporary_path.c_str(), file_path.c_str()) != 0) {
            std::remove(temporary_path.c_str());
            return false;
        }
        return true;
    }

    // True when the background has been turned down far enough to disappear. The
    // interface uses it to explain what is about to happen rather than leaving the
    // user with a panel that looks broken.
    bool background_is_faint() const { return opacity < kFaintBackground; }

private:
    static const char* trim(char* text) {
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
        char* end = text + std::strlen(text);
        while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                              end[-1] == '\t' || end[-1] == '"')) {
            --end;
        }
        *end = '\0';
        return text;
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
        if (std::strncmp(text, "auto", 4) == 0) {
            return kColourAuto;
        }
        return to_colour(text, fallback);
    }

    static std::string colour_or_auto_text(uint32_t colour) {
        return colour == kColourAuto ? "auto" : colour_text(colour);
    }

    static bool as_bool(const char* value) {
        return std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0 ||
               std::strcmp(value, "yes") == 0;
    }

    static float clamp(double value, double low, double high) {
        if (value < low) return static_cast<float>(low);
        if (value > high) return static_cast<float>(high);
        return static_cast<float>(value);
    }
};

}  // namespace vocem

#endif  // VOCEM_CONFIG_H
