// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// theme_for() and its two helpers: turning a Config into a Theme. The tokens,
// presets and inline arithmetic are in vocem/theme.h. Pure and allocation-free.

#include "vocem/theme.h"

namespace vocem {

// tests/panel_geometry.cpp finds every shape by its colour within a draw list,
// so two roles on one value make a plausible wrong figure (distinct_tokens).
// The blue channel walks outward one step at a time until nothing collides: at
// most a few hundred comparisons against a dozen tokens, on the stack.
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

// A blend rather than plain white: a stark silhouette is louder than a picture
// not yet arrived deserves, and white is `toast_title`, which the geometry test
// would confuse it with. Rec. 709 luma.
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

// The palette is Discord's for the parts that are not configurable: the
// overlay stands in for the Discord client.
Theme theme_for(const Config& config) {
    Theme theme;

    const bool light_panel = is_light(config.panel_colour);
    const bool light_toast = is_light(config.notification_colour);
    // With pills there is no panel edge and no line under the channel name.
    const bool names_box = config.panel_box == Config::kBoxNames;

    theme.panel_surface = hex(config.panel_colour);
    theme.toast_surface = hex(config.notification_colour);
    theme.speaking_ring = hex(config.speaking_colour);

    // Two palettes, chosen by the box they sit on: picking a pale box is not
    // asking to pick the text colours too.
    if (light_panel) {
        theme.text_channel = hex(0x3c3e44);
        theme.text_speaking = hex(0x18191c);
        theme.text_idle = hex(0x5a5c62);
        // The lightest grey that clears 4.5:1 on a pale box at 88% over snow
        // (4.8:1) while staying under the idle grey (5.9:1), so the ordering
        // survives greyscale.
        theme.text_muted = hex(0x66696f);
        theme.text_overflow = hex(0x6e7076);
    } else {
        // Chosen against the surface they sit on, composited over the scene
        // (WCAG 2.2, tests/theme_contrast.cpp): at the default surface and
        // opacity the worst case, a muted name over snow, is 4.9:1 and the
        // channel name 12.8:1. Speaking / idle / muted stay ordered in greyscale.
        theme.text_channel = hex(0xffffff);
        theme.text_speaking = hex(0xf2f3f5);
        theme.text_idle = hex(0xb5bac1);
        theme.text_muted = hex(0x9aa0ab);
        // The idle grey to the eye, one part off it for the geometry test, which
        // finds the "+N more" line by colour.
        theme.text_overflow = hex(0xb5bac0);
    }

    // The body is its palette's idle grey under a full-strength title; the
    // title is also drawn in the heavier Inter (panel.cpp). Both clear the 4.5:1
    // floor on their default surface (tests/theme_contrast.cpp) and collide with
    // no other role in the toast's draw list.
    if (light_toast) {
        theme.toast_title = hex(0x18191c);
        theme.toast_body = hex(0x5a5c62);
    } else {
        theme.toast_title = hex(0xffffff);
        theme.toast_body = hex(0xb5bac1);
    }

    // The toast's edge and accent, premultiplied by the message box's opacity
    // as the panel's hairline is by the panel's, so a transparent box leaves no
    // floating bar. The fade-out multiplies on top, at the draw.
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
    // The surface at full opacity, not the panel's: the one band around the
    // glyph that is always the box's colour.
    theme.badge_rim = hex(config.panel_colour);
    // 0xfffffe, not white: the channel name is white in the same draw list.
    theme.badge_glyph = hex(0xfffffe);

    // The line under the channel name: a hairline like the panel's edge (pale at
    // 10% on a dark box, dark on a pale one) and the blurple accent bar in front
    // of it. 0xfdfdfd / 0x020202 because 0xffffff and 0xfefefe / 0x010101 are
    // taken in the same draw list. With pills there is no box for a line to
    // cross, so both segments get alpha zero here rather than a condition at the
    // draw: ImGui and Qt both cull it, and the line still claims its unit of the
    // column so the two modes lay out identically.
    const float line_alpha = names_box ? 0.0f : 1.0f;
    theme.separator = light_panel ? hex(0x020202, static_cast<uint8_t>(38 * line_alpha))
                                  : hex(0xfdfdfd, static_cast<uint8_t>(26 * line_alpha));
    theme.separator_accent = hex(0x5865f2, static_cast<uint8_t>(255 * line_alpha));

    // The hairline follows the box's opacity, premultiplied here so the
    // drawing, the preview and the measurement agree: a faded panel takes its
    // edge with it, and pills have no edge at all. 20 is 8% of 255, an edge
    // highlight rather than a border.
    const auto premultiplied = [&](uint32_t rgb, float alpha) {
        const float opacity = names_box ? 0.0f : config.opacity;
        return hex(rgb, static_cast<uint8_t>(alpha * opacity + 0.5f));
    };
    theme.panel_hairline =
        light_panel ? premultiplied(0x010101, 20.0f) : premultiplied(0xfefefe, 20.0f);

    theme.panel_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;
    theme.toast_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;

    // The three colours a user may pin (config.h: kColourAuto keeps the ramps
    // above). A pinned value replaces its role exactly unless it lands on
    // another measured role in the same draw list, where step_off_taken() steps
    // it off. Applied last, so every token it must avoid is final; idle before
    // speaking, so a pair pinned to one colour still comes out as two. No
    // contrast promise: a pinned colour is the user's own choice. The counts come
    // from the arrays, so a role added to a list cannot be silently landed on.
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
