// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The palette, worked out from the settings: theme_for() and the two helpers it
// leans on. The tokens themselves, the presets and the arithmetic every consumer
// inlines (hex, is_light, decoration_allowance) stay in vocem/theme.h; this is
// the two hundred lines that turn a Config into a Theme, compiled once per width
// into vocem_common rather than into every file that reads a colour. Pure and
// allocation-free, as the header promises: plain arithmetic on the stack.

#include "vocem/theme.h"

namespace vocem {

// A pinned colour that lands exactly on another measured role's value, stepped
// off it by the smallest visible-to-nobody amount. tests/panel_geometry.cpp
// finds every shape by its colour within a draw list, so two roles on one value
// make a figure plausible and wrong (distinct_tokens is the assertion); the
// hairline's 0xfefefe and the badge glyph's 0xfffffe already pay one part in
// 255 for the same property, and a user's pick pays the same coin. The blue
// channel walks outward one step at a time until nothing collides -- pure
// arithmetic on the stack, at most a few hundred comparisons against a dozen
// tokens, so it stays within what a header the injected code compiles may do.
uint32_t step_off_taken(uint32_t rgb, const uint32_t* taken, int count) {
    const auto collides = [&](uint32_t value) {
        for (int i = 0; i < count; ++i) {
            if (taken[i] == value) {
                return true;
            }
        }
        return false;
    };
    if (!collides(rgb)) {
        return rgb;
    }
    const int blue = static_cast<int>(rgb & 0xff);
    for (int step = 1; step < 256; ++step) {
        for (int sign : {-1, 1}) {
            const int candidate = blue + sign * step;
            if (candidate < 0 || candidate > 255) {
                continue;
            }
            const uint32_t value = (rgb & 0xffff00u) | static_cast<uint32_t>(candidate);
            if (!collides(value)) {
                return value;
            }
        }
    }
    return rgb;  // Unreachable: there are far fewer roles than blues.
}

// The mark on a placeholder disc, from the disc's own colour: carried most of
// the way towards white on a dark disc, towards black on a pale one. Two
// reasons it is a blend rather than plain white, both from the drawing: a stark
// white silhouette is louder than a picture that has not arrived deserves to
// be, and plain white is exactly `toast_title`, so anything that finds shapes
// by colour would read the shoulders as part of the sender's name.
//
// Rec. 709 luma, the same weights the drawing used before this moved here.
Colour avatar_mark_for(Colour disc) {
    const float luminance = (0.2126f * static_cast<float>(disc.r) +
                             0.7152f * static_cast<float>(disc.g) +
                             0.0722f * static_cast<float>(disc.b)) / 255.0f;
    const float towards = luminance < 0.5f ? 255.0f : 0.0f;
    const auto blend = [towards](uint8_t channel) {
        const float value = static_cast<float>(channel);
        return static_cast<uint8_t>(value + (towards - value) * 0.45f + 0.5f);
    };
    return Colour{blend(disc.r), blend(disc.g), blend(disc.b), 255};
}

// The theme the user's settings come to. Pure: same settings, same answer, no
// allocation, no state.
//
// The palette is Discord's for the parts that are not configurable, because the
// thing this overlay stands in for is the Discord client and a participant list
// that looked like something else would be read as something else.
Theme theme_for(const Config& config) {
    Theme theme;

    const bool light_panel = is_light(config.panel_colour);
    const bool light_toast = is_light(config.notification_colour);
    // Where the surface goes decides two tokens as well as one shape: a panel
    // whose box is only behind the names has no edge and no line under the
    // channel name to draw.
    const bool names_box = config.panel_box == Config::kBoxNames;

    theme.panel_surface = hex(config.panel_colour);
    theme.toast_surface = hex(config.notification_colour);
    theme.speaking_ring = hex(config.speaking_colour);

    // Two palettes, chosen by the box they sit on rather than by a setting: a user
    // who picks a pale colour has said what the box should be, not that they would
    // like to pick the text colours too.
    if (light_panel) {
        theme.text_channel = hex(0x3c3e44);
        theme.text_speaking = hex(0x18191c);
        theme.text_idle = hex(0x5a5c62);
        // Not 0x96989e, which this was: measured on the palest surface offered --
        // Breeze Light's window at 88% over snow -- that grey read 2.5:1, and a
        // muted name that misses the floor is the exact defect this palette
        // replaced on the dark side. 0x66696f is the lightest step that clears
        // 4.5:1 there (4.8:1), while staying visibly under the idle grey (5.9:1)
        // so the ordering survives greyscale.
        theme.text_muted = hex(0x66696f);
        theme.text_overflow = hex(0x6e7076);
    } else {
        // Not Discord's greys any more. Those were drawn by Discord on a
        // near-black surface and arrived here on the blurple, where the dimmest of
        // them crossed under the box -- a muted name at 1.02:1, invisible. This
        // ramp is chosen against the surface it actually sits on, composited over
        // the scene behind it (WCAG 2.2, measured in tests/theme_contrast.cpp):
        // on the default surface at its default opacity the worst case -- a muted
        // name over snow -- reads 4.9:1, over the 4.5:1 floor, and the best is the
        // channel name at 12.8:1. The steps stay far enough apart that speaking /
        // idle / muted survive greyscale, which is the ordering the same test
        // asserts.
        theme.text_channel = hex(0xffffff);
        theme.text_speaking = hex(0xf2f3f5);
        theme.text_idle = hex(0xb5bac1);
        theme.text_muted = hex(0x9aa0ab);
        // The idle grey to the eye, one part in 255 off it to the measurement:
        // the "+N more" line's alignment is asserted by tests/panel_geometry.cpp,
        // and a shape is found there by its colour within a draw list.
        theme.text_overflow = hex(0xb5bac0);
    }

    // The sender and what the sender said are two roles, and they are told apart
    // by colour again: each palette's body is that palette's own idle grey --
    // the same grey the panel greys a name with -- under a title that keeps the
    // full-strength ink. The body was white for a while (one part in 255 under
    // the title, so the two were one colour to the eye), on the argument that
    // the weight alone carried the hierarchy; the owner's answer, looking at it
    // in a game, is that a message a shade quieter than the name above it is
    // what a message is. The weight still carries its half: the title is drawn
    // in the heavier Inter (panel.cpp), which no colour measurement can see.
    //
    // Both greys clear the 4.5:1 floor on their own default surface, which
    // tests/theme_contrast.cpp asserts rather than trusts, and neither collides
    // with another role in the toast's draw list (the panel's overflow grey,
    // 0xb5bac0, is a part away and lives in the other window's list).
    if (light_toast) {
        theme.toast_title = hex(0x18191c);
        theme.toast_body = hex(0x5a5c62);
    } else {
        theme.toast_title = hex(0xffffff);
        theme.toast_body = hex(0xb5bac1);
    }

    // The toast's edge and accent, both premultiplied by the message box's own
    // opacity, exactly as the panel's hairline is by the panel's: the bar is part
    // of the box, and a box turned down to nothing must not leave a full-strength
    // blurple bar floating on the game. The fade-out multiplies on top, at the
    // draw.
    const auto toast_premultiplied = [&](uint32_t rgb, float alpha) {
        return hex(rgb, static_cast<uint8_t>(alpha * config.notification_opacity + 0.5f));
    };
    theme.toast_hairline = light_toast ? toast_premultiplied(0x010101, 20.0f)
                                       : toast_premultiplied(0xfefefe, 20.0f);
    theme.toast_accent = toast_premultiplied(0x5865f2, 255.0f);

    theme.text_outline_ink = light_panel ? hex(0xffffff) : hex(0x000000);
    theme.avatar_placeholder = hex(0x4f545c);
    theme.avatar_mark = avatar_mark_for(theme.avatar_placeholder);
    theme.avatar_scrim = hex(0x000000, 110);
    theme.badge_fill = hex(0xf23f43);
    // The surface at full opacity, deliberately not at the panel's: the rim's job
    // is to be the one band around the glyph that is always the box's colour,
    // whatever the box lets through elsewhere.
    theme.badge_rim = hex(config.panel_colour);
    // 0xfffffe, not white: the channel name is pure white in the same draw list,
    // and the geometry test finds a shape by its colour. One part in 255 keeps
    // the two measurable separately -- same trade as the hairline's 0xfefefe.
    theme.badge_glyph = hex(0xfffffe);

    // The line under the channel name, chosen now rather than inherited. Phase 1
    // named ImGui's default (0x6e6e80 at half alpha) without changing a pixel,
    // exactly so that this change would happen where it could be seen: the
    // hairline is now the same treatment as the panel's edge -- pale at 10% on a
    // dark box, dark on a pale one -- and the accent bar in front of it is the
    // blurple, doing the one job it is allowed (decoration, never meaning).
    // 0xfdfdfd and 0x020202 rather than pure white and black: the channel name
    // owns 0xffffff and the edge hairlines own 0xfefefe / 0x010101 in the same
    // draw list, and the measurement tells shapes apart by colour.
    // Both segments carry the alpha of a line that belongs to a box: where the
    // surface is drawn behind the names only there is no box for a line to be
    // drawn across, and a full-width hairline floating on the game under a
    // pill is furniture with nothing to be furniture of. Zero rather than a
    // condition at the draw, so the drawing, the preview and the measurement
    // all learn it from the same place -- ImGui and Qt both cull an alpha of
    // zero, and the line still claims its unit of the column so the two modes
    // lay out identically.
    const float line_alpha = names_box ? 0.0f : 1.0f;
    theme.separator = light_panel ? hex(0x020202, static_cast<uint8_t>(38 * line_alpha))
                                  : hex(0xfdfdfd, static_cast<uint8_t>(26 * line_alpha));
    theme.separator_accent = hex(0x5865f2, static_cast<uint8_t>(255 * line_alpha));

    // The hairline follows the box's own opacity, premultiplied here rather than
    // at the draw: a panel faded to nothing must take its edge with it, and every
    // consumer of the token -- the drawing, the preview, the measurement -- has
    // to agree on that without re-deriving it. The 20 is 8% of 255: enough to
    // read as an edge highlight on the dark surface, not enough to read as a
    // border.
    // ... and by nothing at all where the surface is drawn behind the names
    // instead: the hairline is the edge of a box, and the pills have no edge of
    // their own. tests/panel_geometry.cpp reads the same token to decide whether
    // to expect a stroke, so this is the one place that has to know.
    const auto premultiplied = [&](uint32_t rgb, float alpha) {
        const float opacity = names_box ? 0.0f : config.opacity;
        return hex(rgb, static_cast<uint8_t>(alpha * opacity + 0.5f));
    };
    theme.panel_hairline =
        light_panel ? premultiplied(0x010101, 20.0f) : premultiplied(0xfefefe, 20.0f);

    theme.panel_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;
    theme.toast_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;

    // The three colours a user may pin (config.h: kColourAuto, the default, keeps
    // the ramps above). A pinned value replaces its role exactly -- except when
    // it lands on a colour another measured role in the same draw list already
    // wears, where it takes the one-part-in-255 step step_off_taken() explains.
    // Applied last, so every token it must stay clear of already has its final
    // value; idle before speaking, so a pair pinned to one colour still comes out
    // as two. What is deliberately not here: no contrast promise. A pinned text
    // colour is the user's own choice, like a pinned surface, and
    // tests/theme_contrast.cpp prints what an arbitrary choice measures rather
    // than promising a floor no palette can hold on it.
    // The counts are taken from the arrays rather than written beside them: a
    // role added to the list and not to the number would be a role a pinned
    // colour is allowed to land on, silently.
    if (config.text_idle_colour != Config::kColourAuto) {
        const uint32_t taken[] = {
            token_rgb(theme.avatar_placeholder), token_rgb(theme.avatar_mark),
            token_rgb(theme.badge_fill),         token_rgb(theme.text_channel),
            token_rgb(theme.text_speaking),      token_rgb(theme.text_muted),
            token_rgb(theme.separator),          token_rgb(theme.separator_accent),
            token_rgb(theme.text_overflow),      token_rgb(theme.panel_hairline),
            token_rgb(theme.badge_glyph)};
        theme.text_idle =
            hex(step_off_taken(config.text_idle_colour, taken, sizeof(taken) / sizeof(taken[0])));
    }
    if (config.text_speaking_colour != Config::kColourAuto) {
        const uint32_t taken[] = {
            token_rgb(theme.avatar_placeholder), token_rgb(theme.avatar_mark),
            token_rgb(theme.badge_fill),         token_rgb(theme.text_channel),
            token_rgb(theme.text_idle),          token_rgb(theme.text_muted),
            token_rgb(theme.separator),          token_rgb(theme.separator_accent),
            token_rgb(theme.text_overflow),      token_rgb(theme.panel_hairline),
            token_rgb(theme.badge_glyph)};
        theme.text_speaking = hex(
            step_off_taken(config.text_speaking_colour, taken, sizeof(taken) / sizeof(taken[0])));
    }
    if (config.notification_text_colour != Config::kColourAuto) {
        const uint32_t taken[] = {token_rgb(theme.avatar_placeholder),
                                  token_rgb(theme.avatar_mark), token_rgb(theme.toast_title),
                                  token_rgb(theme.toast_hairline), token_rgb(theme.toast_accent)};
        theme.toast_body = hex(step_off_taken(config.notification_text_colour, taken,
                                              sizeof(taken) / sizeof(taken[0])));
    }

    return theme;
}

}  // namespace vocem
